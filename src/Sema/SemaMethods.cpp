//===--- SemaMethods.cpp - Methods declared inside a struct body --------===//
//
// A method is not a new kind of function. It is an ordinary function whose first
// parameter is the receiver and whose name is mangled to `Point.length`, so
// everything after this file -- overload resolution, literal adaptation,
// generics, IRGen, the calling convention -- sees a plain function and needs to
// know nothing about methods. That is the whole trick, and it is why adding
// methods cost no changes to IRGen or the VM.
//
// ── the receiver is written as a bare type ────────────────────────────────
//
//   function length(self):int              // by value: read-only view
//   function scale(ptr self, int by):void  // by pointer: may write
//
// There is no useful name to give it (the body always says `self`), so it is
// written without one, and the missing name is exactly what distinguishes a
// receiver from an ordinary parameter. Only the first parameter of a method may
// omit its name, so no existing function can be reinterpreted as a method.
//
// `ptr self` versus `self` is the whole mutability story: the value receiver is
// marked const, so `self.x = 1` is an error rather than a write to a copy that
// the caller never sees. Axiom has no `const` methods or reference qualifiers
// because it does not need them -- the parameter type already says it.
//
// ── static methods ────────────────────────────────────────────────────────
//
// A method with no receiver parameter is static: `Point.origin()` rather than
// `p.origin()`. This exists because Axiom has no constructors, and a library
// needs a conventional way to say "make one of these".

#include "acvm/Common/Diagnostic.h"
#include "acvm/Sema/Sema.h"

namespace acvm {

namespace {

/// The receiver is `self` or `ptr self`, and nothing else. Recognising it by
/// spelling rather than by any new keyword keeps `self` an ordinary identifier
/// everywhere else -- including in existing code, where it is used as a word in
/// comments and strings but never as a variable.
bool isSelfTypeNode(const TypeNode& node) {
    if (node.kind == TypeNode::Kind::Named) {
        return node.name == "self";
    }
    if (node.kind == TypeNode::Kind::Ptr && node.inner != nullptr) {
        return isSelfTypeNode(*node.inner);
    }
    return false;
}

/// True when the receiver is taken by pointer, i.e. `ptr self`.
bool isPtrSelfTypeNode(const TypeNode& node) {
    return node.kind == TypeNode::Kind::Ptr && node.inner != nullptr &&
           isSelfTypeNode(*node.inner);
}

} // namespace

void Sema::declareMethods(Stmt& structDecl, Scope& scope) {
    const Type* owner = structDecl.structType;
    if (owner == nullptr) {
        return;
    }

    for (StmtPtr& methodPtr : structDecl.methods) {
        Stmt& decl = *methodPtr;
        const std::string sourceName = decl.name;

        if (decl.isForward) {
            m_diagnostics.error(decl.nameRange,
                                "method '" + sourceName +
                                    "' has no body; a method is compiled like any other function, "
                                    "so its body has to be available. If the type belongs to a "
                                    "native library, declare the struct in the matching .ax file "
                                    "(methods included) and let the .axd refer to 'ptr " +
                                    owner->describe() + "'");
            continue;
        }
        if (!decl.externalLibrary.empty()) {
            // The VM looks a native function up by its name, and the mangled name
            // contains a '.', which is not a valid C identifier. Rather than let
            // the link fail at run time with a symbol that could never exist, say
            // what to do instead.
            m_diagnostics.error(decl.nameRange,
                                "method '" + sourceName + "' cannot be implemented in library '" +
                                    decl.externalLibrary +
                                    "'; its symbol would have to be named '" + owner->describe() +
                                    "." + sourceName +
                                    "', which is not a valid C identifier. Write a free function "
                                    "declared in the library and call it from an Axiom method");
            continue;
        }
        if (decl.funcKind == FuncKind::Global) {
            m_diagnostics.error(decl.nameRange, "a method cannot be 'global'");
            continue;
        }
        if (decl.funcKind == FuncKind::Event || decl.funcKind == FuncKind::Process) {
            m_diagnostics.error(decl.nameRange,
                                "a method cannot be '" +
                                    std::string(decl.funcKind == FuncKind::Event ? "event"
                                                                                : "process") +
                                    "'; the scheduler resolves those by name before types exist");
            continue;
        }
        if (decl.typeParams.size() > 0) {
            // Generic methods would work -- the template is keyed by the mangled
            // name like any other generic -- but nothing exercises that yet, and
            // claiming support without a test is how a feature rots. Refuse
            // explicitly so it is obvious this is a gap, not a bug.
            m_diagnostics.error(decl.nameRange,
                                "a method cannot be generic yet; declare 'function " + sourceName +
                                    "<T>(...)' at file scope and take the receiver as a 'ptr " +
                                    owner->describe() + "' parameter");
            continue;
        }

        // Receiver validation. `isReceiver` is set by the parser when the first
        // parameter was written without a name.
        bool hasReceiver = false;
        bool receiverByPtr = false;
        if (!decl.params.empty() && decl.params[0].isReceiver) {
            TypeNode& receiverNode = *decl.params[0].type;
            if (!isSelfTypeNode(receiverNode)) {
                m_diagnostics.error(receiverNode.range,
                                    "the receiver of a method must be written as 'self' or "
                                    "'ptr self', found '" +
                                        receiverNode.spelling + "'");
                continue;
            }
            hasReceiver = true;
            receiverByPtr = isPtrSelfTypeNode(receiverNode);
        }
        // A parameter *named* `self` but written with a name is almost certainly
        // a receiver the author expected to be implicit.
        //
        // The signature is still declared afterwards, on purpose: suppressing the
        // function entirely would turn one mistake into "undefined method" at every
        // call site (spec §7.2 asks for no cascades), and the message above already
        // says what is wrong.
        for (std::size_t i = 0; i < decl.params.size(); ++i) {
            if (!decl.params[i].isReceiver && decl.params[i].name == "self") {
                m_diagnostics.error(decl.params[i].range,
                                    "the receiver is written as just 'self', without a name; write "
                                    "'function " + sourceName + "(self)' or 'function " +
                                        sourceName + "(ptr self)'");
                break;
            }
        }

        // Mangling: `Point.length`. A '.' cannot appear in an Axiom identifier, so
        // this can never collide with a name the user writes, and it reads well in
        // a diagnostic. `nameRange` is untouched, so error carets still land on
        // the method name in the source.
        decl.name = owner->describe() + "." + sourceName;

        // `self` resolves to the owner type while the signature is read. Only set
        // for the duration, so `self` stays undefined in free functions.
        const Type* savedReceiver = m_receiverType;
        m_receiverType = owner;
        const int index = declareFunctionSignature(decl, scope, /*topLevel=*/false);
        m_receiverType = savedReceiver;
        if (index < 0) {
            continue;
        }

        FuncInfo& info = m_functions[static_cast<std::size_t>(index)];
        info.ownerType = owner;
        info.hasReceiver = hasReceiver;
        info.receiverByPointer = receiverByPtr;

        if (hasReceiver) {
            // A value receiver is always const: a write through it would edit a
            // copy the caller never sees, which is the kind of bug that reads as
            // correct code. A pointer receiver keeps whatever the author wrote, so
            // `const ptr self` stays read-only instead of being silently loosened.
            if (!receiverByPtr) {
                decl.params[0].isConst = true;
                info.paramIsConst[0] = true;
            }
        }

        Symbol* symbol = scope.lookupLocal(decl.name);
        if (symbol == nullptr) {
            // declareFunctionSignature registers the symbol itself, so this only
            // happens if it bailed out after pushing the FuncInfo.
            continue;
        }
        m_methods[owner][sourceName] = symbol;
    }
}

/// Resolves `object.name(args)` and rewrites the expression into a plain call.
///
/// `objectIsType` selects the static form `Point.origin()`, where `objectType` is
/// the struct type named on the left. Otherwise `objectType` is the type of the
/// value (for `p.name()`) or of the pointee (for `p->name()`).
const Type* Sema::checkMethodCall(Expr& expr, Expr& callee, Scope& scope, Flow& flow,
                                  bool objectIsType, const Type* objectType) {
    const std::string& methodName = callee.text;

    const auto ownerIt = m_methods.find(objectType);
    if (ownerIt == m_methods.end()) {
        return nullptr;   // not a struct with methods
    }
    const auto symbolIt = ownerIt->second.find(methodName);
    if (symbolIt == ownerIt->second.end()) {
        return nullptr;   // struct has methods, but not this one
    }
    Symbol* symbol = symbolIt->second;

    // Every overload of one name must agree on the receiver kind, because the
    // receiver is built before an overload is chosen. Allowing `p.x()` to mean
    // "value receiver" for one overload and "pointer receiver" for another would
    // make the call depend on overload resolution that itself needs the receiver.
    bool wantReceiver = false;
    bool wantPtrReceiver = false;
    bool sawAny = false;
    for (const int index : symbol->overloads) {
        const FuncInfo& info = m_functions[static_cast<std::size_t>(index)];
        if (!sawAny) {
            wantReceiver = info.hasReceiver;
            wantPtrReceiver = info.receiverByPointer;
            sawAny = true;
            continue;
        }
        // The receiver is built before an overload is chosen, so every overload of
        // one name has to agree on its shape. Otherwise whether a call compiles
        // depends on which overload wins, which in turn depends on the receiver.
        if (info.hasReceiver != wantReceiver || info.receiverByPointer != wantPtrReceiver) {
            m_diagnostics.error(callee.range,
                                "the overloads of '" + methodName +
                                    "' disagree about their receiver; every overload of one name "
                                    "must be all methods with the same receiver form ('self' or "
                                    "'ptr self'), or all static functions");
            expr.type = m_types.errorType();
            return expr.type;
        }
    }

    if (objectIsType) {
        if (wantReceiver) {
            m_diagnostics.error(callee.range,
                                "'" + methodName + "' is a method of '" + objectType->describe() +
                                    "', so it needs an object: write 'object." + methodName +
                                    "(...)'; only a static method can be called on the type name");
            expr.type = m_types.errorType();
            return expr.type;
        }
        // `Point.origin(...)`: no receiver.
        auto calleeName = std::make_unique<Expr>(ExprKind::Name, callee.range);
        calleeName->text = symbol->name;
        calleeName->sym = symbol;
        expr.kids[0] = std::move(calleeName);
        const int chosen = resolveOverload(*symbol, expr.kids, scope, flow, expr.range);
        expr.funcIndex = chosen;
        // The call is what makes the body need to be legal for this instance.
        ensureDeferredBody(chosen);
        expr.type = chosen >= 0 ? m_functions[static_cast<std::size_t>(chosen)].ret
                                : m_types.errorType();
        return expr.type;
    }

    if (!wantReceiver) {
        m_diagnostics.error(callee.range,
                            "'" + methodName + "' is a static method of '" +
                                objectType->describe() + "'; call it on the type: '" +
                                objectType->describe() + "." + methodName + "(...)'");
        expr.type = m_types.errorType();
        return expr.type;
    }

    // Build the receiver argument. `callee.kind` says how the object was
    // written, which decides whether the object is already a pointer.
    const bool objectIsPointer = callee.kind == ExprKind::Arrow;
    ExprPtr receiver;
    if (objectIsPointer == wantPtrReceiver) {
        // `ptr self` with `p->m()`, or `self` with `v.m()`: pass the object as-is.
        receiver = std::move(callee.kids[0]);
    } else if (wantPtrReceiver) {
        // `ptr self` with `v.m()`: take the address, which needs a variable.
        Expr& object = *callee.kids[0];
        if (!object.isLValue) {
            m_diagnostics.error(callee.range,
                                "'" + methodName + "' takes 'ptr self', so it needs a variable it "
                                "can modify, but this is a temporary value");
            expr.type = m_types.errorType();
            return expr.type;
        }
        auto addr = std::make_unique<Expr>(ExprKind::Unary, object.range);
        addr->unop = UnOp::AddrOf;
        addr->kids.push_back(std::move(callee.kids[0]));
        addr->type = m_types.getPtr(objectType);
        receiver = std::move(addr);
    } else {
        // `self` with `p->m()`: dereference to get a value receiver.
        auto deref = std::make_unique<Expr>(ExprKind::Unary, callee.kids[0]->range);
        deref->unop = UnOp::Deref;
        deref->kids.push_back(std::move(callee.kids[0]));
        deref->type = objectType;
        receiver = std::move(deref);
    }

    // Rewrite `obj.m(args)` as `Point.m(obj, args)`. The receiver's type is
    // supplied so the overload set can be resolved without checking the receiver
    // expression a second time.
    const Type* receiverType = receiver->type;
    auto calleeName = std::make_unique<Expr>(ExprKind::Name, callee.range);
    calleeName->text = symbol->name;
    calleeName->sym = symbol;
    expr.kids[0] = std::move(calleeName);
    expr.kids.insert(expr.kids.begin() + 1, std::move(receiver));

    std::vector<const Type*> known(expr.kids.size(), nullptr);
    known[1] = receiverType;

    const int chosen = resolveOverload(*symbol, expr.kids, scope, flow, expr.range, &known);
    expr.funcIndex = chosen;
    // See the static path: the body becomes checkable only now that a call has
    // picked concrete type arguments.
    ensureDeferredBody(chosen);
    expr.type =
        chosen >= 0 ? m_functions[static_cast<std::size_t>(chosen)].ret : m_types.errorType();
    return expr.type;
}

} // namespace acvm

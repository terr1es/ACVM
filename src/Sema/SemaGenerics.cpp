//===--- SemaGenerics.cpp - Generic functions, compiled per type --------===//
//
// Axiom compiles generic functions by **monomorphisation**: a generic is not
// compiled as written. Each call infers concrete type arguments and asks for an
// instance, which is a deep copy of the declaration checked with `T` bound to
// the inferred type. So `sort<int>` and `sort<int64>` become two ordinary
// functions, and the generated code is identical to what a hand-written
// per-type version would be.
//
// ── why not type erasure ──────────────────────────────────────────────────
//
// The containers (`Vec`) do use erasure: an element size plus `memcpy` is enough
// because copying does not need to know the type. Sorting and arithmetic are a
// different matter -- they need `a < b`, `a + b`, `sizeof(T)` and address
// arithmetic with the right stride, none of which erasure can supply without
// runtime dispatch on every operation. Monomorphisation gives those away for
// free, because at instantiation time `T` really is a concrete type.
//
// ── why the body is copied rather than re-checked in place ────────────────
//
// Sema writes its answers into the tree (`Expr::type`, `Expr::sym`,
// `Stmt::funcIndex`, `TypeNode::resolved`). Checking one declaration twice would
// mean the second pass overwrites the first, and since IRGen runs only after all
// of Sema, the first instance would then be lowered using the second instance's
// symbols. Copying keeps each instance's analysis independent.
//
// ── what a type parameter may be ──────────────────────────────────────────
//
// Any type that the body's operations accept. There is no `T: Comparable`
// constraint syntax; whether `a < b` is legal is decided when the body is
// checked, for the concrete `T`, and the diagnostic points at the operator with
// `T` already substituted. That is the same place a hand-written `sortStrings`
// would fail, so it needs no separate explanation machinery.

#include "acvm/AST/ASTClone.h"
#include "acvm/Common/Diagnostic.h"
#include "acvm/Sema/Sema.h"

// `std::find` lives in <algorithm>. It used to arrive through libc++'s <memory>, so
// this file compiled on the one toolchain it was built with and failed under libstdc++
// with "no matching function for call to find" -- the only candidate left was the
// istreambuf_iterator overload that <string> pulls in. Named explicitly so the
// dependency is visible instead of inherited.
#include <algorithm>
#include <functional>
#include <string>

namespace acvm {

namespace {

/// `sort` + {int} -> `sort<int>`. Angle brackets cannot appear in an Axiom
/// identifier, so this cannot collide with a user-written function name.
std::string mangleInstance(const std::string& name, const std::vector<const Type*>& args) {
    std::string result = name;
    result += "<";
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            result += ",";
        }
        result += args[i] != nullptr ? args[i]->describe() : std::string("<unknown>");
    }
    result += ">";
    return result;
}

} // namespace

const Type* Sema::lookupTypeBinding(const std::string& name) const {
    // Innermost first: a nested instantiation must shadow the outer binding.
    for (auto it = m_typeBindings.rbegin(); it != m_typeBindings.rend(); ++it) {
        if (it->first == name) {
            return it->second;
        }
    }
    return nullptr;
}

void Sema::declareTemplate(Stmt& decl, Scope& scope) {
    // Ordered most specific first. A generic in a native library is also
    // bodyless, but "implemented in library 'x'" explains the real obstacle,
    // while "has no body" would read as "you forgot to write it".
    if (!decl.externalLibrary.empty()) {
        m_diagnostics.error(decl.nameRange,
                            "'" + decl.name +
                                "' is generic, but it is implemented in library '" +
                                decl.externalLibrary +
                                "'; a native library is compiled outside Axiom and cannot be "
                                "specialised for Axiom types. Write the generic in Axiom");
        return;
    }
    // A bodyless declaration cannot be instantiated: there is nothing to copy.
    if (decl.isForward) {
        std::string list;
        for (std::size_t i = 0; i < decl.typeParams.size(); ++i) {
            if (i != 0) {
                list += ", ";
            }
            list += decl.typeParams[i];
        }
        m_diagnostics.error(decl.nameRange,
                            "'" + decl.name + "' declares type parameter(s) <" + list +
                                "> but has no body; a generic function is compiled separately for "
                                "each type it is called with, so its body has to be available");
        return;
    }
    if (decl.funcKind == FuncKind::Event || decl.funcKind == FuncKind::Process) {
        m_diagnostics.error(decl.nameRange,
                            "a generic function cannot be '" +
                                std::string(decl.funcKind == FuncKind::Event ? "event" : "process") +
                                "'; the scheduler resolves those by name before types are known");
        return;
    }
    if (decl.funcKind == FuncKind::Global) {
        m_diagnostics.error(decl.nameRange, "a generic function cannot be 'global'");
        return;
    }

    FuncTemplate tmpl;
    tmpl.name = decl.name;
    tmpl.typeParams = decl.typeParams;
    tmpl.decl = &decl;
    tmpl.kind = decl.funcKind;
    tmpl.loc = decl.nameRange.begin;
    tmpl.file = decl.nameRange.begin.file;

    const int index = static_cast<int>(m_templates.size());
    m_templates.push_back(std::move(tmpl));

    // Put the name in scope so calls can find it. A template and an ordinary
    // function may share a name (that is a useful overload set), so reuse the
    // existing Function symbol when there is one.
    Symbol* symbol = scope.lookupLocal(decl.name);
    if (symbol != nullptr && symbol->kind != Symbol::Kind::Function) {
        m_diagnostics.error(decl.nameRange, "redefinition of '" + decl.name + "'");
        m_diagnostics.note(symbol->loc, "previous definition is here");
        return;
    }
    if (symbol == nullptr) {
        auto& created = m_symbols.emplace_back();
        created.kind = Symbol::Kind::Function;
        created.name = decl.name;
        created.isMutable = false;
        created.loc = decl.nameRange.begin;
        symbol = &created;
        scope.declare(symbol);
    }
    symbol->templates.push_back(index);
}

bool Sema::inferTypeArguments(FuncTemplate& tmpl, const std::vector<const Type*>& argTypes,
                              std::vector<const Type*>& out, std::string& why) {
    const std::vector<std::string>& params = tmpl.typeParams;
    std::vector<const Type*> bindings(params.size(), nullptr);

    const auto indexOfParam = [&](const std::string& name) -> int {
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (params[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    // Unifies one declared parameter shape against one argument type.
    std::function<bool(TypeNode&, const Type*)> unify = [&](TypeNode& node,
                                                            const Type* arg) -> bool {
        if (arg == nullptr || arg->isError()) {
            // The argument already failed to type-check; do not add a second
            // complaint about it.
            return false;
        }
        if (node.kind == TypeNode::Kind::Named) {
            const int slot = indexOfParam(node.name);
            if (slot >= 0) {
                const Type*& bound = bindings[static_cast<std::size_t>(slot)];
                if (bound == nullptr) {
                    bound = arg;
                    return true;
                }
                // The same `T` appearing twice must be the same type both times.
                return m_types.same(bound, arg);
            }
        }
        if (node.kind == TypeNode::Kind::Ptr) {
            if (!arg->isPtr() || node.inner == nullptr) {
                return false;
            }
            return unify(*node.inner, arg->elem);
        }
        if (node.kind == TypeNode::Kind::Array) {
            if (arg->kind != TypeKind::Array || node.inner == nullptr) {
                return false;
            }
            // Only the element type is unified here. The length is left to the
            // concrete pass, which has the diagnostics for "length must be a
            // compile time constant" and re-checks assignability with the real
            // `T` substituted.
            return unify(*node.inner, arg->elem);
        }
        if (node.kind == TypeNode::Kind::Tuple) {
            if (arg->kind != TypeKind::Tuple || arg->elems.size() != node.elems.size()) {
                return false;
            }
            for (std::size_t i = 0; i < node.elems.size(); ++i) {
                if (node.elems[i] == nullptr || !unify(*node.elems[i], arg->elems[i])) {
                    return false;
                }
            }
            return true;
        }
        // No type parameter involved: the argument only has to fit the declared
        // type, which the concrete pass will re-check with full diagnostics.
        const Type* expected = resolveTypeNodeQuiet(node);
        if (expected == nullptr || expected->isError()) {
            return false;
        }
        return m_types.same(expected, arg) || m_types.isAssignable(arg, expected);
    };

    for (std::size_t i = 0; i < argTypes.size() && i < tmpl.decl->params.size(); ++i) {
        TypeNode* declared = tmpl.decl->params[i].type.get();
        if (declared == nullptr) {
            continue;
        }
        // A null argument type means "not known yet", not "does not fit": a bare function
        // name has no type of its own -- `sortBy(points, n, keyX)` -- and only the
        // instantiated parameter, `Func<int, T>` with T bound, can type it. Skipping it
        // here is what lets T be inferred from the remaining arguments; if this parameter
        // was the only place T appeared, the "cannot infer" error below still fires.
        if (argTypes[i] == nullptr) {
            continue;
        }
        if (!unify(*declared, argTypes[i])) {
            why = "argument " + std::to_string(i + 1) + " has type '" +
                  (argTypes[i] != nullptr ? argTypes[i]->describe() : std::string("<unknown>")) +
                  "', which does not match the declared parameter";
            return false;
        }
    }

    out.clear();
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (bindings[i] == nullptr) {
            why = "cannot infer '" + params[i] +
                  "' because it does not appear in any parameter type; a generic function is "
                  "specialised from its arguments, so every type parameter has to be reachable "
                  "from them";
            return false;
        }
        out.push_back(bindings[i]);
    }
    return true;
}

int Sema::instantiateTemplate(int templateIndex, const std::vector<const Type*>& typeArgs,
                              SourceRange range) {
    FuncTemplate& tmpl = m_templates[static_cast<std::size_t>(templateIndex)];
    const std::string mangled = mangleInstance(tmpl.name, typeArgs);

    const auto existing = m_instances.find(mangled);
    if (existing != m_instances.end()) {
        return existing->second;
    }

    // A generic that calls itself with a new type each time would otherwise
    // compile forever. 32 levels is far more than any real use and still fails
    // fast enough to be readable.
    if (m_instantiationDepth >= 32) {
        m_diagnostics.error(range,
                            "'" + tmpl.name +
                                "' keeps instantiating itself with new types; generic recursion "
                                "has to reach a type that stops it (depth limit 32)");
        return -1;
    }

    StmtPtr clone = cloneStmt(*tmpl.decl);
    // The instance is concrete: it must not be treated as a template again, or a
    // call inside its own body would try to infer `T` from nothing.
    clone->typeParams.clear();
    clone->isForward = false;

    // Bind before resolving the signature: parameter and return types mention the
    // type parameters, and `resolveTypeNode` has to see the bindings to make them
    // concrete.
    const std::size_t bindingMark = m_typeBindings.size();
    for (std::size_t i = 0; i < tmpl.typeParams.size() && i < typeArgs.size(); ++i) {
        m_typeBindings.emplace_back(tmpl.typeParams[i], typeArgs[i]);
    }

    // Keep the clone alive before anything can point at it: FuncInfo::decl and
    // the expression nodes inside it outlive this call.
    Stmt* instance = clone.get();
    m_instantiatedDecls.push_back(std::move(clone));

    m_instantiationDepth += 1;
    Scope* global = m_typeScope;
    const int index = declareFunctionSignature(*instance, *global, true, /*declareIntoScope=*/false);
    if (index >= 0) {
        m_functions[static_cast<std::size_t>(index)].name = mangled;
        m_instances.emplace(mangled, index);
        // Register before checking the body: recursion through a *different*
        // type argument must find its instance, and a self-recursive call with
        // the same type arguments has to resolve to this very function.
        checkFunctionBody(*instance, *global);
    }
    m_instantiationDepth -= 1;

    m_typeBindings.resize(bindingMark);
    return index;
}

//===--- Instantiating a generic struct ----------------------------------===//
//
//  `struct Pair<T> { T first; T second; }` used as `Pair<int>` does four things:
//  clone the declaration, bind `T`, resolve the fields and lay them out, and
//  instantiate the methods too. What comes out is an ordinary struct -- `Kind` is
//  still `Struct` -- so layout, member access, IRGen and the native boundary never
//  need to know it was ever generic.
//
//  The instance's `Type::name` is the mangled name (`Pair<int32>`), and
//  `TypeContext` interns types by name, so "lay out a given argument list once"
//  and "aliases are equal" are the same fact.

void Sema::declareStructTemplate(Stmt& decl) {
    StructTemplate tmpl;
    tmpl.name = decl.name;
    tmpl.typeParams = decl.typeParams;
    tmpl.kind = decl.kind;
    tmpl.decl = &decl;
    tmpl.loc = decl.nameRange.begin;
    tmpl.file = decl.nameRange.begin.file;

    if (!decl.externalLibrary.empty()) {
        m_diagnostics.error(decl.nameRange,
                            "'" + decl.name +
                                "' is a generic type, but it is declared in library '" +
                                decl.externalLibrary +
                                "'; a native library is compiled outside Axiom and its layout "
                                "cannot depend on Axiom types. Declare the generic struct in the "
                                "matching .ax file, and let the .axd refer to 'ptr YourType'");
        // Registered anyway, marked rejected, so uses of it do not add a second
        // diagnostic about the argument list of something that was never a template.
        tmpl.rejected = true;
    }

    const auto existing = m_structTemplateIndex.find(decl.name);
    if (existing != m_structTemplateIndex.end()) {
        const StructTemplate& previous =
            m_structTemplates[static_cast<std::size_t>(existing->second)];
        m_diagnostics.error(decl.nameRange, "redefinition of generic type '" + decl.name + "'");
        m_diagnostics.note(previous.loc, "previous definition is here");
        return;
    }

    const int index = static_cast<int>(m_structTemplates.size());
    m_structTemplates.push_back(std::move(tmpl));
    m_structTemplateIndex.emplace(decl.name, index);
}

const Type* Sema::instantiateStruct(int templateIndex, const std::vector<const Type*>& typeArgs,
                                    SourceRange range) {
    StructTemplate& tmpl = m_structTemplates[static_cast<std::size_t>(templateIndex)];
    const std::string mangled = mangleInstance(tmpl.name, typeArgs);

    if (tmpl.rejected) {
        return m_types.errorType();
    }

    const auto cached = m_structInstances.find(mangled);
    if (cached != m_structInstances.end()) {
        return cached->second;
    }

    if (m_structInstantiationDepth >= 32) {
        m_diagnostics.error(range,
                            "'" + tmpl.name +
                                "' keeps instantiating itself with new type arguments; generic "
                                "recursion has to reach a type that stops it (depth limit 32)");
        return m_types.errorType();
    }

    // The type is created and indexed *before* the fields are resolved, so that a
    // field of type `ptr Node<T>` inside `Node<T>` finds this same type instead of
    // starting a second instantiation. (By value would be an error, and `lay
    // out` reports it; through a pointer it is legal and has to work.)
    const TypeKind kind = tmpl.kind == StmtKind::UnionDecl ? TypeKind::Union : TypeKind::Struct;
    const Type* type = m_types.getOrCreateAggregate(mangled, kind);
    m_structInstances.emplace(mangled, type);

    StmtPtr clone = cloneStmt(*tmpl.decl);
    clone->typeParams.clear();
    clone->structType = type;
    Stmt* instance = clone.get();
    m_instantiatedDecls.push_back(std::move(clone));

    const std::size_t bindingMark = m_typeBindings.size();
    for (std::size_t i = 0; i < tmpl.typeParams.size() && i < typeArgs.size(); ++i) {
        m_typeBindings.emplace_back(tmpl.typeParams[i], typeArgs[i]);
    }
    m_structInstantiationDepth += 1;

    // Fields, exactly as `declareDecls` does for a plain struct.
    auto* mutableType = const_cast<Type*>(type);
    std::vector<FieldInfo> fields;
    fields.reserve(instance->fields.size());
    std::set<std::string> seen;
    for (FieldDecl& field : instance->fields) {
        const Type* fieldType =
            field.type != nullptr ? resolveTypeNode(*field.type, *m_typeScope)
                                  : m_types.errorType();
        field.resolvedType = fieldType;
        if (!seen.insert(field.name).second) {
            m_diagnostics.error(field.range, "duplicate member '" + field.name + "'");
        }
        if (fieldType != nullptr && fieldType->isVoid()) {
            m_diagnostics.error(field.range,
                                std::string(instance->kind == StmtKind::UnionDecl ? "a union"
                                                                                  : "a struct") +
                                    " member cannot have type 'void'");
        }
        fields.push_back(FieldInfo{field.name, fieldType, 0});
    }
    if (instance->kind == StmtKind::UnionDecl && fields.empty()) {
        m_diagnostics.error(instance->nameRange,
                            "union '" + mangled +
                                "' has no members; a union needs at least one to have a size");
    }
    mutableType->fields = std::move(fields);
    mutableType->layoutDone = false;

    // Layout after the bindings are in place: `ensureLayout` recurses into nested
    // structs, and a nested `Inner<T>` must be instantiated with this `T`.
    ensureLayout(type, instance->nameRange);

    // Methods, in the same order the plain path uses: they may mention the struct
    // by value, which needs the size first.
    //
    // Methods get their signatures declared here and their **bodies checked on
    // first call**. That is semantics, not an optimization: what a method body may
    // do depends on `T`. `Pair<T>.sum` needs `+`, and a `Pair<Point>` payload has
    // no `+` -- instantiating eagerly would make *using* `Pair<Point>` a compile
    // error even when `sum` is never called. A container library hits this
    // immediately: `sort()` only holds for comparable elements. C++ defers member
    // function definitions for the same reason.
    if (!instance->methods.empty()) {
        declareMethods(*instance, *m_typeScope);
        for (StmtPtr& method : instance->methods) {
            if (method == nullptr || method->funcIndex < 0) {
                continue;
            }
            if (std::find(m_skipBodyChecks.begin(), m_skipBodyChecks.end(), method.get()) !=
                m_skipBodyChecks.end()) {
                continue;
            }
            FuncInfo& info = m_functions[static_cast<std::size_t>(method->funcIndex)];
            info.hasBody = false;
            info.bodyDeferred = true;
            info.deferredBindings.assign(m_typeBindings.begin(), m_typeBindings.end());
        }
    }

    m_structInstantiationDepth -= 1;
    m_typeBindings.resize(bindingMark);
    return type;
}

void Sema::ensureDeferredBody(int funcIndex) {
    if (funcIndex < 0 || static_cast<std::size_t>(funcIndex) >= m_functions.size()) {
        return;
    }
    FuncInfo& info = m_functions[static_cast<std::size_t>(funcIndex)];
    if (!info.bodyDeferred || info.decl == nullptr) {
        return;
    }
    // Cleared before checking so a method that calls itself terminates here instead
    // of recursing through this function.
    info.bodyDeferred = false;
    info.hasBody = true;

    const std::size_t bindingMark = m_typeBindings.size();
    for (const auto& binding : info.deferredBindings) {
        m_typeBindings.emplace_back(binding.first, binding.second);
    }
    checkFunctionBody(*info.decl, *m_typeScope);
    m_typeBindings.resize(bindingMark);
}

} // namespace acvm

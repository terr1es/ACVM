//===--- SemaSupport.cpp - Members, initialisers, overload resolution ----===//
#include "acvm/Sema/Sema.h"

#include "acvm/Common/Diagnostic.h"

#include <algorithm>
#include <string>
#include <vector>

namespace acvm {
namespace {

Int128 boundMin(const Type* type) {
    if (type->isUnsigned) {
        return 0;
    }
    if (type->bits >= 127) {
        return -static_cast<Int128>(static_cast<unsigned __int128>(1) << 126);
    }
    return -static_cast<Int128>(static_cast<unsigned __int128>(1) << (type->bits - 1));
}

Int128 boundMax(const Type* type) {
    if (type->isUnsigned) {
        if (type->bits >= 127) {
            return static_cast<Int128>((static_cast<unsigned __int128>(1) << 127) - 1);
        }
        return static_cast<Int128>((static_cast<unsigned __int128>(1) << type->bits) - 1);
    }
    if (type->bits >= 127) {
        return static_cast<Int128>((static_cast<unsigned __int128>(1) << 126) - 1);
    }
    return static_cast<Int128>((static_cast<unsigned __int128>(1) << (type->bits - 1)) - 1);
}

const Expr* literalCoreOf(const Expr& expr, bool& negated) {
    negated = false;
    const Expr* current = &expr;
    while (current->kind == ExprKind::Unary &&
           (current->unop == UnOp::Neg || current->unop == UnOp::Plus)) {
        if (current->unop == UnOp::Neg) {
            negated = !negated;
        }
        current = current->kids[0].get();
    }
    return current;
}

/// Non-mutating counterpart of Sema::adaptLiteral, used while scoring
/// overload candidates.
bool canAdaptLiteral(const Expr& expr, const Type* target) {
    if (target == nullptr || target->isError()) {
        return false;
    }
    bool negated = false;
    const Expr* core = literalCoreOf(expr, negated);
    if (core == nullptr) {
        return false;
    }

    switch (core->kind) {
    case ExprKind::IntLit: {
        if (target->isFloat()) {
            return true;
        }
        if (target->kind == TypeKind::Bool) {
            const Int128 value = negated ? -core->lit.intValue : core->lit.intValue;
            return value == 0 || value == 1;
        }
        if (target->kind == TypeKind::Char) {
            const Int128 value = negated ? -core->lit.intValue : core->lit.intValue;
            return value >= 0 && value <= 65535;
        }
        if (!target->isInt() && !target->isEnum()) {
            return false;
        }
        const Int128 value = negated ? -core->lit.intValue : core->lit.intValue;
        return value >= boundMin(target) && value <= boundMax(target);
    }
    case ExprKind::FloatLit:
        return target->isFloat();
    case ExprKind::CharLit:
        return target->kind == TypeKind::Char;
    case ExprKind::BoolLit:
        return target->kind == TypeKind::Bool;
    case ExprKind::StringLit:
        return target->kind == TypeKind::String;
    case ExprKind::NullLit:
        return target->isPtr();
    default:
        return false;
    }
}

std::string signatureOf(const FuncInfo& info) {
    std::string result = info.name + "(";
    for (std::size_t i = 0; i < info.params.size(); ++i) {
        if (i != 0) {
            result += ", ";
        }
        result += info.params[i] != nullptr ? info.params[i]->describe() : "<null>";
    }
    result += ")";
    return result;
}

} // namespace

//===----------------------------------------------------------------------===//
// Overload resolution
//===----------------------------------------------------------------------===//

const Type* Sema::checkIndirectCallOf(Expr& call, Expr& callee, const Type* signature,
                                     std::vector<ExprPtr>& args, Scope& scope, Flow& flow) {
    const std::string name =
        callee.kind == ExprKind::Name ? callee.text : std::string("this function value");
    const std::size_t given = args.size() - 1;
    if (given != signature->funcParamCount()) {
        m_diagnostics.error(call.range,
                            "'" + name + "' has type '" + signature->describe() + "' and takes " +
                                std::to_string(signature->funcParamCount()) +
                                " argument(s), but " + std::to_string(given) + " were given");
        for (std::size_t i = 1; i < args.size(); ++i) {
            checkExpr(*args[i], scope, flow);
        }
        call.type = m_types.errorType();
        return call.type;
    }
    bool ok = true;
    for (std::size_t i = 0; i < given; ++i) {
        checkExpr(*args[i + 1], scope, flow);
        adaptLiteral(*args[i + 1], signature->funcParam(i));
        const Type* actual = args[i + 1]->type;
        if (actual != nullptr && !actual->isError() &&
            !m_types.isAssignable(actual, signature->funcParam(i))) {
            m_diagnostics.error(args[i + 1]->range,
                                "cannot pass '" + actual->describe() + "' as '" +
                                    signature->funcParam(i)->describe() + "' to '" + name + "'");
            ok = false;
        }
    }
    if (!ok) {
        call.type = m_types.errorType();
        return call.type;
    }
    call.indirectCall = true;
    call.type = signature->funcReturn();
    return call.type;
}

bool Sema::resolveFunctionValue(Expr& expr, const Type* target, Scope& scope) {
    if (target == nullptr || !target->isFunc() || expr.kind != ExprKind::Name) {
        return false;
    }
    // Not a function: not this helper's business. Copying a `Func` from another `Func`
    // is an ordinary load, and an unknown name is the ordinary "undefined" error, so both
    // have to fall through to the regular path rather than be diagnosed here.
    Symbol* named = scope.lookup(expr.text);
    if (named == nullptr || named->kind != Symbol::Kind::Function) {
        return false;
    }

    int match = -1;
    bool ambiguous = false;
    for (const int index : named->overloads) {
        if (index < 0 || static_cast<std::size_t>(index) >= m_functions.size()) {
            continue;
        }
        const FuncInfo& candidate = m_functions[static_cast<std::size_t>(index)];
        if (candidate.params.size() != target->funcParamCount()) {
            continue;
        }
        if (!m_types.isAssignable(candidate.ret, target->funcReturn())) {
            continue;
        }
        bool fits = true;
        for (std::size_t i = 0; i < candidate.params.size(); ++i) {
            if (!m_types.isAssignable(candidate.params[i], target->funcParam(i))) {
                fits = false;
                break;
            }
        }
        if (!fits) {
            continue;
        }
        if (match >= 0) {
            ambiguous = true;
        }
        match = index;
    }

    if (match < 0) {
        m_diagnostics.error(expr.range, "no overload of '" + expr.text + "' matches '" +
                                            target->describe() + "'");
        expr.type = m_types.errorType();
        return false;
    }
    if (ambiguous) {
        m_diagnostics.error(expr.range,
                            "'" + expr.text + "' has more than one overload matching '" +
                                target->describe() + "'; the signature has to name one of "
                                "them exactly");
        expr.type = m_types.errorType();
        return false;
    }
    expr.funcValue = match;
    expr.sym = named;
    expr.type = target;
    expr.isLValue = false;
    return true;
}

int Sema::resolveOverload(Symbol& symbol, std::vector<ExprPtr>& args, Scope& scope, Flow& flow,
                          SourceRange range, const std::vector<const Type*>* knownTypes) {
    const std::size_t argc = args.size() - 1;

    // `knownTypes[i]`, when non-null, is the already-computed type of `args[i]`
    // and is trusted instead of being checked again. A method call uses this for
    // the receiver: its expression is checked before the overload set is chosen,
    // and checking it twice would report "may be used before initialisation"
    // twice for the same variable.
    std::vector<const Type*> argTypes(args.size(), nullptr);
    for (std::size_t i = 1; i < args.size(); ++i) {
        const bool known = knownTypes != nullptr && i < knownTypes->size() &&
                           (*knownTypes)[i] != nullptr;
        // Only a name that *is* a function can be a function value. A variable of type
        // `Func<...>` is an ordinary expression and takes the path below -- which is what
        // makes a generic body able to pass its own `Func<int, T>` parameter on: the
        // parameter is a local, not a function, and its type is already concrete.
        Symbol* named = (!known && args[i]->kind == ExprKind::Name)
                            ? scope.lookup(args[i]->text)
                            : nullptr;
        if (named != nullptr && named->kind == Symbol::Kind::Function) {
            // A bare function name as an argument: only the parameter type can resolve it,
            // and the parameter type belongs to the candidate. Taking the first `Func<...>`
            // declared at this position is exact whenever the overloads agree there, which
            // is the only case where passing a function name is unambiguous anyway.
            bool funcPosition = false;
            for (const int candidate : symbol.overloads) {
                if (candidate < 0 || static_cast<std::size_t>(candidate) >= m_functions.size()) {
                    continue;
                }
                const FuncInfo& info = m_functions[static_cast<std::size_t>(candidate)];
                if (i - 1 >= info.params.size() || !info.params[i - 1]->isFunc()) {
                    continue;
                }
                funcPosition = true;
                if (resolveFunctionValue(*args[i], info.params[i - 1], scope)) {
                    argTypes[i] = info.params[i - 1];
                }
                break;
            }
            if (argTypes[i] != nullptr) {
                continue;
            }
            // A *template* may also declare `Func<...>` here, and its parameter type is not
            // concrete yet -- `Func<int, T>` has no T until the other arguments say what it
            // is. Leaving the type null defers the check to `tryTemplates`, which resolves
            // it against the instantiated parameter. Anything else is the ordinary
            // "function used as a value" error, which the loop below then reports.
            if (!funcPosition) {
                for (const int templateIndex : symbol.templates) {
                    const FuncTemplate& tmpl = m_templates[static_cast<std::size_t>(templateIndex)];
                    if (tmpl.decl == nullptr || i - 1 >= tmpl.decl->params.size()) {
                        continue;
                    }
                    const TypeNode* declared = tmpl.decl->params[i - 1].type.get();
                    if (declared != nullptr && declared->kind == TypeNode::Kind::Named &&
                        declared->name == "Func") {
                        argTypes[i] = nullptr;
                        funcPosition = true;
                        break;
                    }
                }
                if (funcPosition) {
                    continue;
                }
            }
        }
        argTypes[i] = known ? (*knownTypes)[i] : checkExpr(*args[i], scope, flow);
    }

    // `private` is file scoped (spec §2.2.1.2): a function declared in another
    // file may not be called from here, even though `import` merged its name
    // into this scope. Reporting "not callable" or "no overload" would send
    // people looking for a typo, so say what actually happened.
    // "Visible here" means the call site sits in the same file as the
    // declaration, so the comparison uses the call's file, not the file being
    // compiled: `import` merges many files into one unit.
    const auto visibleHere = [&](int index) {
        const FuncInfo& info = m_functions[static_cast<std::size_t>(index)];
        if (info.kind != FuncKind::Private) {
            return true;
        }
        if (!range.begin.isValid() || info.file == kInvalidFileID) {
            return true;
        }
        return info.file == range.begin.file;
    };

    // Generic fallback: tried only when no ordinary overload fits. Doing it in
    // this order means a plain `function sumInt(...)` still wins over
    // `function sum<T>(...)` for the types it was written for, which keeps
    // existing code's behaviour unchanged when generics are introduced next to it.
    const auto tryTemplates = [&]() -> int {
        if (symbol.templates.empty()) {
            return -1;
        }
        int best = -1;
        std::string firstWhy;
        int privateTemplate = -1;
        for (const int templateIndex : symbol.templates) {
            FuncTemplate& tmpl = m_templates[static_cast<std::size_t>(templateIndex)];
            if (tmpl.decl == nullptr || tmpl.decl->params.size() != argc) {
                continue;
            }
            if (tmpl.kind == FuncKind::Private && range.begin.isValid() &&
                tmpl.file != kInvalidFileID && tmpl.file != range.begin.file) {
                privateTemplate = templateIndex;
                continue;
            }
            std::vector<const Type*> typeArgs;
            std::string why;
            // `argTypes` is indexed by argument position (index 0 is the callee
            // slot), while inference walks the declared parameters from 0.
            std::vector<const Type*> paramTypes;
            paramTypes.reserve(argc);
            for (std::size_t i = 0; i < argc; ++i) {
                paramTypes.push_back(argTypes[i + 1]);
            }
            if (!inferTypeArguments(tmpl, paramTypes, typeArgs, why)) {
                if (firstWhy.empty()) {
                    firstWhy = why;
                }
                continue;
            }
            const int instance = instantiateTemplate(templateIndex, typeArgs, range);
            if (instance < 0) {
                return -2;   // instantiation already reported a real error
            }
            // A deferred function value: the parameter type is concrete now, so the name
            // can finally be resolved against it. Failing here is the same class of error
            // as a wrong argument type, and `resolveFunctionValue` has already reported it.
            const FuncInfo& instantiated = m_functions[static_cast<std::size_t>(instance)];
            bool deferredOk = true;
            for (std::size_t i = 0; i < argc; ++i) {
                if (argTypes[i + 1] != nullptr || i >= instantiated.params.size()) {
                    continue;
                }
                if (!resolveFunctionValue(*args[i + 1], instantiated.params[i], scope)) {
                    deferredOk = false;
                }
            }
            if (!deferredOk) {
                return -2;
            }
            if (best >= 0 && best != instance) {
                m_diagnostics.error(range, "the call to '" + symbol.name +
                                               "' is ambiguous; it fits more than one generic "
                                               "instantiation");
                return -2;
            }
            best = instance;
        }
        if (best < 0 && !firstWhy.empty()) {
            m_diagnostics.error(range,
                                "no overload of '" + symbol.name + "' accepts these arguments; " +
                                    firstWhy);
            return -2;
        }
        if (best < 0 && privateTemplate >= 0) {
            // Same wording as the non-generic private diagnostic: the reader has
            // one problem, not two, and "no overload takes 1 argument" would send
            // them looking for an arity mistake.
            const FuncTemplate& tmpl = m_templates[static_cast<std::size_t>(privateTemplate)];
            const std::string_view path = m_diagnostics.path(tmpl.file);
            std::size_t cut = path.find_last_of("/\\");
            const std::string file =
                path.empty() ? std::string("another file")
                             : std::string(path.substr(cut == std::string_view::npos ? 0 : cut + 1));
            m_diagnostics.error(range, "'" + symbol.name + "' is private to " + file +
                                           "; it can only be called from that file");
            return -2;
        }
        return best;
    };

    std::vector<int> candidates;
    int privateIndex = -1;
    for (const int index : symbol.overloads) {
        if (m_functions[static_cast<std::size_t>(index)].params.size() != argc) {
            continue;
        }
        if (!visibleHere(index)) {
            privateIndex = index;
            continue;
        }
        candidates.push_back(index);
    }

    if (candidates.empty()) {
        // No ordinary overload has this arity. The name may still be a generic
        // (`function maxOf<T>(T, T)` is the only `maxOf`), so try that before
        // complaining about the argument count.
        const int instance = tryTemplates();
        if (instance >= 0) {
            return instance;
        }
        if (instance == -2) {
            return -1;
        }
        if (privateIndex >= 0) {
            const FuncInfo& info = m_functions[static_cast<std::size_t>(privateIndex)];
            const std::string_view path = m_diagnostics.path(info.file);
            // Spec §2.2.1.2 writes the message with just the file name.
            std::size_t cut = path.find_last_of("/\\");
            const std::string file =
                path.empty() ? std::string("another file")
                             : std::string(path.substr(cut == std::string_view::npos ? 0 : cut + 1));
            m_diagnostics.error(range, "'" + symbol.name + "' is private to " + file +
                                           "; it can only be called from that file");
            return -1;
        }
        m_diagnostics.error(range, "no overload of '" + symbol.name + "' takes " +
                                       std::to_string(argc) + " argument(s)");
        return -1;
    }

    const auto matchesExactly = [&](int index) {
        const FuncInfo& info = m_functions[static_cast<std::size_t>(index)];
        for (std::size_t i = 0; i < argc; ++i) {
            if (!m_types.same(argTypes[i + 1], info.params[i])) {
                return false;
            }
        }
        return true;
    };

    const auto matchesCompatible = [&](int index) {
        const FuncInfo& info = m_functions[static_cast<std::size_t>(index)];
        for (std::size_t i = 0; i < argc; ++i) {
            if (m_types.same(argTypes[i + 1], info.params[i])) {
                continue;
            }
            if (m_types.isAssignable(argTypes[i + 1], info.params[i])) {
                continue;
            }
            if (canAdaptLiteral(*args[i + 1], info.params[i])) {
                continue;
            }
            return false;
        }
        return true;
    };

    std::vector<int> exact;
    for (const int index : candidates) {
        if (matchesExactly(index)) {
            exact.push_back(index);
        }
    }

    // Tie-break before calling a tie ambiguous: a definition in the file that
    // makes the call wins over an equally good one from another file.
    //
    // The case that forced this: a library's `private helper` and an entry file's
    // public `helper` have the same signature. Inside the library, `helper` must
    // mean the library's own -- a name the entry file cannot even see should not
    // be able to break it. Reporting "ambiguous" instead would let an unrelated
    // declaration in another file make a working library uncallable, which is the
    // same class of failure as the collision it replaced, only louder.
    const auto preferSameFile = [&](std::vector<int>& list) {
        if (list.size() < 2 || !range.begin.isValid()) {
            return;
        }
        std::vector<int> sameFile;
        for (const int index : list) {
            if (m_functions[static_cast<std::size_t>(index)].file == range.begin.file) {
                sameFile.push_back(index);
            }
        }
        if (sameFile.size() == 1) {
            list = sameFile;
        }
    };
    preferSameFile(exact);

    int chosen = -1;
    if (exact.size() == 1) {
        chosen = exact[0];
    } else if (exact.size() > 1) {
        m_diagnostics.error(range, "the call to '" + symbol.name + "' is ambiguous");
        for (const int index : exact) {
            m_diagnostics.note(range.begin,
                               "candidate: " +
                                   signatureOf(m_functions[static_cast<std::size_t>(index)]));
        }
        return -1;
    } else {
        std::vector<int> compatible;
        for (const int index : candidates) {
            if (matchesCompatible(index)) {
                compatible.push_back(index);
            }
        }
        if (compatible.empty()) {
            const int instance = tryTemplates();
            if (instance >= 0) {
                return instance;
            }
            if (instance == -2) {
                return -1;
            }
            m_diagnostics.error(range, "no overload of '" + symbol.name +
                                           "' matches the argument types");
            for (const int index : candidates) {
                m_diagnostics.note(range.begin,
                                   "candidate: " +
                                       signatureOf(m_functions[static_cast<std::size_t>(index)]));
            }
            return -1;
        }
        preferSameFile(compatible);
        if (compatible.size() > 1) {
            m_diagnostics.error(range, "the call to '" + symbol.name +
                                           "' is ambiguous; add an explicit 'as' cast");
            for (const int index : compatible) {
                m_diagnostics.note(range.begin,
                                   "candidate: " +
                                       signatureOf(m_functions[static_cast<std::size_t>(index)]));
            }
            return -1;
        }
        chosen = compatible[0];
    }

    const FuncInfo& info = m_functions[static_cast<std::size_t>(chosen)];
    for (std::size_t i = 0; i < argc; ++i) {
        if (!m_types.same(argTypes[i + 1], info.params[i]) &&
            canAdaptLiteral(*args[i + 1], info.params[i])) {
            adaptLiteral(*args[i + 1], info.params[i]);
        }
    }

    return chosen;
}

//===----------------------------------------------------------------------===//
// Indexing, slicing, members
//===----------------------------------------------------------------------===//

const Type* Sema::checkIndex(Expr& expr, Scope& scope, Flow& flow) {
    const Type* baseType = checkExpr(*expr.kids[0], scope, flow);
    const Type* indexType = checkExpr(*expr.kids[1], scope, flow);

    if (baseType->isError()) {
        expr.type = baseType;
        return expr.type;
    }

    if (!indexType->isIntegerLike() && !indexType->isError()) {
        m_diagnostics.error(expr.kids[1]->range,
                            "an index must be an integer, found '" + indexType->describe() + "'");
    }

    switch (baseType->kind) {
    case TypeKind::Array:
        expr.isLValue = expr.kids[0]->isLValue;
        expr.type = baseType->elem;
        return expr.type;

    case TypeKind::String:
        // `s[i]` yields a writable char (spec §2.4.5.0).
        expr.isLValue = true;
        expr.type = m_types.charType();
        return expr.type;

    case TypeKind::Ptr:
        if (baseType->elem->isVoid()) {
            m_diagnostics.error(expr.range, "'ptr void' cannot be indexed");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.isLValue = true;
        expr.type = baseType->elem;
        return expr.type;

    case TypeKind::Tuple: {
        ConstValue index;
        if (!evalConst(*expr.kids[1], scope, index) || index.isFloat) {
            m_diagnostics.error(expr.kids[1]->range,
                                "a tuple index must be a compile time constant; tuple elements "
                                "may have different types");
            expr.type = m_types.errorType();
            return expr.type;
        }
        if (index.i < 0 || static_cast<std::size_t>(index.i) >= baseType->elems.size()) {
            m_diagnostics.error(expr.kids[1]->range, "tuple index out of range");
            expr.type = m_types.errorType();
            return expr.type;
        }
        expr.memberIndex = static_cast<int>(index.i);
        expr.isLValue = expr.kids[0]->isLValue;
        expr.type = baseType->elems[static_cast<std::size_t>(index.i)];
        return expr.type;
    }

    default:
        m_diagnostics.error(expr.range, "cannot index a value of type '" + baseType->describe() +
                                            "'");
        expr.type = m_types.errorType();
        return expr.type;
    }
}

const Type* Sema::checkSlice(Expr& expr, Scope& scope, Flow& flow) {
    const Type* baseType = checkExpr(*expr.kids[0], scope, flow);
    const Type* lowType = checkExpr(*expr.kids[1], scope, flow);
    const Type* highType = checkExpr(*expr.kids[2], scope, flow);

    if (!lowType->isIntegerLike() && !lowType->isError()) {
        m_diagnostics.error(expr.kids[1]->range, "slice bounds must be integers");
    }
    if (!highType->isIntegerLike() && !highType->isError()) {
        m_diagnostics.error(expr.kids[2]->range, "slice bounds must be integers");
    }

    if (baseType->kind == TypeKind::String) {
        expr.type = m_types.stringType();
        return expr.type;
    }
    if (baseType->kind == TypeKind::Array) {
        m_diagnostics.error(expr.range,
                            "array slices are not supported because Axiom has no slice type; "
                            "copy the elements or pass a 'ptr T' plus a length");
        expr.type = m_types.errorType();
        return expr.type;
    }
    if (!baseType->isError()) {
        m_diagnostics.error(expr.range, "cannot slice a value of type '" + baseType->describe() +
                                            "'");
    }
    expr.type = m_types.errorType();
    return expr.type;
}

const Type* Sema::checkMember(Expr& expr, Scope& scope, Flow& flow) {
    // `quicksort.main()` -- the base is not a variable. Reporting "undefined
    // variable 'quicksort'" sends people looking for a typo, when the real
    // answer is that this build compiles a single file and `import` is only
    // parsed but not loaded.
    const Expr& base = *expr.kids[0];
    if (base.kind == ExprKind::Name && scope.lookup(base.text) == nullptr) {
        m_diagnostics.error(base.range,
                            "undefined variable '" + base.text +
                                "'; a module cannot be used as a namespace here (this build "
                                "compiles a single file, and 'import' is parsed but not "
                                "loaded)");
        expr.type = m_types.errorType();
        return expr.type;
    }

    const Type* baseType = checkExpr(*expr.kids[0], scope, flow);

    if (baseType->isError()) {
        expr.type = baseType;
        return expr.type;
    }

    if (expr.kind == ExprKind::Arrow) {
        if (!baseType->isPtr() || (baseType->elem->kind != TypeKind::Struct &&
                                   baseType->elem->kind != TypeKind::Union)) {
            m_diagnostics.error(expr.range,
                                "'->' requires a pointer to a struct or union, found '" +
                                    baseType->describe() + "'");
            expr.type = m_types.errorType();
            return expr.type;
        }
        baseType = baseType->elem;
    }

    switch (baseType->kind) {
    // A union member is reached exactly like a struct member -- the difference is
    // only that every offset came out 0, which the layout already decided. Reading
    // a member that was not the last one written is a reinterpretation of those
    // bytes, by design; nothing is checked here.
    case TypeKind::Struct:
    case TypeKind::Union: {
        for (std::size_t i = 0; i < baseType->fields.size(); ++i) {
            if (baseType->fields[i].name == expr.text) {
                expr.memberIndex = static_cast<int>(i);
                expr.isLValue = expr.kids[0]->isLValue;
                expr.type = baseType->fields[i].type;
                return expr.type;
            }
        }
        m_diagnostics.error(expr.range,
                            std::string(baseType->kind == TypeKind::Union ? "union '" : "struct '") +
                                baseType->describe() + "' has no member '" + expr.text + "'");
        expr.type = m_types.errorType();
        return expr.type;
    }

    case TypeKind::String:
        if (expr.text == "length") {
            expr.memberIndex = kMemberLength;
            expr.type = m_types.intType();
            return expr.type;
        }
        if (expr.text == "ptr") {
            expr.memberIndex = kMemberPtr;
            expr.type = m_types.getPtr(m_types.charType());
            return expr.type;
        }
        m_diagnostics.error(expr.range,
                            "'string' has no member '" + expr.text +
                                "'; only 'length' and 'ptr' are available");
        expr.type = m_types.errorType();
        return expr.type;

    case TypeKind::Array:
        if (expr.text == "length") {
            expr.memberIndex = kMemberLength;
            expr.isConstExpr = true;
            expr.constInt = static_cast<Int128>(baseType->length);
            expr.type = m_types.intType();
            return expr.type;
        }
        m_diagnostics.error(expr.range, "an array has no member '" + expr.text +
                                            "'; only 'length' is available");
        expr.type = m_types.errorType();
        return expr.type;

    case TypeKind::Ptr:
        m_diagnostics.error(expr.range, "'" + expr.kids[0]->text + "' is a pointer; use '->" +
                                            expr.text + "' to access members");
        expr.type = m_types.errorType();
        return expr.type;

    case TypeKind::Vector:
        m_diagnostics.error(expr.range, "'vector' is not implemented in this build");
        expr.type = m_types.errorType();
        return expr.type;

    default:
        m_diagnostics.error(expr.range, "'" + baseType->describe() + "' has no members");
        expr.type = m_types.errorType();
        return expr.type;
    }
}

//===----------------------------------------------------------------------===//
// Aggregate initialisers
//===----------------------------------------------------------------------===//

const Type* Sema::checkInitializer(Expr& expr, const Type* target, Scope& scope, Flow& flow) {
    if (expr.kind != ExprKind::InitList) {
        // A leaf of an initialiser list: `Func<int, int> table[2] = {twice, square};` -- an
        // array of function values is the shape a dispatch table takes, so the element
        // target has to resolve a name the same way a single declaration does.
        if (expr.kind == ExprKind::Name && target->isFunc() &&
            resolveFunctionValue(expr, target, scope)) {
            return expr.type;
        }
        checkExpr(expr, scope, flow);
        // `struct P p[200] = {0}`: a scalar zero standing for a whole aggregate. This is the
        // C idiom for "zero it all", and it has to be caught here rather than in IRGen,
        // which would otherwise treat the 0 as the *address* to copy from -- a null pointer
        // dereference at run time, which is exactly what it used to do.
        //
        // Only a constant zero is allowed. `{5}` for a struct has no meaning to pick: is 5
        // the first field, or all fields? Refusing is better than guessing.
        if (target->isAggregate() && (expr.type == nullptr || !expr.type->isAggregate())) {
            const bool isZero = expr.isConstExpr && !expr.constIsFloat && expr.constInt == 0;
            if (!isZero) {
                m_diagnostics.error(expr.range,
                                    "cannot initialise '" + target->describe() +
                                        "' from " + expr.type->describe() +
                                        "; use '{0}' to zero it, or a '{...}' matching the "
                                        "type");
                expr.type = m_types.errorType();
                return expr.type;
            }
            expr.aggregateZeroFill = true;
            expr.type = target;
            return expr.type;
        }
        adaptLiteral(expr, target);
        return expr.type;
    }

    if (target->kind == TypeKind::Array) {
        if (expr.kids.size() > target->length) {
            m_diagnostics.error(expr.range,
                                "too many initialisers: the array holds " +
                                    std::to_string(target->length) + " element(s)");
        }
        for (ExprPtr& kid : expr.kids) {
            checkInitializer(*kid, target->elem, scope, flow);
        }
        expr.type = target;
        return expr.type;
    }

    // A union has one storage slot, so a brace initialiser may name exactly one
    // value and it goes to the first member -- the C rule. `{0}` is therefore the
    // portable way to write "all bytes zero", which is what a union needs before
    // its first assignment.
    if (target->kind == TypeKind::Union) {
        if (target->fields.empty()) {
            m_diagnostics.error(expr.range, "union '" + target->describe() + "' has no members");
            expr.type = m_types.errorType();
            return expr.type;
        }
        if (expr.kids.size() > 1) {
            m_diagnostics.error(expr.range,
                                "a union initialiser takes at most one value, because every "
                                "member shares the same storage; initialise one member in the "
                                "braces and assign any others by name afterwards");
        }
        if (!expr.kids.empty()) {
            checkInitializer(*expr.kids[0], target->fields[0].type, scope, flow);
        }
        expr.type = target;
        return expr.type;
    }

    if (target->kind == TypeKind::Struct || target->kind == TypeKind::Tuple) {
        const std::vector<const Type*> members = [&]() {
            std::vector<const Type*> result;
            if (target->kind == TypeKind::Struct) {
                for (const FieldInfo& field : target->fields) {
                    result.push_back(field.type);
                }
            } else {
                result = target->elems;
            }
            return result;
        }();

        if (expr.kids.size() > members.size()) {
            m_diagnostics.error(expr.range, "too many initialisers for '" + target->describe() +
                                                "'");
        }
        for (std::size_t i = 0; i < expr.kids.size(); ++i) {
            if (i >= members.size()) {
                break;
            }
            checkInitializer(*expr.kids[i], members[i], scope, flow);
        }
        expr.type = target;
        return expr.type;
    }

    // A wide bit string is a fixed block of bytes, so `{0}` means "all bits zero" --
    // the only initialiser that makes sense for a value with no numeric form. A
    // non-zero entry is refused rather than silently reinterpreted: there is no
    // field order to map braces onto.
    if (target->isBit()) {
        if (!expr.kids.empty()) {
            const Expr& only = *expr.kids[0];
            const bool zero = expr.kids.size() == 1 && only.kind == ExprKind::IntLit &&
                              only.lit.intValue == 0;
            if (!zero) {
                m_diagnostics.error(expr.range,
                                    "a '" + target->describe() +
                                        "' can only be initialised with '{0}'; assign bits "
                                        "through an array element instead");
                expr.type = m_types.errorType();
                return expr.type;
            }
        }
        expr.type = target;
        return expr.type;
    }

    m_diagnostics.error(expr.range, "a '{...}' initialiser cannot initialise a value of type '" +
                                        target->describe() + "'");
    expr.type = m_types.errorType();
    return expr.type;
}

//===----------------------------------------------------------------------===//
// Misc helpers
//===----------------------------------------------------------------------===//

void Sema::collectNames(Expr& expr, std::vector<std::string>& out) {
    if (expr.kind == ExprKind::Name) {
        out.push_back(expr.text);
    }
    for (ExprPtr& kid : expr.kids) {
        collectNames(*kid, out);
    }
}

} // namespace acvm

//===--- Sema.cpp - Declarations, types, scopes and statements -----------===//
#include "acvm/Sema/Sema.h"

#include "acvm/Common/IntRange.h"

#include "acvm/Common/TextDistance.h"

#include "acvm/Common/Diagnostic.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace acvm {
namespace {

/// Bounds of a type, for constant range checks.
///
/// Both delegate to Common/IntRange.h. They were a third hand-written copy of the
/// same arithmetic (after IntRange itself and SemaExpr's literal bounds), and all
/// three had the same 127-bit clamp -- which is why `int128` was rejected by three
/// separate checks instead of one. The comment in IntRange.h warns that a second
/// copy drifts silently; it happened twice. One definition now.
Int128 typeMin(const Type* type) {
    if (type->isUnsigned) {
        return 0;
    }
    return signedMinOf(type->bits);
}

Int128 typeMax(const Type* type) {
    if (type->isUnsigned) {
        // Same ceiling as literalMax: an unsigned *value* must still fit the signed
        // Int128 the VM carries, which is why `unsigned int128` is refused.
        if (type->bits >= 127) {
            return static_cast<Int128>((static_cast<unsigned __int128>(1) << 127) - 1);
        }
        return static_cast<Int128>((static_cast<unsigned __int128>(1) << type->bits) - 1);
    }
    return signedMaxOf(type->bits);
}

bool intFits(Int128 value, const Type* type) {
    if (type->kind == TypeKind::Char) {
        return value >= 0 && value <= 65535;
    }
    if (type->kind == TypeKind::Bool) {
        return value == 0 || value == 1;
    }
    if (!type->isInt() && !type->isEnum()) {
        return false;
    }
    return value >= typeMin(type) && value <= typeMax(type);
}

} // namespace

/// The root variable of an lvalue expression, if there is one.
Symbol* lvalueRoot(Expr& expr) {
    Expr* current = &expr;
    while (current != nullptr) {
        switch (current->kind) {
        case ExprKind::Name:
            return current->sym;
        case ExprKind::Member:
        case ExprKind::Arrow:
        case ExprKind::Index:
            current = current->kids[0].get();
            break;
        default:
            return nullptr;
        }
    }
    return nullptr;
}

//===----------------------------------------------------------------------===//
// Scope
//===----------------------------------------------------------------------===//

Symbol* Scope::declare(Symbol* symbol) {
    const auto inserted = m_symbols.emplace(symbol->name, symbol);
    return inserted.second ? nullptr : inserted.first->second;
}

Symbol* Scope::lookupLocal(std::string_view name) const {
    const auto it = m_symbols.find(std::string(name));
    return it == m_symbols.end() ? nullptr : it->second;
}

Symbol* Scope::lookup(std::string_view name) const {
    for (const Scope* scope = this; scope != nullptr; scope = scope->m_parent) {
        if (Symbol* symbol = scope->lookupLocal(name)) {
            return symbol;
        }
    }
    return nullptr;
}

void Scope::collectNames(std::vector<std::string>& out) const {
    for (const auto& entry : m_symbols) {
        out.push_back(entry.first);
    }
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

Sema::Sema(DiagnosticEngine& diagnostics, TypeContext& types)
    : m_diagnostics(diagnostics), m_types(types) {
    m_null = m_types.nullType();
}

void Sema::diagError(SourceRange range, std::string message) {
    if (!m_quiet) {
        m_diagnostics.error(range, std::move(message));
    }
}

void Sema::diagError(SourceLocation loc, std::string message) {
    if (!m_quiet) {
        m_diagnostics.error(loc, std::move(message));
    }
}

bool Sema::check(Unit& unit) {
    Scope* global = &m_scopes.emplace_back(nullptr);
    m_typeScope = global;

    std::size_t builtinCount = 0;
    const BuiltinEntry* builtins = builtinTable(builtinCount);
    for (std::size_t i = 0; i < builtinCount; ++i) {
        auto& symbol = m_symbols.emplace_back();
        symbol.kind = Symbol::Kind::Builtin;
        symbol.name = builtins[i].name;
        symbol.builtin = builtins[i].id;
        symbol.isMutable = false;
        global->declare(&symbol);
    }

    collectTypeNames(unit);
    declareDecls(unit, *global);

    for (auto& decl : unit.decls) {
        if (decl->kind == StmtKind::FuncDecl && !decl->isForward &&
            // A generic body is checked once per instantiation, with `T` bound;
            // checking it here would fail on the unbound type parameter.
            decl->typeParams.empty() &&
            std::find(m_skipBodyChecks.begin(), m_skipBodyChecks.end(), decl.get()) ==
                m_skipBodyChecks.end()) {
            checkFunctionBody(*decl, *global);
        }
        // ImportDecl nodes are consumed by the loader long before this point.

        // Method bodies. They are not in `unit.decls` (they live inside the struct
        // node), so the loop above does not reach them.
        if (decl->kind == StmtKind::StructDecl || decl->kind == StmtKind::UnionDecl) {
            for (StmtPtr& method : decl->methods) {
                if (std::find(m_skipBodyChecks.begin(), m_skipBodyChecks.end(), method.get()) ==
                    m_skipBodyChecks.end()) {
                    checkFunctionBody(*method, *global);
                }
            }
        }
    }

    reportDeferredLiterals(unit);

    // A library file is declarations only: it has no `main` and no bodies.
    if (!m_diagnostics.hasErrors() && !m_externalLibrary) {
        checkEntryPoint(unit);
    }

    return !m_diagnostics.hasErrors();
}

void Sema::checkEntryPoint(Unit& unit) {
    std::vector<const FuncInfo*> mains;
    for (const FuncInfo& fn : m_functions) {
        if (fn.name == "main") {
            mains.push_back(&fn);
        }
    }

    if (mains.empty()) {
        // Distinguish "nothing to compile" from "declarations but no entry
        // point": for an empty file the skeleton below would be misleading,
        // since the user has not written anything yet.
        if (unit.decls.empty()) {
            m_diagnostics.error(SourceLocation{},
                                "the file contains no code to compile: it is empty or holds "
                                "only comments");
            return;
        }

        // Point at the first declaration so the caret lands on real source.
        SourceRange range;
        range.begin = unit.decls.front()->range.begin;
        range.end = range.begin;
        m_diagnostics.error(range, "no entry point: this program does not define 'main'");
        m_diagnostics.help(range.begin,
                           "add 'function main():void { ... eof: pass; }' to make it runnable");
        return;
    }

    const auto nameRangeOf = [](const FuncInfo& info) {
        if (info.decl != nullptr) {
            return info.decl->nameRange;
        }
        SourceRange range;
        range.begin = info.loc;
        range.end = info.loc;
        return range;
    };

    if (mains.size() > 1) {
        m_diagnostics.error(nameRangeOf(*mains[1]),
                            "'main' must not be overloaded; a program has exactly one entry "
                            "point, but " + std::to_string(mains.size()) + " were declared");
        return;
    }

    const FuncInfo& main = *mains.front();

    // Each of the following explains what is wrong with the existing 'main'
    // instead of claiming that no 'main' exists.
    if (!main.hasBody) {
        m_diagnostics.error(nameRangeOf(main), "'main' is declared here but never defined");
        m_diagnostics.help(nameRangeOf(main).begin,
                           "give 'main' a body: 'function main():void { ... eof: pass; }'");
        return;
    }

    if (!main.params.empty()) {
        m_diagnostics.error(nameRangeOf(main),
                            "'main' must not take parameters, but it declares " +
                                std::to_string(main.params.size()) +
                                "; the entry point receives no arguments");
        return;
    }

    if (main.ret == nullptr || !main.ret->isVoid()) {
        const std::string actual =
            main.ret != nullptr ? main.ret->describe() : std::string("<unknown>");
        m_diagnostics.error(nameRangeOf(main),
                            "'main' must return 'void', but it returns '" + actual + "'");
        m_diagnostics.help(nameRangeOf(main).begin,
                           "declare it as 'function main():void'; a program's exit status comes "
                           "from 'trap()', not from a return value");
        return;
    }
}

void Sema::collectTypeNames(Unit& unit) {
    for (auto& decl : unit.decls) {
        // A generic struct has no type yet: one is made per distinct set of type
        // arguments, at the places that name them. Registered here so those places
        // can find the template.
        if ((decl->kind == StmtKind::StructDecl || decl->kind == StmtKind::UnionDecl) &&
            !decl->typeParams.empty()) {
            declareStructTemplate(*decl);
            continue;
        }
        if (decl->kind == StmtKind::StructDecl || decl->kind == StmtKind::UnionDecl) {
            const bool isUnion = decl->kind == StmtKind::UnionDecl;
            const Type* type =
                isUnion ? m_types.createUnion(decl->name) : m_types.createStruct(decl->name);
            auto& symbol = m_symbols.emplace_back();
            symbol.kind = Symbol::Kind::TypeName;
            symbol.name = decl->name;
            symbol.type = type;
            symbol.loc = decl->nameRange.begin;
            symbol.isMutable = false;
            if (Symbol* existing = m_scopes.front().declare(&symbol)) {
                m_diagnostics.error(decl->nameRange,
                                    "redefinition of '" + decl->name + "'");
                m_diagnostics.note(existing->loc, "previous definition is here");
            }
            decl->structType = type;
        } else if (decl->kind == StmtKind::EnumDecl) {
            const Type* type = m_types.createEnum(decl->name);
            auto& symbol = m_symbols.emplace_back();
            symbol.kind = Symbol::Kind::TypeName;
            symbol.name = decl->name;
            symbol.type = type;
            symbol.loc = decl->nameRange.begin;
            symbol.isMutable = false;
            if (Symbol* existing = m_scopes.front().declare(&symbol)) {
                m_diagnostics.error(decl->nameRange,
                                    "redefinition of '" + decl->name + "'");
                m_diagnostics.note(existing->loc, "previous definition is here");
            }
            decl->enumType = type;
        }
    }
}

void Sema::declareDecls(Unit& unit, Scope& global) {
    // Resolve struct field types first so that mutually referencing structs
    // can be laid out afterwards.
    for (auto& decl : unit.decls) {
        if (decl->kind != StmtKind::StructDecl && decl->kind != StmtKind::UnionDecl) {
            continue;
        }
        auto* type = const_cast<Type*>(decl->structType);
        if (type == nullptr) {
            continue;
        }
        std::vector<FieldInfo> fields;
        fields.reserve(decl->fields.size());
        std::set<std::string> seen;
        for (FieldDecl& field : decl->fields) {
            const Type* fieldType = resolveTypeNode(*field.type, global);
            field.resolvedType = fieldType;
            if (!seen.insert(field.name).second) {
                m_diagnostics.error(field.range, "duplicate member '" + field.name + "'");
            }
            if (fieldType->isVoid()) {
                m_diagnostics.error(field.range, std::string("a ") +
                                                       (decl->kind == StmtKind::UnionDecl ? "union"
                                                                                          : "struct") +
                                                       " member cannot have type 'void'");
            }
            fields.push_back(FieldInfo{field.name, fieldType, 0});
        }
        if (decl->kind == StmtKind::UnionDecl && fields.empty()) {
            // A union with no members has no size, and `sizeBytes` would stay 0 --
            // which makes `sizeof` report zero and pointer arithmetic on it step
            // by nothing. C rejects it too; say so instead of emitting a
            // zero-sized type.
            m_diagnostics.error(decl->nameRange,
                                "union '" + decl->name +
                                    "' has no members; a union needs at least one to have a size");
        }
        type->fields = std::move(fields);
        type->layoutDone = false;
    }

    for (auto& decl : unit.decls) {
        if (decl->kind == StmtKind::StructDecl || decl->kind == StmtKind::UnionDecl) {
            ensureLayout(decl->structType, decl->nameRange);
        }
    }

    // Methods come after layout: a signature may mention the struct by value, and
    // that needs the size to be known. All signatures land in the same pass, so a
    // method may call any other method regardless of declaration order.
    for (auto& decl : unit.decls) {
        if ((decl->kind == StmtKind::StructDecl || decl->kind == StmtKind::UnionDecl) &&
            !decl->methods.empty()) {
            declareMethods(*decl, global);
        }
    }

    for (auto& decl : unit.decls) {
        switch (decl->kind) {
        case StmtKind::EnumDecl:
            declareEnum(*decl, global);
            break;
        case StmtKind::VarDecl:
            declareGlobalVar(*decl, global);
            break;
        case StmtKind::FuncDecl:
            if (decl->typeParams.empty()) {
                declareFunctionSignature(*decl, global, true);
            } else {
                // A generic is not a function yet: it becomes one per distinct
                // set of inferred type arguments, at the call sites that need it.
                declareTemplate(*decl, global);
            }
            break;
        default:
            break;
        }
    }
}

void Sema::ensureLayout(const Type* type, SourceRange range) {
    if (type == nullptr || (type->kind != TypeKind::Struct && type->kind != TypeKind::Union)) {
        return;
    }
    if (type->layoutDone) {
        return;
    }
    if (m_layoutStack.count(type) != 0) {
        m_diagnostics.error(range,
                            std::string(type->kind == TypeKind::Union ? "union '" : "struct '") +
                                type->name +
                                "' cannot contain itself by value; use 'ptr " + type->name +
                                "' instead");
        return;
    }
    m_layoutStack.insert(type);

    auto* target = const_cast<Type*>(type);
    for (FieldInfo& field : target->fields) {
        ensureLayout(field.type, range);
    }

    // One layout implementation, shared with `TypeContext::setFields` -- see
    // layoutMembers. A struct and a union differ only in the offsets.
    m_types.setFields(type, target->fields);
    target->layoutDone = true;
    m_layoutStack.erase(type);
}

void Sema::declareEnum(Stmt& decl, Scope& scope) {
    const Type* type = decl.enumType;
    if (type == nullptr) {
        return;
    }
    auto* mutableType = const_cast<Type*>(type);

    Int128 next = 0;
    std::set<std::string> seen;
    for (EnumItem& item : decl.items) {
        if (!seen.insert(item.name).second) {
            m_diagnostics.error(item.range, "duplicate enum member '" + item.name + "'");
        }

        Int128 value = next;
        if (item.value) {
            ConstValue constant;
            if (evalConst(*item.value, scope, constant)) {
                if (constant.isFloat) {
                    m_diagnostics.error(item.value->range,
                                        "an enum member value must be an integer constant");
                } else {
                    value = constant.i;
                }
            } else {
                m_diagnostics.error(item.value->range,
                                    "an enum member value must be a compile time constant");
            }
        }
        item.evaluated = value;
        next = value + 1;

        mutableType->members.emplace_back(item.name, static_cast<std::int64_t>(value));

        auto& symbol = m_symbols.emplace_back();
        symbol.kind = Symbol::Kind::EnumConstant;
        symbol.name = item.name;
        symbol.type = type;
        symbol.enumType = type;
        symbol.enumValue = static_cast<std::int64_t>(value);
        symbol.isMutable = false;
        symbol.loc = item.range.begin;
        if (Symbol* existing = scope.declare(&symbol)) {
            m_diagnostics.error(item.range, "redefinition of '" + item.name + "'");
            m_diagnostics.note(existing->loc, "previous definition is here");
        }
    }
}

void Sema::declareGlobalVar(Stmt& decl, Scope& scope) {
    const Type* declared = resolveTypeNode(*decl.type, scope);
    const Type* type = declared;

    for (std::size_t i = decl.dims.size(); i-- > 0;) {
        ConstValue length;
        if (!evalConst(*decl.dims[i], scope, length) || length.isFloat) {
            m_diagnostics.error(decl.dims[i]->range,
                                "an array length must be a compile time constant integer");
            continue;
        }
        if (length.i <= 0) {
            m_diagnostics.error(decl.dims[i]->range, "an array length must be greater than zero");
            continue;
        }
        type = m_types.getArray(type, static_cast<std::uint64_t>(length.i));
    }

    if (type->isVoid()) {
        m_diagnostics.error(decl.nameRange, "a variable cannot have type 'void'");
        type = m_types.errorType();
    }

    decl.varType = type;

    auto& symbol = m_symbols.emplace_back();
    symbol.kind = Symbol::Kind::Global;
    symbol.name = decl.name;
    symbol.type = type;
    symbol.isConst = decl.isConst || decl.isComptime;
    symbol.isMutable = !symbol.isConst;
    symbol.isComptime = decl.isComptime;
    symbol.globalIndex = static_cast<int>(m_globals.size());
    symbol.loc = decl.nameRange.begin;

    if (decl.isComptime) {
        if (!decl.init) {
            m_diagnostics.error(decl.nameRange, "'comptime' requires an initialiser");
        } else {
            ConstValue constant;
            if (!evalConst(*decl.init, scope, constant)) {
                m_diagnostics.error(decl.init->range,
                                    "the initialiser of a 'comptime' declaration must be a "
                                    "compile time constant");
            } else {
                symbol.constIsFloat = constant.isFloat;
                symbol.constInt = constant.i;
                symbol.constFloat = constant.f;
                if (!constant.isFloat && type->isInt() && !intFits(constant.i, type)) {
                    m_diagnostics.error(decl.init->range,
                                        "constant is out of range for '" + type->describe() + "'");
                }
            }
        }
        symbol.kind = Symbol::Kind::Comptime;
    } else {
        if (decl.isConst && !decl.init) {
            m_diagnostics.error(decl.nameRange,
                                "a 'const' variable must be initialised");
        }
        m_globals.push_back(GlobalInfo{decl.name, type, decl.isConst, &decl,
                                       decl.nameRange.begin});
    }

    if (Symbol* existing = scope.declare(&symbol)) {
        m_diagnostics.error(decl.nameRange, "redefinition of '" + decl.name + "'");
        m_diagnostics.note(existing->loc, "previous definition is here");
    }
}

int Sema::declareFunctionSignature(Stmt& decl, Scope& scope, bool topLevel,
                                   bool declareIntoScope) {
    (void)topLevel;
    FuncInfo info;
    info.name = decl.name;
    info.kind = decl.funcKind;
    info.decl = &decl;
    info.loc = decl.nameRange.begin;
    info.forwardDeclared = decl.isForward;

    for (Param& param : decl.params) {
        const Type* type = resolveTypeNode(*param.type, scope);
        param.resolvedType = type;
        if (type->isVoid()) {
            m_diagnostics.error(param.range, "a parameter cannot have type 'void'");
        }
        if (type->isAggregate() && type->kind == TypeKind::Vector) {
            m_diagnostics.error(param.range, "'vector' is not implemented in this build");
        }
        info.params.push_back(type);
        info.paramNames.push_back(param.name);
        info.paramIsConst.push_back(param.isConst);
        info.paramPtrReadOnly.push_back(param.type != nullptr && param.type->isReadOnlyPtr);
    }

    // A declaration from a `library "x";` file crosses into native code. The
    // boundary passes everything in one 64 bit integer register, so a scalar
    // `double` (which the ABI would put in an SSE register) cannot be supported
    // without libffi. Reject it here, where the caret lands on the parameter,
    // instead of trapping at run time on the first call.
    if (!decl.externalLibrary.empty()) {
        for (std::size_t i = 0; i < info.params.size(); ++i) {
            const Type* param = info.params[i];
            if (param == nullptr) {
                continue;
            }
            if (param->isFloat()) {
                m_diagnostics.error(
                    info.loc,
                    "'" + decl.name + "' is implemented in library '" + decl.externalLibrary +
                        "', and a scalar 'double' parameter cannot cross that boundary; "
                        "use 'ptr double' or 'int64'");
                continue;
            }
            // An aggregate by value would be passed as one 64 bit register's worth
            // of bytes. For a struct or union bigger than that it is a silent
            // truncation; for a smaller one it depends on the host ABI's member
            // classification, which Axiom does not model. Either way the call would
            // look fine and compute nonsense, so refuse it and name the workaround.
            //
            // `string` is deliberately not in this group even though it is an
            // aggregate: the VM gives it a marshalling path of its own (copy the
            // text into the staging buffer, pass pointer and length), which is why
            // the standard library has always been able to pass it. Structs and
            // unions have no such path.
            const bool crossesAsAggregate = param->kind == TypeKind::Struct ||
                                            param->kind == TypeKind::Union ||
                                            param->kind == TypeKind::Array ||
                                            param->kind == TypeKind::Tuple;
            if (crossesAsAggregate) {
                m_diagnostics.error(
                    info.loc,
                    "'" + decl.name + "' is implemented in library '" + decl.externalLibrary +
                        "', and a '" + param->describe() +
                        "' parameter cannot cross that boundary by value; the call passes one "
                        "64 bit register, so an aggregate larger than that would be silently "
                        "truncated. Write 'ptr " + param->describe() +
                        " name, int count' to pass it by reference");
                continue;
            }
            // A pointer is marshalled as an array only when an integer count
            // follows it. Otherwise the raw VM address is passed straight to C,
            // where the first dereference crashes -- the VM's pages are not in
            // the process address space the native code can read. Reject the
            // declaration rather than let that happen at run time.
            if (param->isPtr()) {
                const bool marshalled =
                    i + 1 < info.params.size() && info.params[i + 1] != nullptr &&
                    info.params[i + 1]->isIntegerLike();
                if (!marshalled) {
                    m_diagnostics.error(
                        info.loc,
                        "'" + decl.name + "' is implemented in library '" + decl.externalLibrary +
                            "', and its parameter " + std::to_string(i + 1) +
                            " is a 'ptr' with no element count after it; the VM would pass a raw "
                            "address instead of a copy, which the library cannot dereference. "
                            "Write 'ptr T name, int count' to pass an array");
                }
            }
        }
    }

    const Type* returnType = m_types.voidType();
    if (decl.retType) {
        returnType = resolveTypeNode(*decl.retType, scope);
    }
    // A `double` result would come back in an SSE register, which the 64 bit
    // integer call site cannot read; reject it for the same reason as a scalar
    // `double` parameter.
    if (!decl.externalLibrary.empty() && returnType != nullptr && returnType->isFloat()) {
        m_diagnostics.error(decl.retType != nullptr ? decl.retType->range : decl.nameRange,
                            "'" + decl.name + "' is implemented in library '" +
                                decl.externalLibrary +
                                "', and a scalar 'double' result cannot cross that boundary; "
                                "return 'int64' or write through a 'ptr double'");
    }
    info.ret = returnType;

    if (decl.funcKind == FuncKind::Process && !returnType->isVoid()) {
        m_diagnostics.error(decl.nameRange, "a 'process' function must return 'void'");
    }
    if (decl.funcKind == FuncKind::Event && !returnType->isVoid()) {
        m_diagnostics.error(decl.nameRange, "an 'event' function must return 'void'");
    }

    // An instantiation is reachable only through the call that requested it, so
    // it gets no name of its own and cannot collide with (or shadow) anything.
    // Deduplication is not needed either: `instantiateTemplate` already keys
    // instances by their mangled name.
    if (!declareIntoScope) {
        info.name = decl.name;
        // The clone always comes from a template that was required to have a
        // body (see Sema::declareTemplate), and IRGen decides purely on this
        // flag whether to emit code for the function.
        info.hasBody = true;
        info.file = decl.nameRange.begin.file;
        const int index = static_cast<int>(m_functions.size());
        m_functions.push_back(std::move(info));
        decl.funcIndex = index;
        return index;
    }

    Symbol* symbol = scope.lookupLocal(decl.name);
    if (symbol != nullptr && symbol->kind != Symbol::Kind::Function) {
        m_diagnostics.error(decl.nameRange, "redefinition of '" + decl.name + "'");
        m_diagnostics.note(symbol->loc, "previous definition is here");
        decl.funcIndex = -1;
        return -1;
    }

    if (symbol == nullptr) {
        auto& created = m_symbols.emplace_back();
        created.kind = Symbol::Kind::Function;
        created.name = decl.name;
        created.isMutable = false;
        created.loc = decl.nameRange.begin;
        symbol = &created;
        // A local variable of the same name may exist in an outer scope; the
        // function still shadows it from here on.
        scope.declare(symbol);
    }

    // Same signature already declared?
    for (const int index : symbol->overloads) {
        FuncInfo& existing = m_functions[static_cast<std::size_t>(index)];
        bool sameSignature = existing.params.size() == info.params.size();
        if (sameSignature) {
            for (std::size_t i = 0; i < info.params.size(); ++i) {
                if (!m_types.same(existing.params[i], info.params[i])) {
                    sameSignature = false;
                    break;
                }
            }
        }
        if (!sameSignature) {
            continue;
        }
        if (existing.hasBody && !decl.isForward) {
            // Three cases reach a second body with an identical signature, and
            // they are not the same problem.
            const bool fromSameFile =
                existing.decl != nullptr &&
                existing.decl->nameRange.begin.file == decl.nameRange.begin.file;

            // 1. Either side is `private` and the two live in different files.
            //    Not a collision at all: `private` is file scoped, so these are
            //    distinct symbols that merely spell their names the same way --
            //    which is the entire reason to mark a helper `private`.
            //
            //    Merging them was a real bug in both directions. With the library
            //    first, `lib3` calling its own `helper` was told "'helper' is
            //    private to lib2.ax": importing one library broke another. With the
            //    entry file first, a program's own `helper` was reported as private
            //    to a library it had never heard of. Appending a separate FuncInfo
            //    keeps each one reachable only from the file that declared it.
            if (!fromSameFile &&
                (existing.kind == FuncKind::Private || info.kind == FuncKind::Private)) {
                continue;
            }

            if (!fromSameFile) {
                // 2. The entry file replacing an imported definition. Intentional
                //    (spec Q43): a program is allowed to shadow a library helper
                //    with its own. The replacement has to be *complete*, including
                //    `file` -- leaving that behind is what made a user's own
                //    function report as private to the library it overrode.
                const FileID existingFile = existing.decl != nullptr
                                                ? existing.decl->nameRange.begin.file
                                                : kInvalidFileID;
                const bool existingIsRoot = existingFile == m_rootFile;
                const bool declIsRoot = decl.nameRange.begin.file == m_rootFile;

                if (!existingIsRoot && !declIsRoot) {
                    // 3. Two imported files colliding. Neither is privileged, so
                    //    whichever was imported last would win silently -- and the
                    //    call site shows no sign of it. The failure is quiet and
                    //    total: an unrelated `import` elsewhere in the program can
                    //    swap out a function this file depends on.
                    m_diagnostics.error(
                        decl.nameRange,
                        "function '" + decl.name + "' is defined in two imported files "
                        "with the same signature");
                    m_diagnostics.note(existing.loc, "the other definition is here");
                    m_diagnostics.help(decl.nameRange.begin,
                                       "rename one of them, or make the helper 'private' "
                                       "so it only applies to its own file");
                    decl.funcIndex = index;
                    return index;
                }

                existing.ret = info.ret;
                existing.decl = &decl;
                existing.paramNames = info.paramNames;
                existing.file = decl.nameRange.begin.file;
                existing.loc = decl.nameRange.begin;
                decl.funcIndex = index;
                return index;
            }

            m_diagnostics.error(decl.nameRange,
                                "redefinition of '" + decl.name + "' with the same signature");
            m_diagnostics.note(existing.loc, "previous definition is here");
            decl.funcIndex = index;
            return index;
        }
        if (!decl.isForward) {
            // A definition and a `.axd` declaration share one FuncInfo, so a
            // mismatch here means the Axiom fallback does not implement the
            // declared signature. Saying so beats letting the body fail the
            // return-type check further down.
            if (!existing.externalLibrary.empty() && !m_types.same(existing.ret, info.ret)) {
                m_diagnostics.error(
                    decl.nameRange,
                    "the Axiom fallback for '" + decl.name + "' returns '" +
                        (info.ret != nullptr ? info.ret->describe() : std::string("<unknown>")) +
                        "', but the library declaration returns '" +
                        (existing.ret != nullptr ? existing.ret->describe()
                                                 : std::string("<unknown>")) +
                        "'; the two must match");
                m_skipBodyChecks.push_back(&decl);
                decl.funcIndex = index;
                return index;
            }
            existing.hasBody = true;
            existing.decl = &decl;
            existing.paramNames = info.paramNames;
        } else if (!decl.externalLibrary.empty()) {
            // A forward declaration from a `.axd` and a definition from the
            // matching `.ax` fallback land on the same FuncInfo. The definition
            // supplies the body; the declaration supplies the shared-library
            // symbol the VM should try first.
            existing.externalLibrary = decl.externalLibrary;
        }
        decl.funcIndex = index;
        return index;
    }

    const int index = static_cast<int>(m_functions.size());
    info.hasBody = !decl.isForward;
    info.file = decl.nameRange.begin.file;
    if (!decl.externalLibrary.empty()) {
        info.externalLibrary = decl.externalLibrary;
    }
    m_functions.push_back(std::move(info));
    symbol->overloads.push_back(index);
    decl.funcIndex = index;

    if (decl.funcKind == FuncKind::Event && decl.eventCond) {
        // The trigger condition runs before the call, so it must not refer to
        // the function's own parameters (spec §2.2.1.3).
        std::vector<std::string> names;
        collectNames(*decl.eventCond, names);
        for (const std::string& paramName : m_functions[static_cast<std::size_t>(index)].paramNames) {
            if (std::find(names.begin(), names.end(), paramName) != names.end()) {
                m_diagnostics.error(decl.eventCond->range,
                                    "an event trigger condition cannot reference the parameter '" +
                                        paramName +
                                        "'; the condition is evaluated before the call");
            }
        }
    }

    return index;
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

const Type* Sema::resolveTypeNode(TypeNode& node, Scope& scope) {
    switch (node.kind) {
    case TypeNode::Kind::Scalar: {
        switch (node.base) {
        case ScalarBase::Int: {
            const unsigned bits = node.bits == 0 ? 32 : node.bits;
            // 128, not 127: `int128` is the widest signed integer the VM can hold
            // natively, and its range (−2^127 .. 2^127−1) is exactly the range of
            // the `Int128` the VM carries values in. 127 was the cap while
            // `signedMinOf`/`signedMaxOf` still clamped there; see
            // Common/IntRange.h.
            if (bits < 2 || bits > 128) {
                diagError(node.range, "an integer width must be in [2, 128], found " +
                                          std::to_string(bits));
                node.resolved = m_types.errorType();
                return node.resolved;
            }
            // `unsigned int128` is refused rather than half-supported. Its range
            // is 0 .. 2^128−1, and the VM carries every scalar in a *signed*
            // 128-bit cell: the top half of that range has no representation, so
            // printing, `<` and float conversion would all read those values as
            // negative. Accepting the type would mean three silent wrong answers
            // instead of one honest error. `unsigned int127` reaches 2^127−1 and
            // stays inside the cell, which is why that width still works.
            if (bits == 128 && node.isUnsigned) {
                diagError(node.range,
                          "'unsigned int128' is not supported: its maximum (2^128−1) does not fit "
                          "the VM's signed 128 bit value; use 'int128' for the signed range, or "
                          "'unsigned int127' for up to 2^127−1");
                node.resolved = m_types.errorType();
                return node.resolved;
            }
            node.resolved = m_types.getInt(bits, node.isUnsigned);
            return node.resolved;
        }
        case ScalarBase::Float: {
            if (node.mantissa != 0 || node.exponent != 0) {
                if (node.mantissa < 1 || node.exponent < 2 || node.mantissa + node.exponent > 126) {
                    diagError(node.range,
                              "a 'doubleA.B' type requires A >= 1, B >= 2 and A + B <= 126");
                    node.resolved = m_types.errorType();
                    return node.resolved;
                }
                node.resolved = m_types.getFloat(64, node.mantissa, node.exponent);
                return node.resolved;
            }
            const unsigned bits = node.bits == 0 ? 32 : node.bits;
            if (bits != 16 && bits != 32 && bits != 64) {
                diagError(node.range,
                          "a float width must be 16, 32 or 64, found " + std::to_string(bits));
                node.resolved = m_types.errorType();
                return node.resolved;
            }
            node.resolved = m_types.getFloat(bits, 0, 0);
            return node.resolved;
        }
        case ScalarBase::Char:
            if (node.isUnsigned) {
                // `unsigned char` is exactly `char`.
            }
            node.resolved = m_types.charType();
            return node.resolved;
        case ScalarBase::Bool:
            if (node.isUnsigned) {
                diagError(node.range, "'unsigned' cannot modify 'bool'");
            }
            node.resolved = m_types.boolType();
            return node.resolved;
        case ScalarBase::Void:
            if (node.isUnsigned) {
                diagError(node.range, "'unsigned' cannot modify 'void'");
            }
            node.resolved = m_types.voidType();
            return node.resolved;
        }
        break;
    }
    case TypeNode::Kind::Named: {
        // `self` as a type is only meaningful inside a struct body, where it means
        // the struct being declared. Anywhere else `m_receiverType` is null and
        // the name falls through to the ordinary lookup, which reports it as
        // unknown -- correct, because `self` is not a type at file scope.
        if (node.name == "self" && m_receiverType != nullptr) {
            node.resolved = m_receiverType;
            return node.resolved;
        }
        // A generic function's type parameter (`function sort<T>(ptr T data)`).
        // A binding only exists while an instantiation is being checked, so an
        // ordinary function that says `T` still gets "unknown type 'T'".
        if (const Type* bound = lookupTypeBinding(node.name)) {
            node.resolved = bound;
            return node.resolved;
        }

        // `Pair<int>`: a generic struct. Looked up before the symbol table because
        // a template name has no `Type` of its own to point a symbol at.
        const auto templateIt = m_structTemplateIndex.find(node.name);
        if (templateIt != m_structTemplateIndex.end()) {
            const StructTemplate& tmpl =
                m_structTemplates[static_cast<std::size_t>(templateIt->second)];
            if (node.typeArgs.empty()) {
                diagError(node.range,
                          "'" + node.name + "' is a generic " +
                              std::string(tmpl.kind == StmtKind::UnionDecl ? "union" : "struct") +
                              " and needs type arguments, as in '" + node.name + "<int>'");
                node.resolved = m_types.errorType();
                return node.resolved;
            }
            if (node.typeArgs.size() != tmpl.typeParams.size()) {
                diagError(node.range,
                          "'" + node.name + "' takes " + std::to_string(tmpl.typeParams.size()) +
                              " type argument(s), but " + std::to_string(node.typeArgs.size()) +
                              " were given");
                node.resolved = m_types.errorType();
                return node.resolved;
            }
            std::vector<const Type*> typeArgs;
            typeArgs.reserve(node.typeArgs.size());
            bool argsOk = true;
            for (TypeNodePtr& arg : node.typeArgs) {
                if (arg == nullptr) {
                    argsOk = false;
                    continue;
                }
                const Type* resolved = resolveTypeNode(*arg, scope);
                if (resolved == nullptr || resolved->isError()) {
                    argsOk = false;
                }
                typeArgs.push_back(resolved);
            }
            if (!argsOk) {
                node.resolved = m_types.errorType();
                return node.resolved;
            }
            node.resolved = instantiateStruct(templateIt->second, typeArgs, node.range);
            return node.resolved;
        }
        if (!node.typeArgs.empty()) {
            diagError(node.range,
                      "'" + node.name + "' is not a generic type, so it takes no type arguments");
            node.resolved = m_types.errorType();
            return node.resolved;
        }

        if (node.name == "string") {
            node.resolved = m_types.stringType();
            return node.resolved;
        }
        Symbol* symbol = scope.lookup(node.name);
        if (symbol == nullptr || symbol->kind != Symbol::Kind::TypeName) {
            diagError(node.range, "unknown type '" + node.name + "'");
            node.resolved = m_types.errorType();
            return node.resolved;
        }
        node.resolved = symbol->type;
        return node.resolved;
    }
    case TypeNode::Kind::Ptr: {
        const Type* inner = node.inner ? resolveTypeNode(*node.inner, scope) : m_types.errorType();
        node.resolved = m_types.getPtr(inner);
        return node.resolved;
    }
    case TypeNode::Kind::Array: {
        const Type* element = node.inner ? resolveTypeNode(*node.inner, scope) : m_types.errorType();
        if (!node.arrayLen) {
            diagError(node.range, "an array type requires a length");
            node.resolved = m_types.errorType();
            return node.resolved;
        }
        ConstValue length;
        if (!evalConst(*node.arrayLen, scope, length) || length.isFloat) {
            diagError(node.arrayLen->range,
                      "an array length must be a compile time constant integer; 'const' values "
                      "are not compile time constants, use 'comptime'");
            node.resolved = m_types.errorType();
            return node.resolved;
        }
        if (length.i <= 0) {
            diagError(node.arrayLen->range, "an array length must be greater than zero");
            node.resolved = m_types.errorType();
            return node.resolved;
        }
        node.resolved = m_types.getArray(element, static_cast<std::uint64_t>(length.i));
        return node.resolved;
    }
    case TypeNode::Kind::Tuple: {
        if (node.elems.size() < 2) {
            diagError(node.range, "a tuple type needs at least two elements");
            node.resolved = m_types.errorType();
            return node.resolved;
        }
        std::vector<const Type*> elements;
        for (TypeNodePtr& element : node.elems) {
            const Type* type = resolveTypeNode(*element, scope);
            if (type->isVoid()) {
                diagError(element->range, "'void' cannot be a tuple element");
                type = m_types.errorType();
            }
            elements.push_back(type);
        }
        node.resolved = m_types.getTuple(std::move(elements));
        return node.resolved;
    }
    case TypeNode::Kind::Vector:
        diagError(node.range,
                  "'vector T' is not implemented in this build; the 'vec' module provides an "
                  "equivalent growable array");
        node.resolved = m_types.errorType();
        return node.resolved;
    }

    node.resolved = m_types.errorType();
    return node.resolved;
}

const Type* Sema::resolveTypeNodeQuiet(TypeNode& node) {
    const bool saved = m_quiet;
    m_quiet = true;
    const Type* type = resolveTypeNode(node, *m_typeScope);
    m_quiet = saved;
    return type;
}

//===----------------------------------------------------------------------===//
// Function bodies
//===----------------------------------------------------------------------===//

void Sema::checkFunctionBody(Stmt& decl, Scope& global) {
    if (decl.funcIndex < 0 || static_cast<std::size_t>(decl.funcIndex) >= m_functions.size()) {
        return;
    }
    // Copied, not referenced: checking this body can instantiate a generic
    // function, which appends to `m_functions` and may reallocate it.
    const Type* const declaredReturn = m_functions[static_cast<std::size_t>(decl.funcIndex)].ret;

    const Type* savedReturn = m_returnType;
    const FuncKind savedKind = m_funcKind;
    const int savedLoop = m_loopDepth;
    const int savedBranch = m_branchDepth;
    const int savedVirtual = m_virtualDepth;
    const bool savedEof = m_inEof;
    auto savedParams = m_paramNames;

    m_returnType = declaredReturn;
    m_funcKind = decl.funcKind;
    m_loopDepth = 0;
    m_branchDepth = 0;
    m_virtualDepth = 0;
    m_inEof = false;
    m_paramNames.clear();

    Scope* scope = &m_scopes.emplace_back(&global);
    Flow flow;

    for (Param& param : decl.params) {
        auto& symbol = m_symbols.emplace_back();
        symbol.kind = Symbol::Kind::Param;
        symbol.name = param.name;
        symbol.type = param.resolvedType;
        symbol.isMutable = !param.isConst;
        symbol.isConst = param.isConst;
        symbol.loc = param.range.begin;
        param.sym = &symbol;
        m_paramNames.push_back(param.name);

        if (Symbol* existing = scope->declare(&symbol)) {
            m_diagnostics.error(param.range, "redefinition of parameter '" + param.name + "'");
            m_diagnostics.note(existing->loc, "previous definition is here");
        }
        flow.assigned.insert(&symbol);
    }

    if (decl.eventCond) {
        // The trigger condition reads globals only; it is evaluated before the
        // call, so parameters are already rejected during signature checking.
        const Type* conditionType = checkExpr(*decl.eventCond, *scope, flow);
        if (!conditionType->isError() && conditionType->isAggregate()) {
            m_diagnostics.error(decl.eventCond->range,
                                "an event trigger condition must be a scalar expression");
        }
    }

    checkStmtList(decl.body, *scope, flow);

    const bool returns = !decl.body.empty() && alwaysReturns(*decl.body.back());
    if (decl.defaultRet) {
        if (m_returnType->isVoid()) {
            m_diagnostics.error(decl.defaultRet->range,
                                "a 'void' function cannot declare a default return value");
        } else {
            Flow defaultFlow = flow;
            const Type* type = checkExpr(*decl.defaultRet, *scope, defaultFlow);
            adaptLiteral(*decl.defaultRet, m_returnType);
            if (!m_types.isAssignable(type, m_returnType)) {
                m_diagnostics.error(decl.defaultRet->range,
                                    "the default return value has type '" + type->describe() +
                                        "' but the function returns '" +
                                        m_returnType->describe() + "'");
            }
        }
    } else if (!m_returnType->isVoid() && !returns) {
        m_diagnostics.error(decl.range,
                            "not all paths of '" + decl.name +
                                "' return a value; add a 'return' on every path or declare "
                                "'return:<expr>' in the parameter list");
    }

    if (decl.funcKind == FuncKind::Process) {
        bool hasReturn = false;
        for (const StmtPtr& stmt : decl.body) {
            if (containsReturn(*stmt)) {
                hasReturn = true;
                break;
            }
        }
        if (!hasReturn) {
            m_diagnostics.error(decl.nameRange,
                                "a 'process' function must contain at least one reachable 'return'");
        }
    }

    // eof block: no return, no import, no virtual (spec §2.2.2 / §2.2.5).
    m_inEof = true;
    Flow eofFlow = flow;
    checkStmtList(decl.eofBody, *scope, eofFlow);
    m_inEof = savedEof;

    m_returnType = savedReturn;
    m_funcKind = savedKind;
    m_loopDepth = savedLoop;
    m_branchDepth = savedBranch;
    m_virtualDepth = savedVirtual;
    m_paramNames = std::move(savedParams);
}

bool Sema::alwaysReturns(const Stmt& stmt) const {
    switch (stmt.kind) {
    case StmtKind::Return:
        return true;
    case StmtKind::Block:
    case StmtKind::ArenaScope: {
        for (const StmtPtr& inner : stmt.body) {
            if (alwaysReturns(*inner)) {
                return true;
            }
        }
        return false;
    }
    case StmtKind::If:
        return stmt.thenS && stmt.elseS && alwaysReturns(*stmt.thenS) &&
               alwaysReturns(*stmt.elseS);
    case StmtKind::Switch: {
        bool hasDefault = false;
        for (const CaseClause& clause : stmt.cases) {
            if (!clause.value) {
                hasDefault = true;
            }
        }
        if (!hasDefault) {
            return false;
        }
        for (const CaseClause& clause : stmt.cases) {
            bool clauseReturns = false;
            for (const StmtPtr& inner : clause.body) {
                if (alwaysReturns(*inner)) {
                    clauseReturns = true;
                    break;
                }
            }
            if (!clauseReturns && !clause.hasFallthrough) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool Sema::containsReturn(const Stmt& stmt) const {
    if (stmt.kind == StmtKind::Return) {
        return true;
    }
    for (const StmtPtr& inner : stmt.body) {
        if (containsReturn(*inner)) {
            return true;
        }
    }
    if (stmt.thenS && containsReturn(*stmt.thenS)) {
        return true;
    }
    if (stmt.elseS && containsReturn(*stmt.elseS)) {
        return true;
    }
    if (stmt.initS && containsReturn(*stmt.initS)) {
        return true;
    }
    for (const CaseClause& clause : stmt.cases) {
        for (const StmtPtr& inner : clause.body) {
            if (containsReturn(*inner)) {
                return true;
            }
        }
    }
    for (const StmtPtr& inner : stmt.eofBody) {
        if (containsReturn(*inner)) {
            return true;
        }
    }
    return false;
}

void Sema::checkStmtList(std::vector<StmtPtr>& stmts, Scope& scope, Flow& flow) {
    for (StmtPtr& stmt : stmts) {
        checkStmt(*stmt, scope, flow);
    }
}

void Sema::checkScopedList(std::vector<StmtPtr>& stmts, Scope& parent, Flow& flow) {
    Scope* scope = &m_scopes.emplace_back(&parent);
    checkStmtList(stmts, *scope, flow);
}

void Sema::checkValueReadable(Symbol& symbol, Flow& flow, SourceRange range) {
    if (symbol.kind != Symbol::Kind::Local && symbol.kind != Symbol::Kind::Param) {
        return;
    }
    if (flow.assigned.count(&symbol) == 0) {
        m_diagnostics.error(range, "variable '" + symbol.name +
                                       "' may be used before initialisation; add an initialiser "
                                       "or assign it on all paths");
    }
}


namespace {

void walkExpr(Expr& expr, const std::function<void(Expr&)>& visit) {
    visit(expr);
    for (ExprPtr& kid : expr.kids) {
        if (kid) {
            walkExpr(*kid, visit);
        }
    }
}

void walkStmt(Stmt& stmt, const std::function<void(Expr&)>& visit,
              const std::function<void(Stmt&)>& stmtVisit) {
    stmtVisit(stmt);
    if (stmt.init) {
        walkExpr(*stmt.init, visit);
    }
    if (stmt.cond) {
        walkExpr(*stmt.cond, visit);
    }
    if (stmt.post) {
        walkExpr(*stmt.post, visit);
    }
    if (stmt.defaultRet) {
        walkExpr(*stmt.defaultRet, visit);
    }
    if (stmt.eventCond) {
        walkExpr(*stmt.eventCond, visit);
    }
    for (StmtPtr& inner : stmt.body) {
        walkStmt(*inner, visit, stmtVisit);
    }
    for (StmtPtr& inner : stmt.eofBody) {
        walkStmt(*inner, visit, stmtVisit);
    }
    if (stmt.thenS) {
        walkStmt(*stmt.thenS, visit, stmtVisit);
    }
    if (stmt.elseS) {
        walkStmt(*stmt.elseS, visit, stmtVisit);
    }
    if (stmt.initS) {
        walkStmt(*stmt.initS, visit, stmtVisit);
    }
    for (CaseClause& clause : stmt.cases) {
        if (clause.value) {
            walkExpr(*clause.value, visit);
        }
        for (StmtPtr& inner : clause.body) {
            walkStmt(*inner, visit, stmtVisit);
        }
    }
}

} // namespace

void Sema::reportDeferredLiterals(Unit& unit) {
    const std::function<void(Expr&)> visit = [&](Expr& expr) {
        if (expr.literalOutOfRange && !expr.literalTyped) {
            m_diagnostics.error(expr.range,
                                "constant '" + expr.lit.text +
                                    "' does not fit in any type in this context (the default is "
                                    "'int'); add a suffix such as 'i64'");
        }
    };
    const std::function<void(Stmt&)> stmtVisit = [](Stmt&) {};
    for (StmtPtr& decl : unit.decls) {
        walkStmt(*decl, visit, stmtVisit);
    }
}

std::string Sema::conversionHint(const Type* from, const Type* to) const {
    if (from == nullptr || to == nullptr) {
        return {};
    }
    const auto numeric = [](const Type* type) {
        return type->isNumeric() || type->kind == TypeKind::Char ||
               type->kind == TypeKind::Bool;
    };
    if (!numeric(from) || !numeric(to) || m_types.same(from, to)) {
        return {};
    }
    if (!m_types.isAssignable(from, to)) {
        return "; convert it explicitly with 'as " + to->describe() + "'";
    }
    return {};
}

std::string Sema::suggestName(std::string_view name, Scope& scope) const {
    std::vector<std::string> candidates;
    for (Scope* current = &scope; current != nullptr; current = current->parent()) {
        current->collectNames(candidates);
    }

    std::string best;
    int bestDistance = 3;
    for (const std::string& candidate : candidates) {
        const int distance = editDistance(name, candidate);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

} // namespace acvm

//===--- SemaStmt.cpp - Statement checking -------------------------------===//
#include "acvm/Sema/Sema.h"

#include "acvm/Common/Diagnostic.h"

#include <algorithm>
#include <string>
#include <utility>

namespace acvm {
namespace {

bool intFitsLocal(Int128 value, const Type* type) {
    if (type->kind == TypeKind::Char) {
        return value >= 0 && value <= 65535;
    }
    if (!type->isInt() && !type->isEnum()) {
        return false;
    }
    Int128 low = 0;
    Int128 high = 0;
    if (type->isUnsigned) {
        low = 0;
        high = type->bits >= 127
                   ? static_cast<Int128>((static_cast<unsigned __int128>(1) << 127) - 1)
                   : static_cast<Int128>((static_cast<unsigned __int128>(1) << type->bits) - 1);
    } else {
        low = type->bits >= 127
                  ? -static_cast<Int128>(static_cast<unsigned __int128>(1) << 126)
                  : -static_cast<Int128>(static_cast<unsigned __int128>(1) << (type->bits - 1));
        high = type->bits >= 127
                   ? static_cast<Int128>((static_cast<unsigned __int128>(1) << 126) - 1)
                   : static_cast<Int128>((static_cast<unsigned __int128>(1) << (type->bits - 1)) - 1);
    }
    return value >= low && value <= high;
}

bool isScalarCondition(const Type* type) {
    if (type == nullptr) {
        return false;
    }
    switch (type->kind) {
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Ptr:
    case TypeKind::Enum:
        return true;
    default:
        return false;
    }
}

} // namespace

void Sema::checkStmt(Stmt& stmt, Scope& scope, Flow& flow) {
    switch (stmt.kind) {
    case StmtKind::VarDecl:
        checkVarDecl(stmt, scope, flow);
        return;

    case StmtKind::FuncDecl: {
        const int index = declareFunctionSignature(stmt, scope, false);
        if (index >= 0 && !stmt.isForward) {
            // A nested function can see globals but never the enclosing locals
            // (spec §2.2, Q99).
            checkFunctionBody(stmt, *m_typeScope);
        }
        return;
    }

    case StmtKind::ImportDecl:
        m_diagnostics.warning(stmt.range,
                              "modules are not implemented in this build; this 'import' has no "
                              "effect; it loads nothing");
        return;

    case StmtKind::StructDecl:
    case StmtKind::UnionDecl:
    case StmtKind::EnumDecl:
        m_diagnostics.error(stmt.range,
                            "a struct, union or enum declaration is only allowed at file scope");
        return;

    case StmtKind::ExprStmt:
        if (stmt.init) {
            checkExpr(*stmt.init, scope, flow);
        }
        return;

    case StmtKind::Block:
        // A bare block does NOT introduce a scope (spec §2.5.1).
        checkStmtList(stmt.body, scope, flow);
        return;

    case StmtKind::If:
        checkIf(stmt, scope, flow);
        return;

    case StmtKind::While:
        checkWhile(stmt, scope, flow);
        return;

    case StmtKind::For:
        checkFor(stmt, scope, flow);
        return;

    case StmtKind::ForIn:
        checkForIn(stmt, scope, flow);
        return;

    case StmtKind::Loop: {
        const Type* countType = checkExpr(*stmt.cond, scope, flow);
        if (!countType->isIntegerLike() && !countType->isError()) {
            m_diagnostics.error(stmt.cond->range,
                                "the 'loop' count must be an integer, found '" +
                                    countType->describe() + "'");
        }

        Flow bodyFlow = flow;
        ++m_loopDepth;
        ++m_branchDepth;
        if (!stmt.label.empty()) {
            m_labels.insert(stmt.label);
        }
        if (stmt.thenS) {
            if (stmt.thenS->kind == StmtKind::Block) {
                checkScopedList(stmt.thenS->body, scope, bodyFlow);
            } else {
                Scope* inner = &m_scopes.emplace_back(&scope);
                checkStmt(*stmt.thenS, *inner, bodyFlow);
            }
        }
        if (!stmt.label.empty()) {
            m_labels.erase(stmt.label);
        }
        --m_branchDepth;
        --m_loopDepth;
        return;
    }

    case StmtKind::Switch:
        checkSwitch(stmt, scope, flow);
        return;

    case StmtKind::Break:
    case StmtKind::Continue: {
        const bool isBreak = stmt.kind == StmtKind::Break;
        const char* keyword = isBreak ? "break" : "continue";
        if (!stmt.label.empty()) {
            if (m_labels.count(stmt.label) == 0) {
                m_diagnostics.error(stmt.range,
                                    std::string("unknown label '") + stmt.label + "'");
            }
        } else if (isBreak) {
            if (m_loopDepth == 0 && m_switchDepth == 0) {
                m_diagnostics.error(stmt.range, "'break' outside of a loop or switch");
            }
        } else if (m_loopDepth == 0) {
            m_diagnostics.error(stmt.range, std::string("'") + keyword + "' outside of a loop");
        }
        flow.reachable = false;
        return;
    }

    case StmtKind::Return: {
        if (m_inEof) {
            m_diagnostics.error(stmt.range, "'return' is not allowed inside the eof block");
            return;
        }
        const bool isVoid = m_returnType == nullptr || m_returnType->isVoid();
        if (!stmt.init) {
            if (!isVoid) {
                m_diagnostics.error(stmt.range,
                                    "this function returns '" + m_returnType->describe() +
                                        "'; 'return' needs a value");
            }
        } else if (isVoid) {
            // Spec §2.2.3: in a void function `return expr;` is accepted and the
            // expression is neither checked nor evaluated.
        } else {
            const Type* valueType = checkExpr(*stmt.init, scope, flow);
            const unsigned errorsBefore = m_diagnostics.errorCount();
            adaptLiteral(*stmt.init, m_returnType);
            if (m_diagnostics.errorCount() == errorsBefore &&
                !m_types.isAssignable(stmt.init->type != nullptr ? stmt.init->type : valueType,
                                      m_returnType)) {
                m_diagnostics.error(stmt.init->range,
                                    "cannot return a value of type '" + valueType->describe() +
                                        "' from a function returning '" +
                                        m_returnType->describe() + "'");
            }
        }
        flow.reachable = false;
        return;
    }

    case StmtKind::Pass:
    case StmtKind::Empty:
        return;

    case StmtKind::Next:
        if (m_funcKind != FuncKind::Process) {
            m_diagnostics.error(stmt.range,
                                "'next' is only allowed inside a 'process' function");
        }
        return;

    case StmtKind::ArenaScope:
        checkScopedList(stmt.body, scope, flow);
        return;

    case StmtKind::Virtual:
        checkVirtual(stmt, scope, flow);
        return;

    case StmtKind::Destructure:
        checkDestructure(stmt, scope, flow);
        return;
    }
}

void Sema::checkVarDecl(Stmt& stmt, Scope& scope, Flow& flow) {
    const Type* declared = resolveTypeNode(*stmt.type, scope);
    const Type* type = declared;

    for (std::size_t i = stmt.dims.size(); i-- > 0;) {
        ConstValue length;
        if (!evalConst(*stmt.dims[i], scope, length) || length.isFloat) {
            m_diagnostics.error(stmt.dims[i]->range,
                                "an array length must be a compile time constant integer; "
                                "'const' values are not compile time constants, use 'comptime'");
            continue;
        }
        if (length.i <= 0) {
            m_diagnostics.error(stmt.dims[i]->range, "an array length must be greater than zero");
            continue;
        }
        type = m_types.getArray(type, static_cast<std::uint64_t>(length.i));
    }

    if (type->isVoid()) {
        m_diagnostics.error(stmt.nameRange, "a variable cannot have type 'void'");
        type = m_types.errorType();
    }

    if (stmt.isComptime) {
        if (!stmt.init) {
            m_diagnostics.error(stmt.nameRange, "'comptime' requires an initialiser");
        } else {
            ConstValue constant;
            if (!evalConst(*stmt.init, scope, constant)) {
                m_diagnostics.error(stmt.init->range,
                                    "the initialiser of a 'comptime' declaration must be a compile "
                                    "time constant; function calls, pointers and 'const' values "
                                    "are not allowed");
            } else if (!constant.isFloat && type->isInt() && !intFitsLocal(constant.i, type)) {
                m_diagnostics.error(stmt.init->range, "constant is out of range for '" +
                                                          type->describe() + "'");
            }
        }
    } else if (stmt.isConst && !stmt.init) {
        m_diagnostics.error(stmt.nameRange, "a 'const' variable must be initialised");
    } else if (stmt.init) {
        if (type->isFunc() && stmt.init->kind == ExprKind::Name &&
            resolveFunctionValue(*stmt.init, type, scope)) {
            // Handled: the initialiser is a handle, not a load.
        } else if (stmt.init->kind == ExprKind::InitList) {
            checkInitializer(*stmt.init, type, scope, flow);
        } else {
            checkExpr(*stmt.init, scope, flow);
            const unsigned errorsBefore = m_diagnostics.errorCount();
            adaptLiteral(*stmt.init, type);
            const Type* initType = stmt.init->type;
            if (m_diagnostics.errorCount() == errorsBefore && initType != nullptr &&
                !m_types.isAssignable(initType, type)) {
                m_diagnostics.error(stmt.init->range,
                                    "cannot initialise '" + stmt.name + "' of type '" +
                                        type->describe() + "' with a value of type '" +
                                        initType->describe() + "'" +
                                        conversionHint(initType, type));
            }
        }
    }

    stmt.varType = type;

    auto& symbol = m_symbols.emplace_back();
    symbol.name = stmt.name;
    symbol.loc = stmt.nameRange.begin;
    if (stmt.isComptime) {
        symbol.kind = Symbol::Kind::Comptime;
        symbol.isComptime = true;
        symbol.isMutable = false;
        symbol.isConst = true;
        symbol.type = type;
        if (stmt.init) {
            ConstValue constant;
            if (evalConst(*stmt.init, scope, constant)) {
                symbol.constIsFloat = constant.isFloat;
                symbol.constInt = constant.i;
                symbol.constFloat = constant.f;
            }
        }
    } else {
        symbol.kind = Symbol::Kind::Local;
        symbol.type = type;
        symbol.isConst = stmt.isConst;
        symbol.isMutable = !stmt.isConst;
    }
    stmt.sym = &symbol;

    if (Symbol* existing = scope.declare(&symbol)) {
        m_diagnostics.error(stmt.nameRange,
                            "'" + stmt.name + "' is already declared in this scope");
        m_diagnostics.note(existing->loc, "previous declaration is here");
        return;
    }

    if (stmt.isComptime) {
        return;
    }
    if (stmt.init) {
        flow.assigned.insert(&symbol);
    } else {
        flow.assigned.erase(&symbol);
    }
}

void Sema::checkBranch(Stmt& body, Scope& scope, Flow& flow) {
    if (body.kind == StmtKind::Block) {
        checkScopedList(body.body, scope, flow);
        return;
    }
    if (body.kind == StmtKind::Empty) {
        return;
    }
    Scope* inner = &m_scopes.emplace_back(&scope);
    checkStmt(body, *inner, flow);
}

void Sema::checkIf(Stmt& stmt, Scope& scope, Flow& flow) {
    const Type* condType = checkExpr(*stmt.cond, scope, flow);
    if (!isScalarCondition(condType) && !condType->isError()) {
        m_diagnostics.error(stmt.cond->range,
                            "the condition of an 'if' must be a scalar, found '" +
                                condType->describe() + "'");
    }
    if (stmt.cond->kind == ExprKind::Assign) {
        m_diagnostics.warning(stmt.cond->range,
                              "suspicious assignment inside a condition; use '==' to compare");
    }

    if (stmt.thenS && stmt.thenS->kind == StmtKind::Empty) {
        m_diagnostics.warning(stmt.thenS->range,
                              "empty statement after 'if'; the body does nothing");
    }

    // Both branches open a scope of their own (spec §2.5.1). A bare `{...}`
    // block does not, so the scope is created here rather than in checkStmt.
    Flow thenFlow = flow;
    if (stmt.thenS) {
        ++m_branchDepth;
        checkBranch(*stmt.thenS, scope, thenFlow);
        --m_branchDepth;
    }

    Flow elseFlow = flow;
    if (stmt.elseS) {
        ++m_branchDepth;
        checkBranch(*stmt.elseS, scope, elseFlow);
        --m_branchDepth;
    }

    if (!thenFlow.reachable && !elseFlow.reachable) {
        flow.reachable = false;
        return;
    }
    if (!thenFlow.reachable) {
        flow = elseFlow;
        return;
    }
    if (!elseFlow.reachable) {
        flow = thenFlow;
        return;
    }

    std::set<const Symbol*> merged;
    for (const Symbol* symbol : thenFlow.assigned) {
        if (elseFlow.assigned.count(symbol) != 0) {
            merged.insert(symbol);
        }
    }
    flow.assigned = std::move(merged);
    flow.reachable = true;
}

void Sema::checkWhile(Stmt& stmt, Scope& scope, Flow& flow) {
    const Type* condType = checkExpr(*stmt.cond, scope, flow);
    if (!isScalarCondition(condType) && !condType->isError()) {
        m_diagnostics.error(stmt.cond->range,
                            "the condition of a 'while' must be a scalar, found '" +
                                condType->describe() + "'");
    }
    if (stmt.cond->kind == ExprKind::Assign) {
        m_diagnostics.warning(stmt.cond->range,
                              "suspicious assignment inside a condition; use '==' to compare");
    }

    Flow bodyFlow = flow;
    ++m_loopDepth;
    ++m_branchDepth;
    if (!stmt.label.empty()) {
        m_labels.insert(stmt.label);
    }
    if (stmt.thenS) {
        if (stmt.thenS->kind == StmtKind::Block) {
            checkScopedList(stmt.thenS->body, scope, bodyFlow);
        } else {
            Scope* inner = &m_scopes.emplace_back(&scope);
            checkStmt(*stmt.thenS, *inner, bodyFlow);
        }
    }
    if (!stmt.label.empty()) {
        m_labels.erase(stmt.label);
    }
    --m_branchDepth;
    --m_loopDepth;
    // A loop body may run zero times, so its assignments do not count after it.
}

void Sema::checkFor(Stmt& stmt, Scope& scope, Flow& flow) {
    Scope* forScope = &m_scopes.emplace_back(&scope);
    Flow forFlow = flow;

    if (stmt.initS) {
        checkStmt(*stmt.initS, *forScope, forFlow);
    }
    if (stmt.cond) {
        const Type* condType = checkExpr(*stmt.cond, *forScope, forFlow);
        if (!isScalarCondition(condType) && !condType->isError()) {
            m_diagnostics.error(stmt.cond->range,
                                "the condition of a 'for' must be a scalar, found '" +
                                    condType->describe() + "'");
        }
    }

    ++m_loopDepth;
    ++m_branchDepth;
    if (!stmt.label.empty()) {
        m_labels.insert(stmt.label);
    }
    Flow bodyFlow = forFlow;
    if (stmt.thenS) {
        if (stmt.thenS->kind == StmtKind::Block) {
            checkScopedList(stmt.thenS->body, *forScope, bodyFlow);
        } else {
            Scope* inner = &m_scopes.emplace_back(forScope);
            checkStmt(*stmt.thenS, *inner, bodyFlow);
        }
    }
    if (stmt.post) {
        checkExpr(*stmt.post, *forScope, bodyFlow);
    }
    if (!stmt.label.empty()) {
        m_labels.erase(stmt.label);
    }
    --m_branchDepth;
    --m_loopDepth;
}

void Sema::checkForIn(Stmt& stmt, Scope& scope, Flow& flow) {
    // `for x in <range|array|string>`
    Scope* loopScope = &m_scopes.emplace_back(&scope);
    const Type* elementType = m_types.errorType();

    if (stmt.cond->kind == ExprKind::Range) {
        const Type* lowType = checkExpr(*stmt.cond->kids[0], scope, flow);
        const Type* highType = checkExpr(*stmt.cond->kids[1], scope, flow);
        if (!lowType->isIntegerLike() || !highType->isIntegerLike()) {
            m_diagnostics.error(stmt.cond->range,
                                "a range in 'for ... in' requires integer bounds");
        }
        elementType = m_types.getInt(32, false);
    } else {
        const Type* iterable = checkExpr(*stmt.cond, scope, flow);
        if (iterable->kind == TypeKind::String) {
            elementType = m_types.charType();
        } else if (iterable->kind == TypeKind::Array) {
            elementType = iterable->elem;
        } else if (!iterable->isError()) {
            m_diagnostics.error(stmt.cond->range,
                                "cannot iterate over a value of type '" + iterable->describe() +
                                    "'; expected a range, an array or a 'string'");
        }
    }

    auto& symbol = m_symbols.emplace_back();
    symbol.kind = Symbol::Kind::Local;
    symbol.name = stmt.name;
    symbol.type = elementType;
    symbol.isMutable = true;
    symbol.loc = stmt.nameRange.begin;
    stmt.sym = &symbol;

    if (Symbol* existing = loopScope->declare(&symbol)) {
        m_diagnostics.error(stmt.nameRange, "redefinition of '" + stmt.name + "'");
        m_diagnostics.note(existing->loc, "previous declaration is here");
    }

    Flow bodyFlow = flow;
    bodyFlow.assigned.insert(&symbol);

    ++m_loopDepth;
    ++m_branchDepth;
    if (!stmt.label.empty()) {
        m_labels.insert(stmt.label);
    }
    if (stmt.thenS) {
        if (stmt.thenS->kind == StmtKind::Block) {
            checkScopedList(stmt.thenS->body, *loopScope, bodyFlow);
        } else {
            Scope* inner = &m_scopes.emplace_back(loopScope);
            checkStmt(*stmt.thenS, *inner, bodyFlow);
        }
    }
    if (!stmt.label.empty()) {
        m_labels.erase(stmt.label);
    }
    --m_branchDepth;
    --m_loopDepth;
}

void Sema::checkSwitch(Stmt& stmt, Scope& scope, Flow& flow) {
    const Type* subject = checkExpr(*stmt.cond, scope, flow);
    if (!subject->isIntegerLike() && !subject->isError()) {
        m_diagnostics.error(stmt.cond->range,
                            "a 'switch' subject must be an integer, 'char' or 'bool', found '" +
                                subject->describe() + "'");
    }

    Scope* switchScope = &m_scopes.emplace_back(&scope);
    ++m_switchDepth;

    std::set<std::string> seenValues;
    bool hasDefault = false;

    for (std::size_t i = 0; i < stmt.cases.size(); ++i) {
        CaseClause& clause = stmt.cases[i];
        if (clause.value) {
            ConstValue constant;
            if (!evalConst(*clause.value, scope, constant)) {
                m_diagnostics.error(clause.value->range,
                                    "a 'case' value must be a compile time constant");
            } else {
                const std::string key = constant.isFloat
                                            ? std::to_string(constant.f)
                                            : formatInt128(constant.i);
                if (!seenValues.insert(key).second) {
                    m_diagnostics.error(clause.value->range,
                                        "duplicate 'case' value " + key);
                }
            }
        } else {
            if (hasDefault) {
                m_diagnostics.error(clause.range, "duplicate 'default' clause");
            }
            if (i + 1 != stmt.cases.size()) {
                m_diagnostics.error(clause.range,
                                    "'default' must be the last clause of a switch");
            }
            hasDefault = true;
        }

        if (clause.hasFallthrough && i + 1 == stmt.cases.size()) {
            m_diagnostics.error(clause.range,
                                "'fallthrough' must not be the last statement of a switch");
        }

        Flow caseFlow = flow;
        checkStmtList(clause.body, *switchScope, caseFlow);
    }

    --m_switchDepth;
    // Conservatively: assignments inside the cases do not count afterwards.
}

void Sema::checkVirtual(Stmt& stmt, Scope& scope, Flow& flow) {
    if (m_inEof) {
        m_diagnostics.error(stmt.range, "'virtual' is not allowed inside an eof block");
        return;
    }
    if (m_virtualDepth > 0) {
        m_diagnostics.error(stmt.range, "a 'virtual' block cannot be nested");
    }
    if (m_branchDepth == 0) {
        m_diagnostics.error(stmt.range,
                            "'virtual' must appear inside a branch (an 'if', loop or block "
                            "nested inside the function body), not at the top level of a function");
    }

    ++m_virtualDepth;
    ++m_branchDepth;
    Scope* virtualScope = &m_scopes.emplace_back(&scope);
    Flow virtualFlow = flow;
    checkStmtList(stmt.body, *virtualScope, virtualFlow);

    if (stmt.hasEof) {
        if (stmt.eofExplicitEmpty && !m_outerEofEmpty) {
            m_diagnostics.warning(stmt.range,
                                  "the 'eof' of this virtual block is empty while the enclosing "
                                  "function has cleanup code; resources may leak");
        }
        checkStmtList(stmt.eofBody, *virtualScope, virtualFlow);
    }

    --m_branchDepth;
    --m_virtualDepth;

    if (m_returnType != nullptr && !m_returnType->isVoid()) {
        bool hasReturn = false;
        for (const StmtPtr& inner : stmt.body) {
            if (alwaysReturns(*inner)) {
                hasReturn = true;
                break;
            }
        }
        if (!hasReturn) {
            m_diagnostics.error(stmt.range,
                                "a 'virtual' block inside a function returning '" +
                                    m_returnType->describe() + "' must contain a 'return'");
        }
    }
}

void Sema::checkDestructure(Stmt& stmt, Scope& scope, Flow& flow) {
    const Type* valueType = checkExpr(*stmt.init, scope, flow);
    if (valueType->isError()) {
        return;
    }
    if (valueType->kind != TypeKind::Tuple) {
        m_diagnostics.error(stmt.init->range,
                            "tuple destructuring requires a tuple value, found '" +
                                valueType->describe() + "'");
        return;
    }
    if (valueType->elems.size() != stmt.targets.size()) {
        m_diagnostics.error(stmt.range,
                            "the tuple has " + std::to_string(valueType->elems.size()) +
                                " elements but " + std::to_string(stmt.targets.size()) +
                                " targets were given");
        return;
    }

    for (std::size_t i = 0; i < stmt.targets.size(); ++i) {
        Symbol* symbol = scope.lookup(stmt.targets[i]);
        if (symbol == nullptr) {
            m_diagnostics.error(stmt.targetRanges[i],
                                "undefined variable '" + stmt.targets[i] + "'");
            continue;
        }
        if (!symbol->isMutable) {
            m_diagnostics.error(stmt.targetRanges[i],
                                "cannot assign to const variable '" + stmt.targets[i] + "'");
        }
        if (!m_types.isAssignable(valueType->elems[i], symbol->type)) {
            m_diagnostics.error(stmt.targetRanges[i],
                                "element " + std::to_string(i + 1) + " of type '" +
                                    valueType->elems[i]->describe() +
                                    "' cannot be assigned to '" + stmt.targets[i] + "' of type '" +
                                    symbol->type->describe() + "'");
        }
        stmt.targetSymbols.push_back(symbol);
        flow.assigned.insert(symbol);
    }
}

} // namespace acvm

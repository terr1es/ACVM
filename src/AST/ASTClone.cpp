//===--- ASTClone.cpp - Deep copies of syntax tree nodes ----------------===//

#include "acvm/AST/ASTClone.h"

namespace acvm {

TypeNodePtr cloneTypeNode(const TypeNode& node) {
    auto copy = std::make_unique<TypeNode>();
    copy->kind = node.kind;
    copy->range = node.range;
    copy->base = node.base;
    copy->isUnsigned = node.isUnsigned;
    copy->bits = node.bits;
    copy->mantissa = node.mantissa;
    copy->exponent = node.exponent;
    copy->spelling = node.spelling;
    copy->name = node.name;
    copy->isReadOnlyPtr = node.isReadOnlyPtr;
    copy->typeArgs.reserve(node.typeArgs.size());
    for (const TypeNodePtr& arg : node.typeArgs) {
        copy->typeArgs.push_back(arg != nullptr ? cloneTypeNode(*arg) : nullptr);
    }
    if (node.inner != nullptr) {
        copy->inner = cloneTypeNode(*node.inner);
    }
    if (node.arrayLen != nullptr) {
        copy->arrayLen = cloneExpr(*node.arrayLen);
    }
    copy->elems.reserve(node.elems.size());
    for (const TypeNodePtr& elem : node.elems) {
        copy->elems.push_back(elem != nullptr ? cloneTypeNode(*elem) : nullptr);
    }
    // `resolved` stays null: the copy has not been through Sema yet, and that is
    // the whole point of copying.
    return copy;
}

ExprPtr cloneExpr(const Expr& expr) {
    auto copy = std::make_unique<Expr>(expr.kind, expr.range);
    copy->text = expr.text;
    copy->lit = expr.lit;
    copy->unop = expr.unop;
    copy->binop = expr.binop;
    copy->assignop = expr.assignop;
    if (expr.typeNode != nullptr) {
        copy->typeNode = cloneTypeNode(*expr.typeNode);
    }
    copy->kids.reserve(expr.kids.size());
    for (const ExprPtr& kid : expr.kids) {
        copy->kids.push_back(kid != nullptr ? cloneExpr(*kid) : nullptr);
    }
    // Sema results are deliberately not copied: type, sym, funcIndex, builtin,
    // memberIndex, isLValue, the const-expression payload and the literal
    // adaptation flags all describe the original node's place in the original
    // instantiation.
    return copy;
}

StmtPtr cloneStmt(const Stmt& stmt) {
    auto copy = std::make_unique<Stmt>(stmt.kind, stmt.range);

    copy->name = stmt.name;
    copy->nameRange = stmt.nameRange;
    copy->isConst = stmt.isConst;
    copy->isComptime = stmt.isComptime;
    copy->isGlobal = stmt.isGlobal;

    if (stmt.type != nullptr) {
        copy->type = cloneTypeNode(*stmt.type);
    }
    copy->dims.reserve(stmt.dims.size());
    for (const ExprPtr& dim : stmt.dims) {
        copy->dims.push_back(dim != nullptr ? cloneExpr(*dim) : nullptr);
    }
    if (stmt.init != nullptr) {
        copy->init = cloneExpr(*stmt.init);
    }

    copy->funcKind = stmt.funcKind;
    copy->typeParams = stmt.typeParams;
    copy->params.reserve(stmt.params.size());
    for (const Param& param : stmt.params) {
        Param cloned;
        cloned.type = param.type != nullptr ? cloneTypeNode(*param.type) : nullptr;
        cloned.name = param.name;
        cloned.range = param.range;
        cloned.isConst = param.isConst;
        cloned.isReceiver = param.isReceiver;
        // resolvedType / sym belong to the original instantiation.
        copy->params.push_back(std::move(cloned));
    }
    if (stmt.retType != nullptr) {
        copy->retType = cloneTypeNode(*stmt.retType);
    }
    if (stmt.defaultRet != nullptr) {
        copy->defaultRet = cloneExpr(*stmt.defaultRet);
    }
    if (stmt.eventCond != nullptr) {
        copy->eventCond = cloneExpr(*stmt.eventCond);
    }
    copy->body.reserve(stmt.body.size());
    for (const StmtPtr& inner : stmt.body) {
        copy->body.push_back(inner != nullptr ? cloneStmt(*inner) : nullptr);
    }
    copy->eofBody.reserve(stmt.eofBody.size());
    for (const StmtPtr& inner : stmt.eofBody) {
        copy->eofBody.push_back(inner != nullptr ? cloneStmt(*inner) : nullptr);
    }
    copy->hasEof = stmt.hasEof;
    copy->eofExplicitEmpty = stmt.eofExplicitEmpty;
    copy->isForward = stmt.isForward;
    copy->isCf = stmt.isCf;
    copy->externalLibrary = stmt.externalLibrary;
    copy->modules = stmt.modules;
    copy->modulesArePaths = stmt.modulesArePaths;

    copy->fields.reserve(stmt.fields.size());
    for (const FieldDecl& field : stmt.fields) {
        FieldDecl cloned;
        cloned.type = field.type != nullptr ? cloneTypeNode(*field.type) : nullptr;
        cloned.name = field.name;
        cloned.range = field.range;
        copy->fields.push_back(std::move(cloned));
    }
    // Methods are cloned like any other declaration. Structs are file-scope only,
    // so a generic function cannot contain one today -- but cloning is cheap and a
    // silently dropped method would be a very confusing bug if that ever changes.
    copy->methods.reserve(stmt.methods.size());
    for (const StmtPtr& method : stmt.methods) {
        copy->methods.push_back(method != nullptr ? cloneStmt(*method) : nullptr);
    }
    copy->items.reserve(stmt.items.size());
    for (const EnumItem& item : stmt.items) {
        EnumItem cloned;
        cloned.name = item.name;
        cloned.value = item.value != nullptr ? cloneExpr(*item.value) : nullptr;
        cloned.range = item.range;
        cloned.evaluated = item.evaluated;
        copy->items.push_back(std::move(cloned));
    }

    if (stmt.cond != nullptr) {
        copy->cond = cloneExpr(*stmt.cond);
    }
    if (stmt.thenS != nullptr) {
        copy->thenS = cloneStmt(*stmt.thenS);
    }
    if (stmt.elseS != nullptr) {
        copy->elseS = cloneStmt(*stmt.elseS);
    }
    if (stmt.initS != nullptr) {
        copy->initS = cloneStmt(*stmt.initS);
    }
    if (stmt.post != nullptr) {
        copy->post = cloneExpr(*stmt.post);
    }
    copy->cases.reserve(stmt.cases.size());
    for (const CaseClause& clause : stmt.cases) {
        CaseClause cloned;
        cloned.value = clause.value != nullptr ? cloneExpr(*clause.value) : nullptr;
        cloned.body.reserve(clause.body.size());
        for (const StmtPtr& inner : clause.body) {
            cloned.body.push_back(inner != nullptr ? cloneStmt(*inner) : nullptr);
        }
        cloned.hasFallthrough = clause.hasFallthrough;
        cloned.range = clause.range;
        copy->cases.push_back(std::move(cloned));
    }
    copy->label = stmt.label;
    copy->targets = stmt.targets;
    copy->targetRanges = stmt.targetRanges;
    // targetSymbols / sym / funcIndex / varType / structType / enumType are
    // Sema results; the copy starts unanalysed.
    copy->implicitEof = stmt.implicitEof;
    return copy;
}

} // namespace acvm

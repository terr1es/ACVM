//===--- AST.cpp - Destructors, operator names and the tree printer -----===//
#include "acvm/AST/AST.h"

#include "acvm/AST/ASTPrinter.h"
#include "acvm/Sema/Type.h"

#include <ostream>
#include <sstream>
#include <string>

namespace acvm {

Expr::~Expr() = default;
TypeNode::~TypeNode() = default;
Stmt::~Stmt() = default;

const char* unOpName(UnOp op) {
    switch (op) {
    case UnOp::Plus: return "+";
    case UnOp::Neg: return "-";
    case UnOp::Not: return "!";
    case UnOp::BitNot: return "~";
    case UnOp::Deref: return "@";
    case UnOp::AddrOf: return "#";
    }
    return "?";
}

const char* binOpName(BinOp op) {
    switch (op) {
    case BinOp::Add: return "+";
    case BinOp::Sub: return "-";
    case BinOp::Mul: return "*";
    case BinOp::Div: return "/";
    case BinOp::Mod: return "%";
    case BinOp::Pow: return "**";
    case BinOp::Shl: return "<<";
    case BinOp::Shr: return ">>";
    case BinOp::Lt: return "<";
    case BinOp::Le: return "<=";
    case BinOp::Gt: return ">";
    case BinOp::Ge: return ">=";
    case BinOp::Eq: return "==";
    case BinOp::Ne: return "!=";
    case BinOp::BitAnd: return "&";
    case BinOp::BitXor: return "^";
    case BinOp::BitOr: return "|";
    case BinOp::LogAnd: return "&&";
    case BinOp::LogOr: return "||";
    }
    return "?";
}

const char* assignOpName(AssignOp op) {
    switch (op) {
    case AssignOp::Assign: return "=";
    case AssignOp::Add: return "+=";
    case AssignOp::Sub: return "-=";
    case AssignOp::Mul: return "*=";
    case AssignOp::Div: return "/=";
    case AssignOp::Mod: return "%=";
    case AssignOp::BitAnd: return "&=";
    case AssignOp::BitOr: return "|=";
    case AssignOp::BitXor: return "^=";
    case AssignOp::Shl: return "<<=";
    case AssignOp::Shr: return ">>=";
    }
    return "?";
}

namespace {

class Printer {
public:
    explicit Printer(std::ostream& out) : m_out(out) {}

    void print(const Unit& unit) {
        m_out << "Unit\n";
        for (std::size_t i = 0; i < unit.decls.size(); ++i) {
            printStmt(*unit.decls[i], "", i + 1 == unit.decls.size());
        }
    }

private:
    static std::string typeText(const TypeNode& node) {
        switch (node.kind) {
        case TypeNode::Kind::Named:
            return node.name;
        case TypeNode::Kind::Ptr:
            return "ptr " + (node.inner ? typeText(*node.inner) : std::string("<null>"));
        case TypeNode::Kind::Vector:
            return "vector " + (node.inner ? typeText(*node.inner) : std::string("<null>"));
        case TypeNode::Kind::Array:
            return (node.inner ? typeText(*node.inner) : std::string("<null>")) + "[...]";
        case TypeNode::Kind::Tuple: {
            std::string result = "(";
            for (std::size_t i = 0; i < node.elems.size(); ++i) {
                if (i != 0) {
                    result += ", ";
                }
                result += node.elems[i] ? typeText(*node.elems[i]) : std::string("<null>");
            }
            return result + ")";
        }
        case TypeNode::Kind::Scalar:
            break;
        }
        switch (node.base) {
        case ScalarBase::Bool: return "bool";
        case ScalarBase::Char: return "char";
        case ScalarBase::Void: return "void";
        case ScalarBase::Float:
            if (node.mantissa != 0 || node.exponent != 0) {
                return "double" + std::to_string(node.mantissa) + "." +
                       std::to_string(node.exponent);
            }
            return "float" + std::to_string(node.bits == 0 ? 32 : node.bits);
        case ScalarBase::Int:
            break;
        case ScalarBase::Bit:
            return "bit" + std::to_string(node.bits);
        }
        return (node.isUnsigned ? "unsigned int" : "int") +
               std::to_string(node.bits == 0 ? 32 : node.bits);
    }

    void line(const std::string& prefix, bool last, const std::string& text) {
        m_out << prefix << (last ? "`- " : "|- ") << text << '\n';
    }

    static std::string childPrefix(const std::string& prefix, bool last) {
        return prefix + (last ? "   " : "|  ");
    }

    void printStmt(const Stmt& stmt, const std::string& prefix, bool last) {
        const std::string next = childPrefix(prefix, last);

        switch (stmt.kind) {
        case StmtKind::VarDecl: {
            std::ostringstream header;
            if (stmt.isGlobal) {
                header << "global ";
            }
            if (stmt.isConst) {
                header << "const ";
            }
            if (stmt.isComptime) {
                header << "comptime ";
            }
            header << (stmt.type ? typeText(*stmt.type) : "<notype>") << ' ' << stmt.name;
            for (const ExprPtr& dim : stmt.dims) {
                header << '[' << (dim ? "n" : "?") << ']';
            }
            line(prefix, last, header.str());
            if (stmt.init) {
                printExpr(*stmt.init, next, true);
            }
            return;
        }
        case StmtKind::FuncDecl: {
            std::ostringstream header;
            header << "function";
            switch (stmt.funcKind) {
            case FuncKind::Private: header << " private"; break;
            case FuncKind::Global: header << " global"; break;
            case FuncKind::Event: header << " event"; break;
            case FuncKind::Process: header << " process"; break;
            case FuncKind::Import: header << " import"; break;
            case FuncKind::Normal: break;
            }
            header << ' ' << stmt.name << '(';
            for (std::size_t i = 0; i < stmt.params.size(); ++i) {
                if (i != 0) {
                    header << ", ";
                }
                if (stmt.params[i].isConst) {
                    header << "const ";
                }
                header << (stmt.params[i].type ? typeText(*stmt.params[i].type) : "<notype>");
                // A receiver is written without a name in the source, so printing
                // `self self` would misrepresent the declaration it came from.
                if (!stmt.params[i].isReceiver) {
                    header << ' ' << stmt.params[i].name;
                }
            }
            header << ')';
            if (stmt.eventCond) {
                header << " (event condition)";
            }
            header << " : " << (stmt.retType ? typeText(*stmt.retType) : "<notype>");
            if (stmt.isForward) {
                header << " [forward]";
            }
            line(prefix, last, header.str());

            for (std::size_t i = 0; i < stmt.body.size(); ++i) {
                const bool isLast = i + 1 == stmt.body.size() && !stmt.hasEof;
                printStmt(*stmt.body[i], next, isLast);
            }
            if (stmt.hasEof) {
                line(next, true, "eof");
                const std::string eofPrefix = childPrefix(next, true);
                for (std::size_t i = 0; i < stmt.eofBody.size(); ++i) {
                    printStmt(*stmt.eofBody[i], eofPrefix, i + 1 == stmt.eofBody.size());
                }
            }
            return;
        }
        case StmtKind::ImportDecl: {
            std::ostringstream header;
            header << "import";
            if (!stmt.name.empty()) {
                header << ' ' << stmt.name;
            }
            header << " (";
            for (std::size_t i = 0; i < stmt.modules.size(); ++i) {
                if (i != 0) {
                    header << ", ";
                }
                header << stmt.modules[i].first;
            }
            header << ')';
            if (stmt.isCf) {
                header << " is_cf";
            }
            line(prefix, last, header.str());
            return;
        }
        case StmtKind::StructDecl:
        case StmtKind::UnionDecl: {
            line(prefix, last,
                 std::string(stmt.kind == StmtKind::UnionDecl ? "union " : "struct ") + stmt.name);
            // Fields first, then methods, so the layout stays readable at the top
            // -- methods contribute no storage and are the less important half
            // when you are checking what the struct actually looks like.
            const std::size_t total = stmt.fields.size() + stmt.methods.size();
            for (std::size_t i = 0; i < stmt.fields.size(); ++i) {
                const FieldDecl& field = stmt.fields[i];
                line(next, i + 1 == total,
                     (field.type ? typeText(*field.type) : "<notype>") + " " + field.name + ";");
            }
            for (std::size_t i = 0; i < stmt.methods.size(); ++i) {
                printStmt(*stmt.methods[i], next, i + 1 == stmt.methods.size() && i + 1 == total);
            }
            return;
        }
        case StmtKind::EnumDecl: {
            line(prefix, last, "enum " + stmt.name);
            for (std::size_t i = 0; i < stmt.items.size(); ++i) {
                line(next, i + 1 == stmt.items.size(), stmt.items[i].name);
            }
            return;
        }
        case StmtKind::ExprStmt:
            line(prefix, last, "expr");
            if (stmt.init) {
                printExpr(*stmt.init, next, true);
            }
            return;
        case StmtKind::Block: {
            line(prefix, last, "block");
            for (std::size_t i = 0; i < stmt.body.size(); ++i) {
                printStmt(*stmt.body[i], next, i + 1 == stmt.body.size());
            }
            return;
        }
        case StmtKind::If: {
            line(prefix, last, "if");
            if (stmt.cond) {
                printExpr(*stmt.cond, next, false);
            }
            if (stmt.thenS) {
                printStmt(*stmt.thenS, next, stmt.elseS == nullptr);
            }
            if (stmt.elseS) {
                line(next, false, "else");
                printStmt(*stmt.elseS, childPrefix(next, false), true);
            }
            return;
        }
        case StmtKind::While: {
            line(prefix, last, stmt.label.empty() ? "while" : "while @" + stmt.label);
            if (stmt.cond) {
                printExpr(*stmt.cond, next, false);
            }
            if (stmt.thenS) {
                printStmt(*stmt.thenS, next, true);
            }
            return;
        }
        case StmtKind::For: {
            line(prefix, last, "for");
            if (stmt.initS) {
                printStmt(*stmt.initS, next, false);
            }
            if (stmt.cond) {
                printExpr(*stmt.cond, next, false);
            }
            if (stmt.post) {
                printExpr(*stmt.post, next, false);
            }
            if (stmt.thenS) {
                printStmt(*stmt.thenS, next, true);
            }
            return;
        }
        case StmtKind::ForIn: {
            line(prefix, last, "for " + stmt.name + " in");
            if (stmt.cond) {
                printExpr(*stmt.cond, next, false);
            }
            if (stmt.thenS) {
                printStmt(*stmt.thenS, next, true);
            }
            return;
        }
        case StmtKind::Loop:
            line(prefix, last, "loop");
            if (stmt.cond) {
                printExpr(*stmt.cond, next, false);
            }
            if (stmt.thenS) {
                printStmt(*stmt.thenS, next, true);
            }
            return;
        case StmtKind::Switch: {
            line(prefix, last, "switch");
            if (stmt.cond) {
                printExpr(*stmt.cond, next, false);
            }
            for (std::size_t i = 0; i < stmt.cases.size(); ++i) {
                const CaseClause& clause = stmt.cases[i];
                const bool clauseLast = i + 1 == stmt.cases.size();
                if (clause.value) {
                    line(next, false, "case");
                    printExpr(*clause.value, childPrefix(next, false), false);
                } else {
                    line(next, false, "default");
                }
                const std::string bodyPrefix = childPrefix(next, false);
                std::size_t count = clause.body.size();
                for (std::size_t k = 0; k < count; ++k) {
                    printStmt(*clause.body[k], bodyPrefix, k + 1 == count);
                }
                (void)clauseLast;
            }
            return;
        }
        case StmtKind::Break:
            line(prefix, last, stmt.label.empty() ? "break" : "break " + stmt.label);
            return;
        case StmtKind::Continue:
            line(prefix, last, stmt.label.empty() ? "continue" : "continue " + stmt.label);
            return;
        case StmtKind::Return:
            line(prefix, last, "return");
            if (stmt.init) {
                printExpr(*stmt.init, next, true);
            }
            return;
        case StmtKind::Pass:
            line(prefix, last, "pass");
            return;
        case StmtKind::Next:
            line(prefix, last, "next");
            return;
        case StmtKind::ArenaScope:
            line(prefix, last, "arena_scope");
            for (std::size_t i = 0; i < stmt.body.size(); ++i) {
                printStmt(*stmt.body[i], next, i + 1 == stmt.body.size());
            }
            return;
        case StmtKind::Virtual: {
            line(prefix, last, "virtual");
            for (std::size_t i = 0; i < stmt.body.size(); ++i) {
                printStmt(*stmt.body[i], next, i + 1 == stmt.body.size() && !stmt.hasEof);
            }
            if (stmt.hasEof) {
                line(next, true, "eof");
                const std::string eofPrefix = childPrefix(next, true);
                for (std::size_t i = 0; i < stmt.eofBody.size(); ++i) {
                    printStmt(*stmt.eofBody[i], eofPrefix, i + 1 == stmt.eofBody.size());
                }
            }
            return;
        }
        case StmtKind::Destructure: {
            std::string header = "destructure (";
            for (std::size_t i = 0; i < stmt.targets.size(); ++i) {
                if (i != 0) {
                    header += ", ";
                }
                header += stmt.targets[i];
            }
            header += ")";
            line(prefix, last, header);
            if (stmt.init) {
                printExpr(*stmt.init, next, true);
            }
            return;
        }
        case StmtKind::Empty:
            line(prefix, last, ";");
            return;
        }
    }

    void printExpr(const Expr& expr, const std::string& prefix, bool last) {
        const std::string next = childPrefix(prefix, last);

        switch (expr.kind) {
        case ExprKind::IntLit:
            line(prefix, last, "int " + expr.lit.text);
            return;
        case ExprKind::FloatLit:
            line(prefix, last, "float " + expr.lit.text);
            return;
        case ExprKind::CharLit:
            line(prefix, last, "char " + std::to_string(expr.lit.charValue));
            return;
        case ExprKind::StringLit:
            line(prefix, last, "string \"" + expr.lit.stringValue + "\"");
            return;
        case ExprKind::BoolLit:
            line(prefix, last, std::string("bool ") + (expr.lit.intValue != 0 ? "true" : "false"));
            return;
        case ExprKind::NullLit:
            line(prefix, last, "null");
            return;
        case ExprKind::Name:
            line(prefix, last, "name " + expr.text);
            return;
        case ExprKind::Unary:
            line(prefix, last, std::string("unary ") + unOpName(expr.unop));
            break;
        case ExprKind::Binary:
            line(prefix, last, std::string("binary ") + binOpName(expr.binop));
            break;
        case ExprKind::Assign:
            line(prefix, last, std::string("assign ") + assignOpName(expr.assignop));
            break;
        case ExprKind::Ternary:
            line(prefix, last, "ternary");
            break;
        case ExprKind::Cast:
            line(prefix, last, "cast as " +
                                   (expr.typeNode ? typeText(*expr.typeNode) : "<notype>"));
            break;
        case ExprKind::Call:
            line(prefix, last, "call");
            break;
        case ExprKind::Index:
            line(prefix, last, "index");
            break;
        case ExprKind::Slice:
            line(prefix, last, "slice");
            break;
        case ExprKind::Member:
            line(prefix, last, "." + expr.text);
            break;
        case ExprKind::Arrow:
            line(prefix, last, "->" + expr.text);
            break;
        case ExprKind::PreInc:
            line(prefix, last, "pre ++");
            break;
        case ExprKind::PreDec:
            line(prefix, last, "pre --");
            break;
        case ExprKind::PostInc:
            line(prefix, last, "post ++");
            break;
        case ExprKind::PostDec:
            line(prefix, last, "post --");
            break;
        case ExprKind::TupleLit:
            line(prefix, last, "tuple");
            break;
        case ExprKind::InitList:
            line(prefix, last, "init");
            break;
        case ExprKind::SizeOf:
            line(prefix, last, "sizeof");
            return;
        case ExprKind::AlignOf:
            line(prefix, last, "alignof");
            return;
        case ExprKind::Range:
            line(prefix, last, "range");
            break;
        }

        for (std::size_t i = 0; i < expr.kids.size(); ++i) {
            printExpr(*expr.kids[i], next, i + 1 == expr.kids.size());
        }
    }

    std::ostream& m_out;
};

} // namespace

void dumpAst(const Unit& unit, std::ostream& out) {
    Printer printer(out);
    printer.print(unit);
}

} // namespace acvm

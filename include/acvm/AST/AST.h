//===--- AST.h - Axiom syntax tree -------------------------------------===//
#pragma once

#include "acvm/Common/SourceLocation.h"
#include "acvm/Common/Value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace acvm {

struct Expr;
struct TypeNode;
struct Stmt;
struct Symbol;
class Type;

using ExprPtr = std::unique_ptr<Expr>;
using TypeNodePtr = std::unique_ptr<TypeNode>;
using StmtPtr = std::unique_ptr<Stmt>;

//===----------------------------------------------------------------------===//
// Types as written in source (spec §2.3)
//===----------------------------------------------------------------------===//

enum class ScalarBase : std::uint8_t { Int, Float, Char, Bool, Void, Bit };

struct TypeNode {
    enum class Kind : std::uint8_t { Scalar, Named, Ptr, Array, Tuple, Vector };

    Kind kind = Kind::Scalar;
    SourceRange range;

    // Scalar
    ScalarBase base = ScalarBase::Int;
    bool isUnsigned = false;
    unsigned bits = 0;                  // 0 = type default (int 32 / float 32)
    /// Set when a `bitN` width was written in power form (`bit2u20`): the exponent
    /// is kept only so a rejected width can say "found 2097152 (2^21)" instead of
    /// printing a bare number the reader has to decode.
    unsigned powerExponent = 0;
    /// The base of a power-form width, kept only to reject `bit3u4` with a message
    /// that names the base instead of reporting a meaningless width.
    unsigned powerBase = 0;
    unsigned mantissa = 0;              // doubleA.B
    unsigned exponent = 0;
    std::string spelling;               // original text, for diagnostics

    // Named (struct / enum)
    std::string name;

    /// Type arguments written after the name: `Pair<int>` gives one entry.
    /// Empty for a plain named type. A name that is a generic struct template
    /// must have them; one that is not must not.
    std::vector<TypeNodePtr> typeArgs;

    // Ptr / Array / Vector
    TypeNodePtr inner;

    /// `ptr const T`. Only meaningful where a pointer crosses into a native
    /// function: it promises the callee does not write through it, which lets
    /// the VM skip copying the array back. Outside that setting it is accepted
    /// and ignored -- there is no mutation checking in Axiom itself.
    bool isReadOnlyPtr = false;

    // Array
    ExprPtr arrayLen;

    // Tuple
    std::vector<TypeNodePtr> elems;

    // filled by Sema
    const Type* resolved = nullptr;

    TypeNode() = default;
    ~TypeNode();
    TypeNode(const TypeNode&) = delete;
    TypeNode& operator=(const TypeNode&) = delete;
};

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

enum class ExprKind : std::uint8_t {
    IntLit,
    FloatLit,
    CharLit,
    StringLit,
    BoolLit,
    NullLit,
    Name,
    Unary,
    Binary,
    Assign,
    Ternary,
    Cast,
    Call,
    Index,
    Slice,
    Member,
    Arrow,
    PreInc,
    PreDec,
    PostInc,
    PostDec,
    TupleLit,
    InitList,
    SizeOf,
    AlignOf,
    Range,
};

/// Pseudo member indices filled in by Sema for built-in members.
inline constexpr int kMemberNone = -1;
inline constexpr int kMemberLength = -2;
inline constexpr int kMemberPtr = -3;

enum class UnOp : std::uint8_t { Plus, Neg, Not, BitNot, Deref, AddrOf };
enum class BinOp : std::uint8_t {
    Add, Sub, Mul, Div, Mod, Pow, Shl, Shr, Lt, Le, Gt, Ge, Eq, Ne,
    BitAnd, BitXor, BitOr, LogAnd, LogOr,
};
enum class AssignOp : std::uint8_t {
    Assign, Add, Sub, Mul, Div, Mod, BitAnd, BitOr, BitXor, Shl, Shr,
};

const char* unOpName(UnOp op);
const char* binOpName(BinOp op);
const char* assignOpName(AssignOp op);

struct LitValue {
    Int128 intValue = 0;
    double floatValue = 0;
    std::uint32_t charValue = 0;
    std::string stringValue;   // decoded
    bool fits128 = true;

    enum class Suffix : std::uint8_t { None, I, U, F, D } suffix = Suffix::None;
    unsigned suffixBits = 0;
    std::string text;          // raw literal text for diagnostics
};

struct Expr {
    Expr(ExprKind k, SourceRange r) : kind(k), range(r) {}
    ~Expr();
    Expr(const Expr&) = delete;
    Expr& operator=(const Expr&) = delete;

    ExprKind kind;
    SourceRange range;

    std::string text;   // identifier / member name
    LitValue lit;
    UnOp unop = UnOp::Plus;
    BinOp binop = BinOp::Add;
    AssignOp assignop = AssignOp::Assign;
    std::vector<ExprPtr> kids;
    TypeNodePtr typeNode;   // cast target, sizeof / alignof operand

    // ---- filled in by Sema ----
    const Type* type = nullptr;
    Symbol* sym = nullptr;
    int funcIndex = -1;      // resolved callee
    int builtin = -1;        // Builtin id for intrinsic calls
    int memberIndex = -1;    // struct field index / built-in member id
    bool isLValue = false;
    bool isConstExpr = false;
    Int128 constInt = 0;
    bool constIsFloat = false;
    double constFloat = 0;
    /// Set when a literal must be materialised directly as its target type
    /// (spec §2.4.1.1), e.g. `int8 a = 100;`.
    bool literalTyped = false;
    /// Set when an unsuffixed integer literal does not fit its default type;
    /// a surrounding context may still retype it (spec §2.4.1.1).
    bool literalOutOfRange = false;
    /// A function used as a *value*: the resolved callee, or -1.
    ///
    /// Only a `Func<...>` declaration may name a function without calling it, and the
    /// declaration is where the signature is written -- which is also the only place
    /// overload resolution can run, because a call site cannot spell type arguments
    /// (`f<int>(x)` is indistinguishable from a comparison, see Parser.cpp). So this is
    /// filled in by Sema and read by IRGen, which emits the handle.
    int funcValue = -1;
    /// Set on a call whose callee is a `Func<...>` *value* rather than a name: the
    /// target is only known at run time, so IRGen emits CALL_INDIRECT.
    bool indirectCall = false;
    /// A scalar constant zero standing for a whole aggregate: `struct P p[200] = {0}`.
    ///
    /// Memcpy'ing from it would treat the 0 as an address, so IRGen emits nothing -- the
    /// declaration already zeroed the slot. Only Sema sets this, and only for a constant
    /// zero; any other scalar against an aggregate is an error there.
    bool aggregateZeroFill = false;
};

//===----------------------------------------------------------------------===//
// Statements and declarations
//===----------------------------------------------------------------------===//

enum class FuncKind : std::uint8_t { Normal, Private, Global, Event, Process, Import };

enum class StmtKind : std::uint8_t {
    VarDecl,
    FuncDecl,
    ImportDecl,
    StructDecl,
    /// `union U { ... }`: members share one storage slot. Parsed like a struct
    /// and laid out differently; that difference lives entirely in Sema.
    UnionDecl,
    EnumDecl,
    ExprStmt,
    Block,
    If,
    While,
    For,
    ForIn,
    Loop,
    Switch,
    Break,
    Continue,
    Return,
    Pass,
    Next,
    ArenaScope,
    Virtual,
    Destructure,
    Empty,
};

struct Param {
    TypeNodePtr type;
    std::string name;
    SourceRange range;
    bool isConst = false;
    /// True for the receiver of a method, which is written without a name:
    /// `function length(self):int` or `function scale(ptr self, int by):void`.
    /// Sema names it `self` so the body can refer to it, and marks it const
    /// unless the receiver is a pointer.
    bool isReceiver = false;

    const Type* resolvedType = nullptr;
    Symbol* sym = nullptr;
};

struct CaseClause {
    ExprPtr value;      // null for `default`
    std::vector<StmtPtr> body;
    bool hasFallthrough = false;
    SourceRange range;
};

struct FieldDecl {
    TypeNodePtr type;
    std::string name;
    SourceRange range;
    const Type* resolvedType = nullptr;
};

struct EnumItem {
    std::string name;
    ExprPtr value;      // optional
    SourceRange range;
    Int128 evaluated = 0;
};

struct Stmt {
    Stmt(StmtKind k, SourceRange r) : kind(k), range(r) {}
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    StmtKind kind;
    SourceRange range;

    // VarDecl / ImportDecl / FuncDecl
    std::string name;
    SourceRange nameRange;
    bool isConst = false;
    bool isComptime = false;
    bool isGlobal = false;

    TypeNodePtr type;
    std::vector<ExprPtr> dims;    // declarator array suffixes: `int a[10]`
    ExprPtr init;

    // FuncDecl
    FuncKind funcKind = FuncKind::Normal;
    /// Type parameters of a generic declaration: `function sort<T>(...)` or
    /// `struct Pair<T> { ... }` both give `{"T"}`. Empty for an ordinary one.
    ///
    /// Neither is compiled as written. Sema keeps the declaration as a template
    /// and, for every distinct set of type arguments it is used with, clones this
    /// node and checks the copy with the parameters bound -- so the code that
    /// reaches IRGen is always monomorphic.
    std::vector<std::string> typeParams;
    std::vector<Param> params;
    TypeNodePtr retType;
    ExprPtr defaultRet;
    ExprPtr eventCond;
    std::vector<StmtPtr> body;
    std::vector<StmtPtr> eofBody;
    bool hasEof = false;
    bool eofExplicitEmpty = false;
    bool isForward = false;
    bool isCf = false;
    /// Non-empty when this node came from a `library "x";` file: the symbol is
    /// implemented in that shared library. Set on merge (Driver::expandImports)
    /// so the information survives when imported declarations are spliced into
    /// the importing unit -- a unit-level flag would be lost there.
    std::string externalLibrary;
    std::vector<std::pair<std::string, SourceRange>> modules;
    /// True when the modules were written as file paths (`import "x.ax";`)
    /// rather than as module names (`return (Math);`). Paths are resolved
    /// verbatim against the importing file's directory.
    bool modulesArePaths = false;

    // StructDecl / UnionDecl / EnumDecl
    std::vector<FieldDecl> fields;
    std::vector<EnumItem> items;
    /// Methods declared inside a struct or union body. They contribute no
    /// storage, so the layout is exactly the field list -- a struct with methods
    /// still crosses to a native library unchanged.
    std::vector<StmtPtr> methods;

    // control flow
    ExprPtr cond;
    StmtPtr thenS;
    StmtPtr elseS;
    StmtPtr initS;
    ExprPtr post;
    std::vector<CaseClause> cases;
    std::string label;
    std::vector<std::string> targets;   // destructure names
    std::vector<SourceRange> targetRanges;
    std::vector<Symbol*> targetSymbols;

    // ---- filled in by Sema ----
    Symbol* sym = nullptr;
    int funcIndex = -1;             // index into the module's function table
    const Type* varType = nullptr;  // resolved declared type
    const Type* structType = nullptr;
    const Type* enumType = nullptr;
    bool implicitEof = true;
};

/// A whole translation unit: an ordered list of top level declarations.
struct Unit {
    std::vector<StmtPtr> decls;

    /// `library "name";` — when present, bodyless functions in this file are
    /// external: implemented in the named shared library rather than in Axiom.
    /// This is the whole of the dynamic-library syntax. Empty
    /// when the file is ordinary Axiom.
    std::string libraryName;
    SourceRange libraryRange{};

    /// `module "json" version "0.1.0";` — what this file *is*, for packaging.
    ///
    /// Kept separate from `libraryName`, which says where the code lives (a
    /// shared library with no Axiom body). A file may be both: `net.axd` declares
    /// `library "net"` because the bodies are in net.dll, and `module "net"
    /// version ...` because it is also the `net` module users import.
    ///
    /// Name and version are strings, not numbers: a version is `1.10.2`, which no
    /// numeric type would preserve, and the name is only ever compared for
    /// equality with the file's own stem.
    std::string moduleName;
    std::string moduleVersion;
    SourceRange moduleRange{};

    [[nodiscard]] bool isExternalLibrary() const { return !libraryName.empty(); }
    [[nodiscard]] bool declaresModule() const { return !moduleVersion.empty(); }
};

} // namespace acvm

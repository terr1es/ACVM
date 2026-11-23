//===--- Sema.h - Name resolution and type checking ---------------------===//
#pragma once

#include "acvm/AST/AST.h"
#include "acvm/Sema/Builtin.h"
#include "acvm/Sema/Type.h"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace acvm {

class DiagnosticEngine;

/// A named entity visible in a scope.
struct Symbol {
    enum class Kind : std::uint8_t {
        Error,
        Local,
        Param,
        Global,
        Constant,
        Comptime,
        Function,
        Builtin,
        EnumConstant,
        TypeName,
    };

    Kind kind = Kind::Error;
    std::string name;
    const Type* type = nullptr;
    SourceLocation loc{};

    bool isMutable = true;      // false for const / comptime / params declared const
    bool isConst = false;
    bool isComptime = false;

    int globalIndex = -1;
    std::vector<int> overloads;      // Function
    /// Generic functions with this name, as indices into Sema::m_templates.
    /// Kept separate from `overloads` because a template is not a callable
    /// function until it has been instantiated for concrete type arguments.
    std::vector<int> templates;
    Builtin builtin = Builtin::None;

    // Comptime / enum constant payload
    bool constIsFloat = false;
    Int128 constInt = 0;
    double constFloat = 0;

    // Enum member
    std::int64_t enumValue = 0;
    const Type* enumType = nullptr;
};

struct FuncInfo {
    std::string name;
    FuncKind kind = FuncKind::Normal;
    const Type* ret = nullptr;
    std::vector<const Type*> params;
    std::vector<std::string> paramNames;
    std::vector<bool> paramIsConst;
    /// `ptr const T` per parameter: the native callee promises not to write
    /// through the pointer, so the VM may skip copying the array back. Only
    /// consulted for external functions.
    std::vector<bool> paramPtrReadOnly;
    Stmt* decl = nullptr;
    bool hasBody = false;
    bool forwardDeclared = false;
    /// Set for a method declared in a struct body: the type it belongs to.
    /// A method is an ordinary function whose name is mangled to
    /// `Point.length` and whose first parameter is the receiver, so nothing
    /// downstream of Sema needs to know it was ever a method.
    const Type* ownerType = nullptr;
    /// True when parameter 0 is the receiver (`self` / `ptr self`).
    bool hasReceiver = false;
    /// Set while a generic struct's method exists as a signature but its body has
    /// not been checked, because nothing called it yet. Declaring the signature is
    /// what makes `p.sum()` resolve; checking the body is deferred because whether
    /// it is legal depends on the type arguments -- `Pair<T>::sum` needs `+`, which
    /// a `Pair<Point>` payload does not have. C++ does the same thing for the same
    /// reason, and without it one unusable method would poison every instance.
    ///
    /// `hasBody` is false while this is set, so IRGen emits nothing for a method
    /// that is never called.
    bool bodyDeferred = false;
    /// The type bindings to re-establish when the deferred body is finally checked.
    /// Checking happens later, from a call site where the instance's `T` is no
    /// longer in scope, so it has to be carried along.
    std::vector<std::pair<std::string, const Type*>> deferredBindings;
    /// True when the receiver is `ptr self` rather than `self`.
    ///
    /// Kept explicitly rather than inferred from `paramIsConst[0]`, because a
    /// receiver may also be written `const ptr self` -- a read-only pointer -- and
    /// then constness says nothing about whether it is a pointer.
    bool receiverByPointer = false;
    SourceLocation loc{};
    /// File that declared this function. `private` is file scoped (spec
    /// §2.2.1.2), so visibility checks compare it with the entry file.
    FileID file = kInvalidFileID;
    /// Non-empty when the declaration came from a `library "x";` file: the
    /// implementation is in that shared library, not in Axiom. Carried on the
    /// function rather than the unit because `import` merges units and the
    /// file-level marker would be lost.
    std::string externalLibrary;
};

struct GlobalInfo {
    std::string name;
    const Type* type = nullptr;
    bool isConst = false;
    Stmt* decl = nullptr;
    SourceLocation loc{};
};

/// A generic struct as written: `struct Pair<T> { T first; T second; }`.
///
/// Like a generic function, it is never compiled as written: every use with
/// concrete type arguments produces an instance with its own layout, its own
/// methods and its own `Type` node. `TypeKind` stays `Struct`, so nothing
/// downstream -- layout, member access, IRGen, the native boundary -- needs to
/// know the type was generic.
struct StructTemplate {
    std::string name;
    std::vector<std::string> typeParams;
    /// `StructDecl` or `UnionDecl`; a generic union is a union per instance.
    StmtKind kind = StmtKind::StructDecl;
    /// The declaration in the source unit. Owns nothing -- the unit does.
    Stmt* decl = nullptr;
    FileID file = kInvalidFileID;
    SourceLocation loc{};
    /// Set when the template was rejected (`library` file). Uses of it then
    /// resolve straight to the error type, so one real problem does not produce a
    /// second message blaming the use site.
    bool rejected = false;
};

/// A generic function as written: `function sort<T>(ptr T data, int count)`.
///
/// Not callable by itself. Every call infers concrete type arguments and asks
/// Sema to instantiate this template for them (monomorphisation), so the code
/// that reaches IRGen is always monomorphic and pays nothing for being generic.
struct FuncTemplate {
    std::string name;
    std::vector<std::string> typeParams;
    /// The declaration in the source unit. Owns nothing -- the unit does.
    Stmt* decl = nullptr;
    FuncKind kind = FuncKind::Normal;
    FileID file = kInvalidFileID;
    SourceLocation loc{};
};

class Scope {
public:
    explicit Scope(Scope* parent = nullptr) : m_parent(parent) {}

    /// Returns nullptr when the declaration succeeded, otherwise the existing
    /// symbol in this very scope.
    Symbol* declare(Symbol* symbol);
    [[nodiscard]] Symbol* lookupLocal(std::string_view name) const;
    [[nodiscard]] Symbol* lookup(std::string_view name) const;
    [[nodiscard]] Scope* parent() const { return m_parent; }
    void collectNames(std::vector<std::string>& out) const;

private:
    Scope* m_parent;
    std::unordered_map<std::string, Symbol*> m_symbols;
};

/// Result of evaluating a constant expression.
struct ConstValue {
    bool isFloat = false;
    Int128 i = 0;
    double f = 0;
};

class Sema {
public:
    Sema(DiagnosticEngine& diagnostics, TypeContext& types);

    /// Returns true when no errors were reported.
    bool check(Unit& unit);

    [[nodiscard]] const std::vector<FuncInfo>& functions() const { return m_functions; }
    [[nodiscard]] const std::vector<GlobalInfo>& globals() const { return m_globals; }
    [[nodiscard]] TypeContext& types() { return m_types; }

    /// The file the user asked to compile, as opposed to the ones `import`
    /// pulled in.
    ///
    /// Needed for one decision: when a function in one file collides with a
    /// function of the same signature in another, the entry file may intentionally
    /// override an imported one (spec Q43) -- that is how a program replaces a
    /// library helper with its own. Two *imported* files colliding is a different
    /// situation with no such intent behind it, and it must be reported: the
    /// winner would otherwise depend on import order, which the reader cannot see
    /// at the call site.
    ///
    /// Unset means "no entry file known"; collisions are then all treated as
    /// imports colliding with imports, which errs toward reporting.
    void setRootFile(FileID file) { m_rootFile = file; }

    /// Set when the file being checked declares `library "name";`. Bodyless
    /// functions are then external (implemented in a shared library), so a
    /// missing body is the normal case rather than an error, and the file needs
    /// no entry point.
    void setExternalLibrary(bool external, std::string name = {}) {
        m_externalLibrary = external;
        m_currentLibraryName = std::move(name);
    }

    [[nodiscard]] const Type* nullType() const { return m_null; }

private:
    struct Flow {
        std::set<const Symbol*> assigned;
        bool reachable = true;
    };

    // -- declaration collection -------------------------------------------
    void collectTypeNames(Unit& unit);
    void declareDecls(Unit& unit, Scope& global);
    void declareStruct(Stmt& decl, Scope& scope);
    void declareEnum(Stmt& decl, Scope& scope);
    void declareGlobalVar(Stmt& decl, Scope& scope);
    /// Builds the FuncInfo for a declaration and records it in `m_functions`.
    ///
    /// `declareIntoScope` is false when instantiating a generic function: the
    /// instance must not become visible under the template's name (it is only
    /// reachable through the call that asked for it), but its parameter and
    /// return types still have to be resolved against the real global scope so
    /// that struct names in the signature keep working.
    int declareFunctionSignature(Stmt& decl, Scope& scope, bool topLevel,
                                 bool declareIntoScope = true);

    // -- generics (monomorphisation) ---------------------------------------
    void declareTemplate(Stmt& decl, Scope& scope);
    /// Registers `struct Name<T> { ... }` so uses can instantiate it.
    void declareStructTemplate(Stmt& decl);
    /// Instantiates a generic struct for concrete type arguments, or returns the
    /// existing instance. Returns the error type on failure.
    const Type* instantiateStruct(int templateIndex, const std::vector<const Type*>& typeArgs,
                                  SourceRange range);

    // -- methods -----------------------------------------------------------
    /// Declares every method of a struct under a mangled name and records it in
    /// `m_methods` so a call can find it by the user-visible name.
    void declareMethods(Stmt& structDecl, Scope& scope);
    /// Resolves `object.name(args)` -- `objectIsType` selects the `Point.origin()`
    /// form, which has no receiver.
    const Type* checkMethodCall(Expr& expr, Expr& callee, Scope& scope, Flow& flow,
                                bool objectIsType, const Type* objectType);
    /// Finds a binding for `name` among the type arguments of the instantiation
    /// currently being checked, or nullptr.
    [[nodiscard]] const Type* lookupTypeBinding(const std::string& name) const;
    /// Works out `T` from the declared parameter shapes and the argument types.
    /// Returns false and fills `why` when the call cannot be made to fit.
    [[nodiscard]] bool inferTypeArguments(FuncTemplate& tmpl,
                                          const std::vector<const Type*>& argTypes,
                                          std::vector<const Type*>& out, std::string& why);
    /// Instantiates `tmpl` for `typeArgs`, or returns an existing instance.
    /// Returns the function index, or -1 on error.
    int instantiateTemplate(int templateIndex, const std::vector<const Type*>& typeArgs,
                            SourceRange range);
    /// Checks a deferred method body if it has not been checked yet. Cheap when it
    /// has: one flag read.
    void ensureDeferredBody(int funcIndex);

    // -- body checking -----------------------------------------------------
    void checkFunctionBody(Stmt& decl, Scope& global);
    void checkStmt(Stmt& stmt, Scope& scope, Flow& flow);
    void checkStmtList(std::vector<StmtPtr>& stmts, Scope& scope, Flow& flow);
    void checkScopedList(std::vector<StmtPtr>& stmts, Scope& parent, Flow& flow);
    void checkVarDecl(Stmt& decl, Scope& scope, Flow& flow);
    void checkExprStmt(Stmt& stmt, Scope& scope, Flow& flow);
    void checkIf(Stmt& stmt, Scope& scope, Flow& flow);
    /// Checks one branch body in a fresh scope (bare blocks do not open one).
    void checkBranch(Stmt& body, Scope& scope, Flow& flow);
    void checkWhile(Stmt& stmt, Scope& scope, Flow& flow);
    void checkFor(Stmt& stmt, Scope& scope, Flow& flow);
    void checkForIn(Stmt& stmt, Scope& scope, Flow& flow);
    void checkSwitch(Stmt& stmt, Scope& scope, Flow& flow);
    void checkVirtual(Stmt& stmt, Scope& scope, Flow& flow);
    void checkDestructure(Stmt& stmt, Scope& scope, Flow& flow);

    // -- expressions -------------------------------------------------------
    const Type* checkExpr(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkName(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkUnary(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkBinary(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkAssign(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkTernary(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkCast(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkCall(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkIndex(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkSlice(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkMember(Expr& expr, Scope& scope, Flow& flow);
    const Type* checkBuiltinCall(Expr& expr, Symbol& symbol, Scope& scope, Flow& flow);
    const Type* checkInitializer(Expr& expr, const Type* target, Scope& scope, Flow& flow);

    /// Types a literal expression as `target` when possible (spec §2.4.1.1).
    bool adaptLiteral(Expr& expr, const Type* target);
    [[nodiscard]] bool isLiteralExpr(const Expr& expr) const;

    // -- helpers -----------------------------------------------------------
    const Type* resolveTypeNode(TypeNode& node, Scope& scope);
    const Type* resolveTypeNodeQuiet(TypeNode& node);
    Scope* typeScope() { return m_typeScope; }

    [[nodiscard]] bool evalConst(Expr& expr, Scope& scope, ConstValue& out);
    [[nodiscard]] std::string suggestName(std::string_view name, Scope& scope) const;

    /// Explains how to fix a rejected conversion, e.g. char -> int. Returns an
    /// empty string when there is nothing specific to say.
    [[nodiscard]] std::string conversionHint(const Type* from, const Type* to) const;
    [[nodiscard]] bool alwaysReturns(const Stmt& stmt) const;
    [[nodiscard]] bool containsReturn(const Stmt& stmt) const;
    void checkValueReadable(Symbol& symbol, Flow& flow, SourceRange range);
    static void collectNames(Expr& expr, std::vector<std::string>& out);

    /// `knownTypes[i]` (optional) supplies the type of `args[i]` without checking
    /// the expression again; index 0 is the callee slot and is ignored.
    [[nodiscard]] int resolveOverload(Symbol& symbol, std::vector<ExprPtr>& args, Scope& scope,
                                      Flow& flow, SourceRange range,
                                      const std::vector<const Type*>* knownTypes = nullptr);

    void diagError(SourceRange range, std::string message);
    void diagError(SourceLocation loc, std::string message);
    void ensureLayout(const Type* type, SourceRange range);
    void reportDeferredLiterals(Unit& unit);

    /// Verifies that the program has a usable `function main():void` and
    /// explains precisely what is wrong with an existing one.
    void checkEntryPoint(Unit& unit);

    DiagnosticEngine& m_diagnostics;
    TypeContext& m_types;
    /// Bodies that already produced a declaration-level error, so checking them
    /// again would only add symptoms (spec §7.2 asks for no cascades).
    std::vector<const Stmt*> m_skipBodyChecks;
    /// See `setRootFile`.
    FileID m_rootFile = kInvalidFileID;
    bool m_externalLibrary = false;
    std::string m_currentLibraryName;
    std::deque<Symbol> m_symbols;
    std::deque<Scope> m_scopes;

    std::vector<FuncInfo> m_functions;
    std::vector<GlobalInfo> m_globals;
    Scope* m_typeScope = nullptr;

    /// Generic function templates, indexed by Symbol::templates.
    std::vector<FuncTemplate> m_templates;
    /// Type arguments of the instantiations being checked, innermost last.
    /// A stack rather than a map because instantiating one generic can call
    /// another, and the inner one must not see the outer one's `T`.
    std::vector<std::pair<std::string, const Type*>> m_typeBindings;
    /// Mangled name -> function index, so each distinct set of type arguments is
    /// compiled exactly once.
    std::map<std::string, int> m_instances;
    /// Owns the cloned declarations of instantiated generics. FuncInfo::decl
    /// points into here, so it has to outlive the check.
    std::vector<StmtPtr> m_instantiatedDecls;
    /// Guards against a generic that instantiates itself with ever-new types.
    int m_instantiationDepth = 0;

    /// Generic struct templates, by name.
    std::vector<StructTemplate> m_structTemplates;
    std::map<std::string, int> m_structTemplateIndex;
    /// Mangled name -> the instance's type. One layout per distinct argument list.
    std::map<std::string, const Type*> m_structInstances;
    int m_structInstantiationDepth = 0;

    // per function state
    const Type* m_returnType = nullptr;
    FuncKind m_funcKind = FuncKind::Normal;
    int m_loopDepth = 0;
    int m_switchDepth = 0;
    int m_branchDepth = 0;
    int m_virtualDepth = 0;
    bool m_inEof = false;
    /// Set while checking a construct that does not read the value: the
    /// operand of `#`, or the target of an assignment. The definite-assignment
    /// check must not fire there (spec §5.3.1 talks about reads).
    bool m_lvalueContext = false;
    bool m_outerEofEmpty = true;
    std::vector<std::string> m_paramNames;
    std::set<std::string> m_labels;

    /// The struct whose body is being checked, so `self` resolves to it while a
    /// method signature is read. Null everywhere else, which keeps `self` an
    /// ordinary (undefined) identifier in free functions.
    const Type* m_receiverType = nullptr;
    /// Methods per owner type: `m_methods[S]["length"]` is the Symbol holding
    /// every `length` overload of `S`. Keyed by Type pointer because types are
    /// interned, so pointer identity is type identity.
    std::map<const Type*, std::map<std::string, Symbol*>> m_methods;

    const Type* m_null = nullptr;
    bool m_quiet = false;
    std::set<const Type*> m_layoutStack;
};

/// Root variable of an lvalue chain (`a.b[i]` -> `a`), or nullptr.
Symbol* lvalueRoot(Expr& expr);

} // namespace acvm

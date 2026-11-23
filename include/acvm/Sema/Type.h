//===--- Type.h - Type descriptions, layouts and the type context -------===//
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace acvm {

class Type;

enum class TypeKind : std::uint8_t {
    Error,
    Void,
    Bool,
    Char,
    Int,
    Float,
    Ptr,
    Array,
    Tuple,
    Struct,
    /// All members start at offset 0 and the size is the largest member, rounded
    /// up to the widest alignment -- the C layout, so a union can cross a native
    /// boundary unchanged. Reading a member that was not the last one written is
    /// a reinterpretation of those bytes rather than an error: that is the whole
    /// point of the type.
    Union,
    Enum,
    String,
    Vector,
    /// The type of the `null` literal: convertible to any `ptr T`, but not
    /// dereferenceable and not arithmetic (spec §2.4.2).
    Null,
};

struct FieldInfo {
    std::string name;
    const Type* type = nullptr;
    std::uint32_t offset = 0;
};

/// A (mostly) immutable type. Instances are owned and interned by TypeContext,
/// so pointer identity is usually enough for equality.
class Type {
public:
    TypeKind kind = TypeKind::Error;

    // Int
    bool isUnsigned = false;
    unsigned bits = 0;

    // Float (binary16 / binary32 / binary64, or a doubleA.B spelling)
    unsigned mantissa = 0;
    unsigned exponent = 0;

    // Ptr / Array / Vector
    const Type* elem = nullptr;
    std::uint64_t length = 0;

    // Tuple
    std::vector<const Type*> elems;

    // Struct / Enum / named
    std::string name;
    std::vector<FieldInfo> fields;                  // struct members
    std::vector<std::pair<std::string, std::int64_t>> members;  // enum items

    // Layout
    std::uint32_t sizeBytes = 0;
    std::uint32_t alignBytes = 1;
    bool layoutDone = false;

    [[nodiscard]] bool isInt() const { return kind == TypeKind::Int; }
    [[nodiscard]] bool isFloat() const { return kind == TypeKind::Float; }
    [[nodiscard]] bool isEnum() const { return kind == TypeKind::Enum; }
    [[nodiscard]] bool isNumeric() const { return isInt() || isFloat() || isEnum(); }
    [[nodiscard]] bool isIntegerLike() const { return isInt() || isEnum() || kind == TypeKind::Char || kind == TypeKind::Bool; }
    [[nodiscard]] bool isPtr() const { return kind == TypeKind::Ptr; }
    /// Types that live in memory and are moved by copying bytes.
    [[nodiscard]] bool isAggregate() const {
        switch (kind) {
        case TypeKind::Array:
        case TypeKind::Tuple:
        case TypeKind::Struct:
        case TypeKind::Union:
        case TypeKind::String:
        case TypeKind::Vector:
            return true;
        default:
            return false;
        }
    }
    [[nodiscard]] bool isVoid() const { return kind == TypeKind::Void; }
    [[nodiscard]] bool isError() const { return kind == TypeKind::Error; }
    [[nodiscard]] bool isBool() const { return kind == TypeKind::Bool; }

    /// Signed integer range for `bits` wide two's complement values.
    [[nodiscard]] std::int64_t minSigned() const;
    [[nodiscard]] std::uint64_t maxUnsigned() const;

    [[nodiscard]] std::string describe() const;

private:
    friend class TypeContext;
};

class TypeContext {
public:
    TypeContext();

    [[nodiscard]] const Type* errorType() const { return m_error; }
    [[nodiscard]] const Type* voidType() const { return m_void; }
    [[nodiscard]] const Type* boolType() const { return m_bool; }
    [[nodiscard]] const Type* charType() const { return m_char; }
    [[nodiscard]] const Type* stringType() const { return m_string; }
    [[nodiscard]] const Type* nullType() const { return m_null; }
    [[nodiscard]] const Type* intType() { return getInt(32, false); }      // `int`
    [[nodiscard]] const Type* floatType() { return getFloat(32, 0, 0); }   // `float`
    [[nodiscard]] const Type* doubleType() { return getFloat(64, 0, 0); }  // `double`

    [[nodiscard]] const Type* getInt(unsigned bits, bool isUnsigned);
    [[nodiscard]] const Type* getFloat(unsigned bits, unsigned mantissa, unsigned exponent);
    [[nodiscard]] const Type* getPtr(const Type* elem);
    [[nodiscard]] const Type* getArray(const Type* elem, std::uint64_t length);
    [[nodiscard]] const Type* getTuple(std::vector<const Type*> elems);
    [[nodiscard]] const Type* getVector(const Type* elem);

    /// Creates an empty struct type so that `ptr Node` can refer to it before
    /// its members are known, then finishes the layout.
    [[nodiscard]] const Type* createStruct(const std::string& name);
    void setFields(const Type* type, std::vector<FieldInfo> fields);

    /// Like `createStruct` / `createUnion`, but returns the existing type when
    /// `name` is already interned, and picks the kind. Instance names are mangled
    /// (`Pair<int32>`), so a second instantiation of the same arguments finds the
    /// first one's type instead of making a duplicate that compares equal by name
    /// but not by pointer. `kind` matters because a generic *union* has to keep
    /// its C layout rather than becoming a struct whose members are laid out in
    /// sequence.
    [[nodiscard]] const Type* getOrCreateAggregate(const std::string& name, TypeKind kind);

    /// Like `createStruct`, but members share one storage slot (C union layout).
    [[nodiscard]] const Type* createUnion(const std::string& name);

    [[nodiscard]] const Type* createEnum(const std::string& name);

    [[nodiscard]] bool same(const Type* lhs, const Type* rhs) const;
    [[nodiscard]] bool isAssignable(const Type* from, const Type* to) const;

    /// Result type of a mixed arithmetic operation (spec §4.2.2). Returns
    /// nullptr when the combination is illegal (e.g. mixed signedness).
    [[nodiscard]] const Type* mergeArithmetic(const Type* lhs, const Type* rhs);

    /// True when integer arithmetic on this type traps instead of wrapping.
    [[nodiscard]] bool trapsOnOverflow(const Type* type) const {
        return type->isInt() && !type->isUnsigned;
    }

private:
    [[nodiscard]] Type* make(TypeKind kind);

    std::deque<Type> m_types;
    std::map<std::pair<unsigned, bool>, Type*> m_ints;
    std::map<std::pair<unsigned, std::pair<unsigned, unsigned>>, Type*> m_floats;
    std::map<const Type*, Type*> m_ptrs;
    std::map<std::pair<const Type*, std::uint64_t>, Type*> m_arrays;
    std::map<const Type*, Type*> m_vectors;
    std::map<std::vector<const Type*>, Type*> m_tuples;
    std::map<std::string, Type*> m_named;

    const Type* m_error = nullptr;
    const Type* m_void = nullptr;
    const Type* m_bool = nullptr;
    const Type* m_char = nullptr;
    const Type* m_string = nullptr;
    const Type* m_int32 = nullptr;
    const Type* m_null = nullptr;
};

} // namespace acvm

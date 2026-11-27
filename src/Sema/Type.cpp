//===--- Type.cpp --------------------------------------------------------===//
#include "acvm/Sema/Type.h"

#include "acvm/Common/Value.h"

#include <algorithm>
#include <utility>

namespace acvm {
namespace {

std::uint32_t alignUp(std::uint32_t value, std::uint32_t align) {
    if (align <= 1) {
        return value;
    }
    return (value + align - 1) / align * align;
}

/// Lays out a sequence of members using C rules.
///
/// `struct` (spec §2.3): declaration order, each member aligned to its own
/// alignment, the whole padded to the largest alignment.
///
/// `union`: every member starts at offset 0 and the size is the largest member
/// padded to the widest alignment. That is exactly the C layout, which is what
/// lets a union cross a native boundary without any marshalling.
///
/// One function rather than two because the padding rules ("round the size up to
/// the alignment") are shared, and a union laid out by a separate copy of them
/// would be the kind of duplication that drifts silently.
void layoutMembers(TypeKind kind, std::vector<FieldInfo>& members, std::uint32_t& size,
                   std::uint32_t& align) {
    std::uint32_t maxAlign = 1;

    if (kind == TypeKind::Union) {
        std::uint32_t maxSize = 0;
        for (FieldInfo& member : members) {
            const std::uint32_t memberAlign = std::max<std::uint32_t>(member.type->alignBytes, 1);
            member.offset = 0;
            maxSize = std::max(maxSize, member.type->sizeBytes);
            maxAlign = std::max(maxAlign, memberAlign);
        }
        size = alignUp(maxSize, maxAlign);
        align = maxAlign;
        return;
    }

    std::uint32_t offset = 0;
    /// Bits already used in the byte at `offset`. Non-zero only while a run of bit
    /// fields is open; any ordinary member closes the byte first.
    std::uint32_t bitCursor = 0;

    for (FieldInfo& member : members) {
        const std::uint32_t memberAlign = std::max<std::uint32_t>(member.type->alignBytes, 1);
        const bool packs = member.type->isBit() && member.type->bits <= 8;
        if (packs) {
            // Never straddle a byte: if the rest of this one is too small, start the
            // next. Wasting up to 7 bits per run is the price of every access
            // touching exactly one byte -- which is what makes a bit write a plain
            // read-modify-write with no cross-byte carry to get wrong.
            if (bitCursor + member.type->bits > 8) {
                offset += 1;
                bitCursor = 0;
            }
            member.offset = offset;
            member.bitOffset = static_cast<std::uint8_t>(bitCursor);
            member.bitWidth = member.type->bits;
            bitCursor += member.type->bits;
            maxAlign = std::max(maxAlign, memberAlign);
            continue;
        }
        if (bitCursor > 0) {
            offset += 1;   // close the partially filled byte
            bitCursor = 0;
        }
        offset = alignUp(offset, memberAlign);
        member.offset = offset;
        member.bitOffset = 0;
        member.bitWidth = 0;
        offset += member.type->sizeBytes;
        maxAlign = std::max(maxAlign, memberAlign);
    }

    if (bitCursor > 0) {
        offset += 1;
    }
    size = alignUp(offset, maxAlign);
    align = maxAlign;
}

} // namespace

std::int64_t Type::minSigned() const {
    if (bits == 0 || bits >= 127) {
        return 0;
    }
    return -static_cast<std::int64_t>(static_cast<std::uint64_t>(1) << (bits - 1));
}

std::uint64_t Type::maxUnsigned() const {
    if (bits == 0 || bits >= 65) {
        return ~static_cast<std::uint64_t>(0);
    }
    if (bits == 64) {
        return ~static_cast<std::uint64_t>(0);
    }
    return (static_cast<std::uint64_t>(1) << bits) - 1;
}

std::string Type::describe() const {
    switch (kind) {
    case TypeKind::Error:
        return "<error>";
    case TypeKind::Void:
        return "void";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::Char:
        return "char";
    case TypeKind::Int:
        return (isUnsigned ? "unsigned int" : "int") + std::to_string(bits);
    case TypeKind::Bit:
        return "bit" + std::to_string(bits);
    case TypeKind::Float: {
        if (mantissa != 0 || exponent != 0) {
            return "double" + std::to_string(mantissa) + "." + std::to_string(exponent);
        }
        return "float" + std::to_string(bits);
    }
    case TypeKind::Ptr:
        return "ptr " + (elem != nullptr ? elem->describe() : std::string("<null>"));
    case TypeKind::Array:
        return (elem != nullptr ? elem->describe() : std::string("<null>")) + "[" +
               std::to_string(length) + "]";
    case TypeKind::Tuple: {
        std::string result = "(";
        for (std::size_t i = 0; i < elems.size(); ++i) {
            if (i != 0) {
                result += ", ";
            }
            result += elems[i] != nullptr ? elems[i]->describe() : "<null>";
        }
        result += ")";
        return result;
    }
    case TypeKind::Struct:
        return name.empty() ? "struct" : name;
    case TypeKind::Union:
        return name.empty() ? "union" : name;
    case TypeKind::Enum:
        return name.empty() ? "enum" : name;
    case TypeKind::String:
        return "string";
    case TypeKind::Vector:
        return "vector " + (elem != nullptr ? elem->describe() : std::string("<null>"));
    case TypeKind::Null:
        return "null";
    case TypeKind::Func: {
        // `Func<ret, args...>`, the shape it is written in. A null return type is a
        // partially built instance, which only `describe` asks about on an error path.
        std::string result = "Func<";
        result += funcReturn() != nullptr ? funcReturn()->describe() : "<null>";
        for (std::size_t i = 0; i < funcParamCount(); ++i) {
            result += ", ";
            result += funcParam(i) != nullptr ? funcParam(i)->describe() : "<null>";
        }
        return result + ">";
    }
    }
    return "<unknown>";
}

TypeContext::TypeContext() {
    Type* error = make(TypeKind::Error);
    error->name = "<error>";
    m_error = error;

    Type* voidType = make(TypeKind::Void);
    voidType->name = "void";
    voidType->sizeBytes = 0;
    voidType->alignBytes = 1;
    m_void = voidType;

    Type* boolType = make(TypeKind::Bool);
    boolType->name = "bool";
    boolType->sizeBytes = 1;
    boolType->alignBytes = 1;
    m_bool = boolType;

    Type* charType = make(TypeKind::Char);
    charType->name = "char";
    charType->bits = 16;
    charType->sizeBytes = 2;
    charType->alignBytes = 2;
    m_char = charType;

    Type* stringType = make(TypeKind::String);
    stringType->name = "string";
    stringType->sizeBytes = 16;   // ptr + length, both pointer width
    stringType->alignBytes = 8;
    m_string = stringType;

    Type* nullType = make(TypeKind::Null);
    nullType->name = "null";
    nullType->sizeBytes = 8;
    nullType->alignBytes = 8;
    m_null = nullType;

    m_int32 = getInt(32, false);
}

Type* TypeContext::make(TypeKind kind) {
    m_types.emplace_back();
    Type* type = &m_types.back();
    type->kind = kind;
    return type;
}

const Type* TypeContext::getInt(unsigned bits, bool isUnsigned) {
    // 128 is the widest: `Int128` holds exactly that, and `signedMin`/`signedMax`
    // for 128 are its endpoints.
    if (bits < 2 || bits > 128) {
        return m_error;
    }
    const auto key = std::make_pair(bits, isUnsigned);
    if (const auto it = m_ints.find(key); it != m_ints.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Int);
    type->bits = bits;
    type->isUnsigned = isUnsigned;
    type->name = type->describe();
    type->sizeBytes = (bits + 7) / 8;
    type->alignBytes = type->sizeBytes;
    m_ints.emplace(key, type);
    return type;
}

const Type* TypeContext::getBit(unsigned bits) {
    if (bits < 1) {
        return m_error;
    }
    if (const auto it = m_bits.find(bits); it != m_bits.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Bit);
    type->bits = bits;
    type->isUnsigned = true;   // a bit string has no sign; reads zero-extend
    type->name = type->describe();
    type->sizeBytes = (bits + 7) / 8;
    // Alignment 1 so a bit field can follow a `bit1` in the same byte. The packing
    // itself is decided by the layout pass, which is the only place that knows
    // whether the previous member left room.
    type->alignBytes = 1;
    m_bits.emplace(bits, type);
    return type;
}

const Type* TypeContext::getFloat(unsigned bits, unsigned mantissa, unsigned exponent) {
    if (mantissa != 0 || exponent != 0) {
        // doubleA.B: accepted syntactically, stored as binary64.
        const auto key = std::make_pair(64u, std::make_pair(mantissa, exponent));
        if (const auto it = m_floats.find(key); it != m_floats.end()) {
            return it->second;
        }
        Type* type = make(TypeKind::Float);
        type->bits = 64;
        type->mantissa = mantissa;
        type->exponent = exponent;
        type->name = type->describe();
        type->sizeBytes = 8;
        type->alignBytes = 8;
        m_floats.emplace(key, type);
        return type;
    }

    if (bits != 16 && bits != 32 && bits != 64) {
        return m_error;
    }
    const auto key = std::make_pair(bits, std::make_pair(0u, 0u));
    if (const auto it = m_floats.find(key); it != m_floats.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Float);
    type->bits = bits;
    type->name = "float" + std::to_string(bits);
    type->sizeBytes = bits / 8;
    type->alignBytes = type->sizeBytes;
    m_floats.emplace(key, type);
    return type;
}

const Type* TypeContext::getPtr(const Type* elem) {
    if (elem == nullptr) {
        elem = m_error;
    }
    if (const auto it = m_ptrs.find(elem); it != m_ptrs.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Ptr);
    type->elem = elem;
    type->sizeBytes = 8;
    type->alignBytes = 8;
    type->name = type->describe();
    m_ptrs.emplace(elem, type);
    return type;
}

const Type* TypeContext::getArray(const Type* elem, std::uint64_t length) {
    if (elem == nullptr) {
        elem = m_error;
    }
    const auto key = std::make_pair(elem, length);
    if (const auto it = m_arrays.find(key); it != m_arrays.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Array);
    type->elem = elem;
    type->length = length;
    const unsigned perByte = elem->bitElementsPerByte();
    if (perByte > 0) {
        // Packed: `bit1 flags[1000]` is 125 bytes, not 1000.
        type->sizeBytes = static_cast<std::uint32_t>((length + perByte - 1) / perByte);
        type->alignBytes = 1;
    } else {
        type->sizeBytes = static_cast<std::uint32_t>(elem->sizeBytes * length);
        type->alignBytes = std::max<std::uint32_t>(elem->alignBytes, 1);
    }
    type->name = type->describe();
    m_arrays.emplace(key, type);
    return type;
}

const Type* TypeContext::getTuple(std::vector<const Type*> elems) {
    if (const auto it = m_tuples.find(elems); it != m_tuples.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Tuple);
    type->elems = std::move(elems);
    for (const Type* element : type->elems) {
        type->fields.push_back(FieldInfo{"", element, 0});
    }
    layoutMembers(TypeKind::Tuple, type->fields, type->sizeBytes, type->alignBytes);
    type->layoutDone = true;
    type->name = type->describe();
    const auto stored = type->elems;
    m_tuples.emplace(stored, type);
    return type;
}

const Type* TypeContext::getVector(const Type* elem) {
    if (elem == nullptr) {
        elem = m_error;
    }
    if (const auto it = m_vectors.find(elem); it != m_vectors.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Vector);
    type->elem = elem;
    type->sizeBytes = 24;   // data pointer + length + capacity
    type->alignBytes = 8;
    type->name = type->describe();
    m_vectors.emplace(elem, type);
    return type;
}

const Type* TypeContext::getFunc(std::vector<const Type*> signature) {
    if (const auto it = m_funcs.find(signature); it != m_funcs.end()) {
        return it->second;
    }
    Type* type = make(TypeKind::Func);
    type->elems = std::move(signature);
    // The handle, not the signature: a function value is one word at run time, so every
    // layout question answers the same as `int64`. Keeping the signature in `elems` is
    // what lets the two be told apart at compile time while the bytes stay identical.
    type->sizeBytes = 8;
    type->alignBytes = 8;
    type->layoutDone = true;
    type->name = type->describe();
    const auto stored = type->elems;
    m_funcs.emplace(stored, type);
    return type;
}

const Type* TypeContext::createStruct(const std::string& name) {
    Type* type = make(TypeKind::Struct);
    type->name = name;
    type->sizeBytes = 0;
    type->alignBytes = 1;
    m_named.emplace(name, type);
    return type;
}

void TypeContext::setFields(const Type* type, std::vector<FieldInfo> fields) {
    if (type == nullptr || (type->kind != TypeKind::Struct && type->kind != TypeKind::Union)) {
        return;
    }
    auto* mutableType = const_cast<Type*>(type);
    mutableType->fields = std::move(fields);
    layoutMembers(type->kind, mutableType->fields, mutableType->sizeBytes,
                  mutableType->alignBytes);
}

const Type* TypeContext::getOrCreateAggregate(const std::string& name, TypeKind kind) {
    const auto existing = m_named.find(name);
    if (existing != m_named.end()) {
        return existing->second;
    }
    return kind == TypeKind::Union ? createUnion(name) : createStruct(name);
}

const Type* TypeContext::createUnion(const std::string& name) {
    Type* type = make(TypeKind::Union);
    type->name = name;
    type->sizeBytes = 0;
    type->alignBytes = 1;
    m_named.emplace(name, type);
    return type;
}

const Type* TypeContext::createEnum(const std::string& name) {
    Type* type = make(TypeKind::Enum);
    type->name = name;
    type->bits = 32;          // spec Q107: underlying type is always int
    type->sizeBytes = 4;
    type->alignBytes = 4;
    m_named.emplace(name, type);
    return type;
}

bool TypeContext::same(const Type* lhs, const Type* rhs) const {
    if (lhs == rhs) {
        return true;
    }
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    if (lhs->kind != rhs->kind) {
        return false;
    }
    switch (lhs->kind) {
    case TypeKind::Int:
        return lhs->bits == rhs->bits && lhs->isUnsigned == rhs->isUnsigned;
    case TypeKind::Bit:
        return lhs->bits == rhs->bits;
    case TypeKind::Float:
        return lhs->bits == rhs->bits && lhs->mantissa == rhs->mantissa &&
               lhs->exponent == rhs->exponent;
    case TypeKind::Ptr:
        return same(lhs->elem, rhs->elem);
    case TypeKind::Array:
        return lhs->length == rhs->length && same(lhs->elem, rhs->elem);
    // A tuple compares element by element, and a function type is stored the same way:
    // `elems[0]` is the return type and `elems[1..]` the parameters. One branch for both.
    //
    // Reaching this without `lhs == rhs` is not possible today -- `getFunc` interns the
    // signature vector, so two `Func<int, int>` are one object -- which is why the missing
    // case never produced a wrong answer. It did produce a `-Wswitch` warning, and it would
    // become a real bug the moment types are rebuilt rather than interned: a module read
    // back from bytecode, or a signature mentioning an error type, would compare unequal
    // to itself and the assignment would be rejected.
    case TypeKind::Tuple:
    case TypeKind::Func: {
        if (lhs->elems.size() != rhs->elems.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs->elems.size(); ++i) {
            if (!same(lhs->elems[i], rhs->elems[i])) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Struct:
    case TypeKind::Union:
    case TypeKind::Enum:
        return lhs->name == rhs->name;
    case TypeKind::Vector:
        return same(lhs->elem, rhs->elem);
    case TypeKind::Void:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::String:
    case TypeKind::Null:
    case TypeKind::Error:
        return true;
    }
    return false;
}

bool TypeContext::isAssignable(const Type* from, const Type* to) const {
    if (from == nullptr || to == nullptr) {
        return false;
    }
    if (from->isError() || to->isError()) {
        return true;
    }
    if (same(from, to)) {
        return true;
    }
    // Spec §4.2: no implicit numeric conversion; only weak enums convert.
    if (from->isEnum() && to->isInt()) {
        return to->bits == 32 && !to->isUnsigned;
    }
    if (from->isInt() && to->isEnum()) {
        return from->bits == 32 && !from->isUnsigned;
    }
    if (from->isEnum() && to->isEnum()) {
        return true;
    }
    // `null` is polymorphic over pointers (spec §2.4.2).
    if (from->kind == TypeKind::Null && to->isPtr()) {
        return true;
    }
    return false;
}

const Type* TypeContext::mergeArithmetic(const Type* lhs, const Type* rhs) {
    if (lhs == nullptr || rhs == nullptr || lhs->isError() || rhs->isError()) {
        return m_error;
    }

    // Weak enums behave as int.
    const Type* left = lhs->isEnum() ? m_int32 : lhs;
    const Type* right = rhs->isEnum() ? m_int32 : rhs;

    if (left->isFloat() || right->isFloat()) {
        if (!left->isNumeric() || !right->isNumeric()) {
            return nullptr;
        }
        if (left->isFloat() && right->isFloat()) {
            return left->bits >= right->bits ? left : right;
        }
        return left->isFloat() ? left : right;
    }

    // `char` and `bool` are not numbers (spec §4.2.2 makes `bool` + `intN` an
    // error and `char` follows the same rule). Two text values of the same kind
    // keep their type, which is what `min` / `max` need; mixing them with
    // numbers is rejected so that they cannot silently decay into `int`.
    if (left->kind == TypeKind::Char || right->kind == TypeKind::Char) {
        return (left->kind == TypeKind::Char && right->kind == TypeKind::Char) ? m_char : nullptr;
    }
    if (left->kind == TypeKind::Bool || right->kind == TypeKind::Bool) {
        return (left->kind == TypeKind::Bool && right->kind == TypeKind::Bool) ? m_bool : nullptr;
    }

    if (!left->isInt() || !right->isInt()) {
        return nullptr;
    }

    if (left->isUnsigned != right->isUnsigned) {
        return nullptr;
    }
    return getInt(std::max(left->bits, right->bits), left->isUnsigned);
}

} // namespace acvm

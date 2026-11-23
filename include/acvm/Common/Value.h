//===--- Value.h - Runtime value of the ACVM ----------------------------===//
#pragma once

#include <cstdint>
#include <string>

namespace acvm {

/// 128 bit signed integer, wide enough for every Axiom int type up to int127.
#if defined(__SIZEOF_INT128__)
__extension__ using Int128 = __int128;
#else
#error "ACVM requires a compiler with 128 bit integer support"
#endif

/// A runtime value.
///
/// Scalars are held inline: integers as canonical 128 bit values, floats as
/// double, pointers as 64 bit addresses. Aggregates (struct / tuple / array /
/// string / vector) live in VM memory; for those the VM carries a `Tag::Addr`
/// value pointing at the storage.
class Value {
public:
    enum class Tag : std::uint8_t { Int, Float, Ptr, Addr };

    Value() = default;

    static Value fromInt(Int128 v) {
        Value value;
        value.m_tag = Tag::Int;
        value.m_int = v;
        return value;
    }
    static Value fromFloat(double v) {
        Value value;
        value.m_tag = Tag::Float;
        value.m_float = v;
        return value;
    }
    static Value fromPtr(std::uint64_t v) {
        Value value;
        value.m_tag = Tag::Ptr;
        value.m_int = static_cast<Int128>(v);
        return value;
    }
    static Value fromAddr(std::uint64_t v) {
        Value value;
        value.m_tag = Tag::Addr;
        value.m_int = static_cast<Int128>(v);
        return value;
    }

    [[nodiscard]] Tag tag() const { return m_tag; }
    [[nodiscard]] bool isInt() const { return m_tag == Tag::Int; }
    [[nodiscard]] bool isFloat() const { return m_tag == Tag::Float; }
    [[nodiscard]] bool isAddr() const { return m_tag == Tag::Addr; }

    /// Integer view of the value. Integers and addresses share one field, so
    /// a pointer that flows into an integer operation (pointer difference, a
    /// pointer-to-int cast) keeps its value instead of silently becoming 0.
    [[nodiscard]] Int128 asInt() const {
        return m_tag == Tag::Float ? static_cast<Int128>(m_float) : m_int;
    }
    [[nodiscard]] double asFloat() const {
        return m_tag == Tag::Float ? m_float : static_cast<double>(m_int);
    }
    /// Address view. Integers convert to their numeric value for the same
    /// reason: the address arithmetic in the VM produces integers.
    [[nodiscard]] std::uint64_t asPtr() const {
        return m_tag == Tag::Float ? static_cast<std::uint64_t>(m_float)
                                   : static_cast<std::uint64_t>(m_int);
    }

    /// Numeric view used by arithmetic, comparisons and printing.
    [[nodiscard]] bool numericIsFloat() const { return m_tag == Tag::Float; }
    [[nodiscard]] double numeric() const { return m_tag == Tag::Float ? m_float : static_cast<double>(m_int); }

private:
    /// One storage slot, because a value is one of these and never two: `m_tag`
    /// says which member is live.
    ///
    /// They used to be separate members, which made a `Value` 48 bytes -- the
    /// 16 byte integer, then 8 for the float, all forced onto 16 byte alignment.
    /// Every instruction that pushes or pops a value copied all 48, on the hottest
    /// path there is. A union makes it 32, and the tag decides which half is real.
    ///
    /// Only the live member may be read. Every accessor below tests `m_tag` first,
    /// which is why this is safe: `fromFloat` writes `m_float` and leaves the tag
    /// `Float`, so nothing ever reads `m_int` out of a float.
    union {
        /// Holds integers and addresses (see the accessors above).
        Int128 m_int = 0;
        double m_float;
    };
    Tag m_tag = Tag::Int;
};

/// Formats a 128 bit signed integer in the given base (2..36, lower case).
std::string formatInt128(Int128 value, int base = 10);

/// Formats a float using the shortest representation that round-trips,
/// printing integral values without a trailing ".0".
std::string formatFloat(double value);

/// Parses the raw digit text of an integer literal (underscores removed,
/// 0x / 0b prefixes understood). Returns false when the value does not fit in
/// 128 bits.
bool parseInt128(const std::string& text, Int128& out);

} // namespace acvm

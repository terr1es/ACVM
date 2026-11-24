//===--- X64Assembler.h - a small x86-64 encoder for generated code ------===//
#pragma once

#include "acvm/JIT/ExecutableMemory.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace acvm::jit {

/// General purpose registers, numbered the way the encoding numbers them: the low
/// three bits go in ModRM, the high bit in REX. That is also the ABI's numbering, so
/// `Reg` values can be compared against the platform's register lists directly.
enum class Reg : std::uint8_t {
    Rax = 0, Rcx = 1, Rdx = 2, Rbx = 3, Rsp = 4, Rbp = 5, Rsi = 6, Rdi = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

/// Condition codes, in the encoding order of the `Jcc` / `SETcc` opcodes.
enum class Cond : std::uint8_t {
    Overflow = 0,        //  jo
    NoOverflow = 1,      //  jno
    Below = 2,           //  jb   (unsigned <)
    AboveEqual = 3,      //  jae  (unsigned >=)
    Equal = 4,           //  je
    NotEqual = 5,        //  jne
    BelowEqual = 6,      //  jbe
    Above = 7,           //  ja
    Sign = 8,            //  js
    NoSign = 9,          //  jns
    Less = 12,           //  jl   (signed <)
    GreaterEqual = 13,   //  jge
    LessEqual = 14,      //  jle
    Greater = 15,        //  jg
};

/// Operand width, which decides both the REX.W bit and which sub-register name the
/// instruction addresses. Axiom's integer types map onto these directly.
enum class Width : std::uint8_t { Byte = 1, Word = 2, Dword = 4, Qword = 8 };

/// `[base + disp]`. No index register and no scale: the VM's frame slots and page
/// offsets are always base+constant, so anything richer would be unused code that
/// still has to be correct.
struct Mem {
    Reg base;
    std::int32_t disp;
};

[[nodiscard]] inline Mem mem(Reg base, std::int32_t disp = 0) { return Mem{base, disp}; }

/// `[base + index*scale + disp]`, the form a table lookup needs. `scale` must be 1, 2, 4
/// or 8 -- those are the only multipliers the encoding can express.
struct MemIndex {
    Reg base;
    Reg index;
    int scale;
    std::int32_t disp;
};

[[nodiscard]] inline MemIndex memIndex(Reg base, Reg index, int scale,
                                       std::int32_t disp = 0) {
    return MemIndex{base, index, scale, disp};
}

/// Emits machine code into a growable buffer, then hands back an executable copy.
///
/// The interface is deliberately close to the metal rather than instruction-shaped:
/// the caller (the lowering pass) decides what to emit, and this only guarantees that
/// the bytes are what the manual says. No optimisation, no register allocation, no
/// implicit temporaries -- every byte emitted here was asked for.
class X64Assembler {
public:
    using Label = std::size_t;

    X64Assembler() = default;

    // -- data movement ----------------------------------------------------

    /// `mov dst, imm`. For a 64 bit destination this uses the `imm64` form
    /// unconditionally: a shorter `imm32` form exists, but choosing between them by
    /// value would make the code depend on constants the caller may resize later.
    void movImm(Reg dst, Width width, std::int64_t imm);

    void mov(Reg dst, Reg src, Width width = Width::Qword);
    void mov(Reg dst, const Mem& src, Width width = Width::Qword);
    void mov(const Mem& dst, Reg src, Width width = Width::Qword);
    void mov(Reg dst, const MemIndex& src, Width width = Width::Qword);
    void mov(const MemIndex& dst, Reg src, Width width = Width::Qword);
    void lea(Reg dst, const MemIndex& src);

    /// `movzx` / `movsx` from a narrow source into a 64 bit register.
    void movzx(Reg dst, const Mem& src, Width srcWidth);
    void movzx(Reg dst, Reg src, Width srcWidth);
    void movsx(Reg dst, const Mem& src, Width srcWidth);
    void movsx(Reg dst, Reg src, Width srcWidth);
    void movsxd(Reg dst, Reg src);
    void movsxd(Reg dst, const Mem& src);

    void lea(Reg dst, const Mem& src);

    // -- arithmetic -------------------------------------------------------

    void add(Reg dst, Reg src, Width width = Width::Qword);
    void sub(Reg dst, Reg src, Width width = Width::Qword);
    void and_(Reg dst, Reg src, Width width = Width::Qword);
    void or_(Reg dst, Reg src, Width width = Width::Qword);
    void xor_(Reg dst, Reg src, Width width = Width::Qword);
    void imul(Reg dst, Reg src, Width width = Width::Qword);
    void neg(Reg dst, Width width = Width::Qword);
    void not_(Reg dst, Width width = Width::Qword);

    void addImm(Reg dst, std::int32_t imm, Width width = Width::Qword);
    void subImm(Reg dst, std::int32_t imm, Width width = Width::Qword);
    void cmpImm(Reg dst, std::int32_t imm, Width width = Width::Qword);

    void cmp(Reg lhs, Reg rhs, Width width = Width::Qword);
    void cmp(Reg lhs, const Mem& rhs, Width width = Width::Qword);
    void test(Reg lhs, Reg rhs, Width width = Width::Qword);

    /// Shifts. The count is an immediate because every shift the compiler emits either
    /// comes from a constant or is a 32 bit limb shift, both known at emission time.
    void shl(Reg dst, std::uint8_t count, Width width = Width::Qword);
    /// Shift by cl, for a count that is only known at run time.
    void shlCl(Reg dst, Width width = Width::Qword);
    void sarCl(Reg dst, Width width = Width::Qword);
    void shrCl(Reg dst, Width width = Width::Qword);
    void sar(Reg dst, std::uint8_t count, Width width = Width::Qword);
    void shr(Reg dst, std::uint8_t count, Width width = Width::Qword);

    /// Signed division: `cqo` first (or `cdq` for a dword), then `idiv`.
    void cqo();
    void cdq();
    void idiv(Reg divisor, Width width = Width::Qword);

    // -- condition and branching -----------------------------------------

    void setcc(Cond cond, Reg dst);
    void cmovcc(Cond cond, Reg dst, Reg src);

    void push(Reg src);
    void pop(Reg dst);

    void ret();
    void ud2();

    Label newLabel();
    void bind(Label label);
    void jmp(Label label);
    void jcc(Cond cond, Label label);

    /// Direct call to an absolute address. Done as `mov r11, imm64; call r11` because a
    /// rel32 call would only reach the same module, and the callee here is a C++ helper
    /// in another translation unit whose address is not known until link time.
    void callAbsolute(const void* target);
    void callReg(Reg target);
    /// `call [base + disp]` -- used for indirect calls through a slot.
    void callMem(const Mem& target);

    // -- bookkeeping ------------------------------------------------------

    [[nodiscard]] std::size_t codeSize() const { return m_code.size(); }

    /// Pads with `nop` until the offset is a multiple of `alignment`. Called before a
    /// label that a loop jumps back to: an unaligned branch target costs fetch cycles
    /// on every iteration.
    void alignTo(std::size_t alignment);

    /// Copies the buffer into W^X memory and makes it executable. After this the
    /// assembler must not be used again -- the returned block owns the code.
    std::unique_ptr<ExecutableBlock> finalize();

    /// The bytes emitted so far, for tests that assert on encodings directly.
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return m_code; }

private:
    void emitByte(std::uint8_t byte) { m_code.push_back(byte); }
    void emitU32(std::uint32_t value);
    void emitU64(std::uint64_t value);
    /// `force` exists for one reason: a byte register numbered 4..7 is only reachable
    /// with a REX prefix, and even an all-zero REX (`0x40`) is what selects
    /// `spl`/`bpl`/`sil`/`dil` over `ah`/`ch`/`dh`/`bh`. Without it the same ModRM
    /// silently addresses a different register.
    void emitRex(bool w, int regField, int indexField, int rmField, bool force = false);
    /// The 0x66 prefix, which is what makes an instruction operate on 16 bits. There is
    /// no REX.W equivalent for it: 8, 32 and 64 bits are the encodable default widths.
    void emitOperandSize(Width width);
    /// ModRM (+SIB when the base register requires it) for a register or memory
    /// operand in the `rm` position.
    void emitModRm(std::size_t regField, Reg rm, bool isMemory, std::int32_t disp);
    /// `[base + index*scale + disp]`. Always uses mod=10 with a SIB byte, so the base's
    /// low bits never change the meaning of the mod field.
    void emitModRmSib(std::size_t regField, const MemIndex& operand);

    struct Patch {
        std::size_t offset;   ///< where the rel32 begins
        Label label;
    };

    std::vector<std::uint8_t> m_code;
    std::vector<Patch> m_patches;
    std::vector<std::int32_t> m_labelOffsets;
    std::vector<bool> m_labelBound;
};

} // namespace acvm::jit

//===--- X64Assembler.cpp - x86-64 encoding ---------------------------------===//
#include "acvm/JIT/X64Assembler.h"

#include <cstring>

namespace acvm::jit {

namespace {

constexpr std::uint8_t kRexBase = 0x40;
constexpr std::uint8_t kModRegIndirect = 0xC0;      // mod = 11
constexpr std::uint8_t kModDisp32 = 0x80;           // mod = 10
constexpr std::uint8_t kModNoDisp = 0x00;           // mod = 00
constexpr std::uint8_t kNoIndexInSib = 0x04;        // index field 100 = none
constexpr std::uint8_t kRspField = 4;               // rsp/r12 need a SIB byte

int regNumber(Reg reg) { return static_cast<int>(reg); }

/// True when `rm` in a ModRM byte means "memory through SIB" rather than a register.
bool needsSib(Reg base) { return (regNumber(base) & 7) == kRspField; }

/// `[rbp]` and `[r13]` with mod=00 mean "disp32, no base", so a zero displacement on
/// those registers still has to be written out.
bool needsDisplacement(Reg base) {
    const int low = regNumber(base) & 7;
    return low == 5;
}

/// Byte registers 4..7 (`spl`, `bpl`, `sil`, `dil`) only exist when a REX prefix is
/// present; without one the encoding means `ah`, `ch`, `dh`, `bh`.
bool byteRegisterNeedsRex(int number) { return (number & 7) >= 4; }

} // namespace

void X64Assembler::emitU32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        emitByte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
}

void X64Assembler::emitU64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        emitByte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
}

void X64Assembler::emitOperandSize(Width width) {
    if (width == Width::Word) {
        emitByte(0x66);
    }
}

void X64Assembler::emitRex(bool w, int regField, int indexField, int rmField,
                           bool force) {
    const int b = (rmField >> 3) & 1;
    const int x = (indexField >> 3) & 1;
    const int r = (regField >> 3) & 1;
    if (!w && !force && b == 0 && x == 0 && r == 0) {
        // No REX needed. Emitting one anyway is legal but wastes a byte on the hot
        // path, and some other encoders (indirect addressing) would then see a
        // different prefix situation than they expect.
        return;
    }
    emitByte(static_cast<std::uint8_t>(kRexBase | (w ? 0x08 : 0) | (r ? 0x04 : 0) |
                                       (x ? 0x02 : 0) | (b ? 0x01 : 0)));
}

void X64Assembler::emitModRm(std::size_t regField, Reg rm, bool isMemory,
                             std::int32_t disp) {
    const int rmField = regNumber(rm);
    if (!isMemory) {
        emitByte(static_cast<std::uint8_t>(kModRegIndirect | ((regField & 7) << 3) |
                                           (rmField & 7)));
        return;
    }

    const bool sib = needsSib(rm);
    std::uint8_t mod = kModDisp32;
    if (disp == 0 && !needsDisplacement(rm)) {
        mod = kModNoDisp;
    }
    // A SIB byte is required whenever the base is rsp/r12: the rm field then means
    // "SIB follows", not "rsp".
    const std::uint8_t rmBits = sib ? 0x04 : static_cast<std::uint8_t>(rmField & 7);
    emitByte(static_cast<std::uint8_t>(mod | ((regField & 7) << 3) | rmBits));

    if (sib) {
        emitByte(static_cast<std::uint8_t>((0 << 6) | (kNoIndexInSib << 3) |
                                           (rmField & 7)));
    }
    if (mod == kModDisp32) {
        emitU32(static_cast<std::uint32_t>(disp));
    }
}

void X64Assembler::emitModRmSib(std::size_t regField, const MemIndex& operand) {
    int scaleBits = 0;
    switch (operand.scale) {
    case 1: scaleBits = 0; break;
    case 2: scaleBits = 1; break;
    case 4: scaleBits = 2; break;
    default: scaleBits = 3; break;   // 8
    }
    emitByte(static_cast<std::uint8_t>(kModDisp32 | ((regField & 7) << 3) | 0x04));
    emitByte(static_cast<std::uint8_t>((scaleBits << 6) |
                                       ((regNumber(operand.index) & 7) << 3) |
                                       (regNumber(operand.base) & 7)));
    emitU32(static_cast<std::uint32_t>(operand.disp));
}

//===--- data movement ---------------------------------------------------===//

void X64Assembler::movImm(Reg dst, Width width, std::int64_t imm) {
    const int rd = regNumber(dst);
    if (width == Width::Qword) {
        emitRex(true, 0, 0, rd);
        emitByte(static_cast<std::uint8_t>(0xB8 + (rd & 7)));
        emitU64(static_cast<std::uint64_t>(imm));
        return;
    }
    emitRex(false, 0, 0, rd);
    emitByte(static_cast<std::uint8_t>(0xB8 + (rd & 7)));
    if (width == Width::Dword) {
        emitU32(static_cast<std::uint32_t>(imm));
    } else if (width == Width::Word) {
        emitByte(static_cast<std::uint8_t>(imm & 0xFF));
        emitByte(static_cast<std::uint8_t>((imm >> 8) & 0xFF));
    } else {
        emitByte(static_cast<std::uint8_t>(imm & 0xFF));
    }
}

void X64Assembler::mov(Reg dst, Reg src, Width width) {
    const bool w = width == Width::Qword;
    const bool byte = width == Width::Byte;
    // 0x89 /r is `MOV r/m, r`, so the source goes in the reg field.
    emitOperandSize(width);
    emitRex(w, regNumber(src), 0, regNumber(dst),
            byte && (byteRegisterNeedsRex(regNumber(src)) ||
                    byteRegisterNeedsRex(regNumber(dst))));
    emitByte(width == Width::Byte ? 0x88 : 0x89);
    emitModRm(static_cast<std::size_t>(regNumber(src)), dst, false, 0);
}

void X64Assembler::mov(Reg dst, const Mem& src, Width width) {
    const bool w = width == Width::Qword;
    emitOperandSize(width);
    emitRex(w, regNumber(dst), 0, regNumber(src.base),
            width == Width::Byte && byteRegisterNeedsRex(regNumber(dst)));
    emitByte(width == Width::Byte ? 0x8A : 0x8B);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src.base, true, src.disp);
}

void X64Assembler::mov(const Mem& dst, Reg src, Width width) {
    const bool w = width == Width::Qword;
    emitOperandSize(width);
    emitRex(w, regNumber(src), 0, regNumber(dst.base),
            width == Width::Byte && byteRegisterNeedsRex(regNumber(src)));
    emitByte(width == Width::Byte ? 0x88 : 0x89);
    emitModRm(static_cast<std::size_t>(regNumber(src)), dst.base, true, dst.disp);
}

void X64Assembler::mov(Reg dst, const MemIndex& src, Width width) {
    emitOperandSize(width);
    emitRex(width == Width::Qword, regNumber(dst), regNumber(src.index),
            regNumber(src.base));
    emitByte(width == Width::Byte ? 0x8A : 0x8B);
    emitModRmSib(static_cast<std::size_t>(regNumber(dst)), src);
}

void X64Assembler::mov(const MemIndex& dst, Reg src, Width width) {
    emitOperandSize(width);
    emitRex(width == Width::Qword, regNumber(src), regNumber(dst.index),
            regNumber(dst.base));
    emitByte(width == Width::Byte ? 0x88 : 0x89);
    emitModRmSib(static_cast<std::size_t>(regNumber(src)), dst);
}

void X64Assembler::lea(Reg dst, const MemIndex& src) {
    emitRex(true, regNumber(dst), regNumber(src.index), regNumber(src.base));
    emitByte(0x8D);
    emitModRmSib(static_cast<std::size_t>(regNumber(dst)), src);
}

void X64Assembler::movzx(Reg dst, const Mem& src, Width srcWidth) {
    // There is no `movzx r64, r/m32`: a 32 bit load into a 32 bit register already clears
    // the upper half, so that case is a plain `mov`. Encoding it as the 16 bit form -- the
    // else branch here before -- read only the low two bytes of a four byte value.
    if (srcWidth == Width::Dword) {
        emitRex(false, regNumber(dst), 0, regNumber(src.base));
        emitByte(0x8B);
        emitModRm(static_cast<std::size_t>(regNumber(dst)), src.base, true, src.disp);
        return;
    }
    emitOperandSize(srcWidth);
    emitRex(true, regNumber(dst), 0, regNumber(src.base));
    emitByte(0x0F);
    emitByte(srcWidth == Width::Byte ? 0xB6 : 0xB7);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src.base, true, src.disp);
}

void X64Assembler::movzx(Reg dst, Reg src, Width srcWidth) {
    if (srcWidth == Width::Dword) {
        // `mov r32, r32` clears the upper half, which is exactly zero-extension.
        emitRex(false, regNumber(src), 0, regNumber(dst));
        emitByte(0x89);
        emitModRm(static_cast<std::size_t>(regNumber(src)), dst, false, 0);
        return;
    }
    emitOperandSize(srcWidth);
    emitRex(true, regNumber(dst), 0, regNumber(src));
    emitByte(0x0F);
    emitByte(srcWidth == Width::Byte ? 0xB6 : 0xB7);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src, false, 0);
}

void X64Assembler::movsx(Reg dst, const Mem& src, Width srcWidth) {
    // A 32 bit sign-extending load is `movsxd` (0x63), a separate opcode from the 8 and 16
    // bit `0F BE`/`0F BF` pair.
    if (srcWidth == Width::Dword) {
        emitRex(true, regNumber(dst), 0, regNumber(src.base));
        emitByte(0x63);
        emitModRm(static_cast<std::size_t>(regNumber(dst)), src.base, true, src.disp);
        return;
    }
    emitOperandSize(srcWidth);
    emitRex(true, regNumber(dst), 0, regNumber(src.base));
    emitByte(0x0F);
    emitByte(srcWidth == Width::Byte ? 0xBE : 0xBF);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src.base, true, src.disp);
}

void X64Assembler::movsx(Reg dst, Reg src, Width srcWidth) {
    if (srcWidth == Width::Dword) {
        movsxd(dst, src);
        return;
    }
    emitOperandSize(srcWidth);
    emitRex(true, regNumber(dst), 0, regNumber(src));
    emitByte(0x0F);
    emitByte(srcWidth == Width::Byte ? 0xBE : 0xBF);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src, false, 0);
}

void X64Assembler::movsxd(Reg dst, Reg src) {
    emitRex(true, regNumber(dst), 0, regNumber(src));
    emitByte(0x63);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src, false, 0);
}

void X64Assembler::movsxd(Reg dst, const Mem& src) {
    emitRex(true, regNumber(dst), 0, regNumber(src.base));
    emitByte(0x63);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src.base, true, src.disp);
}

void X64Assembler::lea(Reg dst, const Mem& src) {
    emitRex(true, regNumber(dst), 0, regNumber(src.base));
    emitByte(0x8D);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src.base, true, src.disp);
}

//===--- arithmetic ------------------------------------------------------===//

void X64Assembler::add(Reg dst, Reg src, Width width) {
    emitRex(width == Width::Qword, regNumber(src), 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x00 : 0x01);
    emitModRm(static_cast<std::size_t>(regNumber(src)), dst, false, 0);
}

void X64Assembler::sub(Reg dst, Reg src, Width width) {
    emitRex(width == Width::Qword, regNumber(src), 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x28 : 0x29);
    emitModRm(static_cast<std::size_t>(regNumber(src)), dst, false, 0);
}

void X64Assembler::and_(Reg dst, Reg src, Width width) {
    emitRex(width == Width::Qword, regNumber(src), 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x20 : 0x21);
    emitModRm(static_cast<std::size_t>(regNumber(src)), dst, false, 0);
}

void X64Assembler::or_(Reg dst, Reg src, Width width) {
    emitRex(width == Width::Qword, regNumber(src), 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x08 : 0x09);
    emitModRm(static_cast<std::size_t>(regNumber(src)), dst, false, 0);
}

void X64Assembler::xor_(Reg dst, Reg src, Width width) {
    emitRex(width == Width::Qword, regNumber(src), 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x30 : 0x31);
    emitModRm(static_cast<std::size_t>(regNumber(src)), dst, false, 0);
}

void X64Assembler::imul(Reg dst, Reg src, Width width) {
    emitRex(width == Width::Qword, regNumber(dst), 0, regNumber(src));
    emitByte(0x0F);
    emitByte(0xAF);
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src, false, 0);
}

void X64Assembler::neg(Reg dst, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xF7);
    emitModRm(3, dst, false, 0);
}

void X64Assembler::not_(Reg dst, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xF7);
    emitModRm(2, dst, false, 0);
}

void X64Assembler::addImm(Reg dst, std::int32_t imm, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x80 : 0x81);
    emitModRm(0, dst, false, 0);
    emitU32(static_cast<std::uint32_t>(imm));
}

void X64Assembler::subImm(Reg dst, std::int32_t imm, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x80 : 0x81);
    emitModRm(5, dst, false, 0);
    emitU32(static_cast<std::uint32_t>(imm));
}

void X64Assembler::cmpImm(Reg dst, std::int32_t imm, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(width == Width::Byte ? 0x80 : 0x81);
    emitModRm(7, dst, false, 0);
    emitU32(static_cast<std::uint32_t>(imm));
}

void X64Assembler::cmp(Reg lhs, Reg rhs, Width width) {
    // 0x39 /r is `CMP r/m, r`: lhs is the rm operand, rhs the reg field.
    emitRex(width == Width::Qword, regNumber(rhs), 0, regNumber(lhs));
    emitByte(width == Width::Byte ? 0x38 : 0x39);
    emitModRm(static_cast<std::size_t>(regNumber(rhs)), lhs, false, 0);
}

void X64Assembler::cmp(Reg lhs, const Mem& rhs, Width width) {
    emitRex(width == Width::Qword, regNumber(lhs), 0, regNumber(rhs.base));
    emitByte(width == Width::Byte ? 0x3A : 0x3B);
    emitModRm(static_cast<std::size_t>(regNumber(lhs)), rhs.base, true, rhs.disp);
}

void X64Assembler::test(Reg lhs, Reg rhs, Width width) {
    emitRex(width == Width::Qword, regNumber(rhs), 0, regNumber(lhs));
    emitByte(width == Width::Byte ? 0x84 : 0x85);
    emitModRm(static_cast<std::size_t>(regNumber(rhs)), lhs, false, 0);
}

void X64Assembler::shl(Reg dst, std::uint8_t count, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xC1);
    emitModRm(4, dst, false, 0);
    emitByte(count);
}

void X64Assembler::sar(Reg dst, std::uint8_t count, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xC1);
    emitModRm(7, dst, false, 0);
    emitByte(count);
}

void X64Assembler::shr(Reg dst, std::uint8_t count, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xC1);
    emitModRm(5, dst, false, 0);
    emitByte(count);
}

void X64Assembler::shlCl(Reg dst, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xD3);
    emitModRm(4, dst, false, 0);
}

void X64Assembler::sarCl(Reg dst, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xD3);
    emitModRm(7, dst, false, 0);
}

void X64Assembler::shrCl(Reg dst, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(dst));
    emitByte(0xD3);
    emitModRm(5, dst, false, 0);
}

void X64Assembler::cqo() {
    emitByte(0x48);
    emitByte(0x99);
}

void X64Assembler::cdq() {
    emitByte(0x99);
}

void X64Assembler::idiv(Reg divisor, Width width) {
    emitRex(width == Width::Qword, 0, 0, regNumber(divisor));
    emitByte(0xF7);
    emitModRm(7, divisor, false, 0);
}

//===--- conditions and branching ----------------------------------------===//

void X64Assembler::setcc(Cond cond, Reg dst) {
    // A byte register numbered 4..7 needs REX to name `spl`..`dil` rather than
    // `ah`..`bh`, so the prefix is mandatory here even when it carries no bits. This
    // is the case the test caught: without it `sete sil` became `sete dh`.
    emitRex(false, 0, 0, regNumber(dst), byteRegisterNeedsRex(regNumber(dst)));
    emitByte(0x0F);
    emitByte(static_cast<std::uint8_t>(0x90 + static_cast<int>(cond)));
    emitModRm(0, dst, false, 0);
}

void X64Assembler::cmovcc(Cond cond, Reg dst, Reg src) {
    emitRex(true, regNumber(dst), 0, regNumber(src));
    emitByte(0x0F);
    emitByte(static_cast<std::uint8_t>(0x40 + static_cast<int>(cond)));
    emitModRm(static_cast<std::size_t>(regNumber(dst)), src, false, 0);
}

void X64Assembler::push(Reg src) {
    const int rd = regNumber(src);
    if (rd >= 8) {
        emitRex(false, 0, 0, rd);
    }
    emitByte(static_cast<std::uint8_t>(0x50 + (rd & 7)));
}

void X64Assembler::pop(Reg dst) {
    const int rd = regNumber(dst);
    if (rd >= 8) {
        emitRex(false, 0, 0, rd);
    }
    emitByte(static_cast<std::uint8_t>(0x58 + (rd & 7)));
}

void X64Assembler::ret() { emitByte(0xC3); }

void X64Assembler::ud2() {
    emitByte(0x0F);
    emitByte(0x0B);
}

X64Assembler::Label X64Assembler::newLabel() {
    m_labelOffsets.push_back(-1);
    m_labelBound.push_back(false);
    return m_labelOffsets.size() - 1;
}

void X64Assembler::bind(Label label) {
    m_labelOffsets[label] = static_cast<std::int32_t>(m_code.size());
    m_labelBound[label] = true;
    // Resolution is deferred to finalize(). Patching here looks tempting -- a backward
    // jump could be resolved the moment its target exists -- but it is wrong: a jump
    // emitted *after* its label has been bound (the backward branch at the bottom of a
    // loop, which is the common case) would never be visited again. It was: the loop
    // body skipped its own back edge and the loop ran once.
}

void X64Assembler::jmp(Label label) {
    emitByte(0xE9);
    m_patches.push_back(Patch{m_code.size(), label});
    emitU32(0);
}

void X64Assembler::jcc(Cond cond, Label label) {
    emitByte(0x0F);
    emitByte(static_cast<std::uint8_t>(0x80 + static_cast<int>(cond)));
    m_patches.push_back(Patch{m_code.size(), label});
    emitU32(0);
}

void X64Assembler::callAbsolute(const void* target) {
    movImm(Reg::R11, Width::Qword, static_cast<std::int64_t>(
                                      reinterpret_cast<std::uintptr_t>(target)));
    emitRex(false, 0, 0, regNumber(Reg::R11));
    emitByte(0xFF);
    emitModRm(2, Reg::R11, false, 0);
}

void X64Assembler::callReg(Reg target) {
    emitRex(false, 0, 0, regNumber(target));
    emitByte(0xFF);
    emitModRm(2, target, false, 0);
}

void X64Assembler::callMem(const Mem& target) {
    emitRex(false, 0, 0, regNumber(target.base));
    emitByte(0xFF);
    emitModRm(2, target.base, true, target.disp);
}

//===--- bookkeeping -----------------------------------------------------===//

void X64Assembler::alignTo(std::size_t alignment) {
    if (alignment <= 1) {
        return;
    }
    while ((m_code.size() % alignment) != 0) {
        emitByte(0x90);   // nop
    }
}

std::unique_ptr<ExecutableBlock> X64Assembler::finalize() {
    // A jump that never got bound means the lowering pass forgot a label; failing here
    // is much better than branching into whatever follows the buffer.
    for (bool bound : m_labelBound) {
        if (!bound) {
            return nullptr;
        }
    }

    for (const Patch& patch : m_patches) {
        const auto target = static_cast<std::int64_t>(m_labelOffsets[patch.label]);
        const auto from = static_cast<std::int64_t>(patch.offset) + 4;
        const auto relative =
            static_cast<std::uint32_t>(static_cast<std::int32_t>(target - from));
        std::memcpy(m_code.data() + patch.offset, &relative, 4);
    }
    auto block = ExecutableBlock::create(m_code.size());
    if (!block) {
        return nullptr;
    }
    std::memcpy(block->writable(), m_code.data(), m_code.size());
    if (!block->makeExecutable()) {
        return nullptr;
    }
    return block;
}

} // namespace acvm::jit

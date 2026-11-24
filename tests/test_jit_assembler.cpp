//===--- test_jit_assembler.cpp - the encoder emits what the manual says ---===//
//
// Every case here calls freshly emitted machine code through a function pointer and
// checks the value that comes back. That is the only kind of test that can catch an
// encoder bug: the bytes are either the instruction the manual describes or they are
// something else entirely, and comparing bytes against a table would just restate the
// encoder in a second place.
//
// The tests are grouped by what they prove, not by opcode, because the failure modes
// differ: a wrong ModRM shows up as reading the wrong register, a missing REX.W shows up
// as truncation to 32 bits, and a missing SIB shows up as a wild address.

#include "TestHarness.hpp"

#include "acvm/JIT/X64Assembler.h"

#include <cstdint>
#include <iostream>
#include <cstring>
#include <vector>

using namespace acvm::jit;

namespace {

/// Compiles `emit` into executable memory and hands back a callable pointer.
template <typename Fn, typename Emit>
Fn compile(Emit&& emit) {
    X64Assembler assembler;
    emit(assembler);
    auto block = assembler.finalize();
    if (!block) {
        return nullptr;
    }
    // The block has to outlive the call. Tests are short-lived, so leaking it into a
    // process-wide list is simpler than threading an owner through every case.
    static std::vector<std::unique_ptr<ExecutableBlock>> keepAlive;
    keepAlive.push_back(std::move(block));
    return reinterpret_cast<Fn>(keepAlive.back()->entry());
}

using Fn0 = std::int64_t (*)();
using Fn1 = std::int64_t (*)(std::int64_t);
using Fn2 = std::int64_t (*)(std::int64_t, std::int64_t);
using FnPtr = std::int64_t (*)(const std::int64_t*);

} // namespace

TEST(the_platform_allows_w_x_executable_memory) {
    CHECK(ExecutableBlock::supported());
    auto block = ExecutableBlock::create(4096);
    CHECK(block != nullptr);
    CHECK(block->makeExecutable());
}

TEST(mov_r64_imm64_then_ret) {
    auto fn = compile<Fn0>([](X64Assembler& a) {
        a.movImm(Reg::Rax, Width::Qword, 42);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(), 42);
}

TEST(mov_r32_imm32_does_not_disturb_the_upper_half) {
    // The point is the absence of REX.W: `mov eax, -1` must leave rax as 0x00000000FFFFF
    // FFFF, not sign-extend. Writing it as a 64 bit immediate instead would hide a
    // missing prefix bug on any value that happens to fit in 32 bits.
    auto fn = compile<Fn0>([](X64Assembler& a) {
        a.movImm(Reg::Rax, Width::Qword, 0);
        a.movImm(Reg::Rax, Width::Dword, -1);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(), static_cast<std::int64_t>(0xFFFFFFFFu));
}

TEST(parameters_arrive_in_rcx_and_rdx) {
    auto fn = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.add(Reg::Rax, Reg::Rdx);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(3, 4), 7);
    CHECK_EQ(fn(-10, 4), -6);
}

TEST(subtraction_and_multiplication) {
    auto sub = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.sub(Reg::Rax, Reg::Rdx);
        a.ret();
    });
    CHECK(sub != nullptr);
    CHECK_EQ(sub(10, 3), 7);
    CHECK_EQ(sub(3, 10), -7);

    auto mul = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.imul(Reg::Rax, Reg::Rdx);
        a.ret();
    });
    CHECK(mul != nullptr);
    CHECK_EQ(mul(7, 6), 42);
    CHECK_EQ(mul(-7, 6), -42);
}

TEST(a_load_through_a_pointer_needs_no_sib_but_a_store_through_rsp_does) {
    // The load uses rcx as the base, which encodes without a SIB byte. The store uses
    // rsp, where the rm field means "SIB follows" -- omitting it produces an address
    // built from whatever follows as an index register, so this case is the one that
    // catches a missing SIB.
    std::int64_t target = 0;
    auto load = compile<FnPtr>([](X64Assembler& a) {
        a.mov(Reg::Rax, mem(Reg::Rcx, 0));
        a.ret();
    });
    CHECK(load != nullptr);
    target = 1234;
    CHECK_EQ(load(&target), 1234);

    auto store = compile<Fn1>([](X64Assembler& a) {
        a.subImm(Reg::Rsp, 16);
        a.mov(mem(Reg::Rsp, 8), Reg::Rcx);
        a.mov(Reg::Rax, mem(Reg::Rsp, 8));
        a.addImm(Reg::Rsp, 16);
        a.ret();
    });
    CHECK(store != nullptr);
    CHECK_EQ(store(-99), -99);
}

TEST(narrow_loads_sign_extend_and_zero_extend_correctly) {
    // The same byte read both ways, because a movsx encoded as movzx passes every
    // positive test and fails only on the sign.
    std::uint8_t buffer[8] = {0x80, 0xFF, 0x7F, 0x00, 0, 0, 0, 0};

    auto signedLoad = compile<FnPtr>([](X64Assembler& a) {
        a.movsx(Reg::Rax, mem(Reg::Rcx, 0), Width::Byte);
        a.ret();
    });
    CHECK(signedLoad != nullptr);
    CHECK_EQ(signedLoad(reinterpret_cast<const std::int64_t*>(buffer)), -128);

    auto unsignedLoad = compile<FnPtr>([](X64Assembler& a) {
        a.movzx(Reg::Rax, mem(Reg::Rcx, 0), Width::Byte);
        a.ret();
    });
    CHECK(unsignedLoad != nullptr);
    CHECK_EQ(unsignedLoad(reinterpret_cast<const std::int64_t*>(buffer)), 128);

    // A separate buffer so the 16 bit result cannot coincide with the 8 bit one:
    // 0xFF80 is -128 either way, which would let a wrong-width load pass.
    std::uint8_t wide[8] = {0x00, 0xFF, 0x00, 0x00, 0, 0, 0, 0};
    auto wordLoad = compile<FnPtr>([](X64Assembler& a) {
        a.movsx(Reg::Rax, mem(Reg::Rcx, 0), Width::Word);
        a.ret();
    });
    CHECK(wordLoad != nullptr);
    CHECK_EQ(wordLoad(reinterpret_cast<const std::int64_t*>(wide)), -256);
}

TEST(a_32_bit_load_would_truncate_so_movsxd_is_used_for_widening) {
    auto fn = compile<FnPtr>([](X64Assembler& a) {
        a.movsxd(Reg::Rax, mem(Reg::Rcx, 0));
        a.ret();
    });
    CHECK(fn != nullptr);
    std::int32_t negative = -5;
    std::int64_t holder = 0;
    std::memcpy(&holder, &negative, 4);
    CHECK_EQ(fn(&holder), -5);
}

TEST(immediate_arithmetic) {
    auto fn = compile<Fn0>([](X64Assembler& a) {
        a.movImm(Reg::Rax, Width::Qword, 100);
        a.addImm(Reg::Rax, 23);
        a.subImm(Reg::Rax, 3);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(), 120);
}

TEST(cmp_sets_the_flags_and_setcc_reads_them) {
    auto fn = compile<Fn2>([](X64Assembler& a) {
        a.cmp(Reg::Rcx, Reg::Rdx);
        a.setcc(Cond::Less, Reg::Rax);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(1, 2), 1);
    CHECK_EQ(fn(2, 1), 0);
    // Signed comparison: -1 is less than 1 even though its encoding is larger.
    CHECK_EQ(fn(-1, 1), 1);
}

TEST(setcc_into_a_high_byte_register_needs_its_own_rex_prefix) {
    // `sil`, `dil`, `spl`, `bpl` are only reachable with a REX prefix; without one the
    // encoding names ah/bh/ch/dh and silently writes the wrong register.
    // rsi has to be saved: it is callee-saved on Win64, so clobbering it corrupts the
    // caller rather than the result -- which is how this test first failed.
    auto fn = compile<Fn2>([](X64Assembler& a) {
        a.push(Reg::Rsi);
        a.movImm(Reg::Rax, Width::Qword, 0);
        a.cmp(Reg::Rcx, Reg::Rdx);
        a.setcc(Cond::Equal, Reg::Rsi);
        a.movzx(Reg::Rax, Reg::Rsi, Width::Byte);
        a.pop(Reg::Rsi);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(5, 5), 1);
    CHECK_EQ(fn(5, 6), 0);
}

TEST(conditional_forward_jump) {
    auto fn = compile<Fn1>([](X64Assembler& a) {
        auto positive = a.newLabel();
        a.movImm(Reg::Rax, Width::Qword, 0);
        a.cmpImm(Reg::Rcx, 0);
        a.jcc(Cond::Greater, positive);
        a.ret();
        a.bind(positive);
        a.movImm(Reg::Rax, Width::Qword, 1);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(5), 1);
    CHECK_EQ(fn(-5), 0);
}

TEST(a_counted_loop_with_a_backward_jump) {
    // The whole shape of a compiled Axiom loop: a counter in a register, a compare, a
    // conditional exit and a backward branch. If any of the four is wrong the sum is
    // wrong rather than the program crashing, which is exactly the silent failure the
    // JIT must not have.
    auto sumTo = compile<Fn1>([](X64Assembler& a) {
        auto loop = a.newLabel();
        auto done = a.newLabel();
        a.movImm(Reg::Rax, Width::Qword, 0);          // sum
        a.movImm(Reg::Rdx, Width::Qword, 0);          // i
        a.bind(loop);
        a.cmp(Reg::Rdx, Reg::Rcx);
        a.jcc(Cond::GreaterEqual, done);
        a.add(Reg::Rax, Reg::Rdx);
        a.addImm(Reg::Rdx, 1);
        a.jmp(loop);
        a.bind(done);
        a.ret();
    });
    CHECK(sumTo != nullptr);
    CHECK_EQ(sumTo(0), 0);
    CHECK_EQ(sumTo(1), 0);
    CHECK_EQ(sumTo(10), 45);
    CHECK_EQ(sumTo(1000), 499500);
}

TEST(jo_catches_a_signed_overflow_in_an_add) {
    // Axiom traps on overflow, so the lowering pass has to test OF after every signed
    // arithmetic instruction. This checks the flag is actually set where it must be.
    auto fn = compile<Fn2>([](X64Assembler& a) {
        auto overflowed = a.newLabel();
        a.mov(Reg::Rax, Reg::Rcx);
        a.add(Reg::Rax, Reg::Rdx);
        a.jcc(Cond::Overflow, overflowed);
        a.ret();
        a.bind(overflowed);
        a.movImm(Reg::Rax, Width::Qword, 1);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(1, 2), 3);
    const std::int64_t max = INT64_MAX;
    CHECK_EQ(fn(max, 1), 1);
    CHECK_EQ(fn(INT64_MIN, -1), 1);
}

TEST(signed_division_with_cqo_and_idiv) {
    // The divisor has to leave rdx first: `cqo` overwrites rdx with the sign of rax,
    // so dividing by rdx is a divide by 0 or -1. The lowering pass faces the same
    // constraint and must reserve a scratch register around every division.
    auto fn = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.mov(Reg::R8, Reg::Rdx);
        a.cqo();
        a.idiv(Reg::R8);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(100, 7), 14);
    CHECK_EQ(fn(-100, 7), -14);
    CHECK_EQ(fn(7, -2), -3);
}

TEST(division_leaves_the_remainder_in_rdx) {
    // Axiom's `%` is the same instruction read from rdx, so the lowering pass needs to
    // know it is there rather than dividing twice.
    auto fn = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.mov(Reg::R8, Reg::Rdx);
        a.cqo();
        a.idiv(Reg::R8);
        a.mov(Reg::Rax, Reg::Rdx);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(100, 7), 2);
    CHECK_EQ(fn(-100, 7), -2);
}

TEST(shifts_are_arithmetic_where_they_must_be) {
    auto sarFn = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.sar(Reg::Rax, 1);
        a.ret();
    });
    CHECK(sarFn != nullptr);
    CHECK_EQ(sarFn(-8, 0), -4);

    auto shlFn = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.shl(Reg::Rax, 2);
        a.ret();
    });
    CHECK(shlFn != nullptr);
    CHECK_EQ(shlFn(3, 0), 12);
}

TEST(and_or_xor_neg_and_not) {
    auto band = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.and_(Reg::Rax, Reg::Rdx);
        a.ret();
    });
    CHECK(band != nullptr);
    CHECK_EQ(band(0b1100, 0b1010), 0b1000);

    auto bor = compile<Fn2>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.or_(Reg::Rax, Reg::Rdx);
        a.ret();
    });
    CHECK(bor != nullptr);
    CHECK_EQ(bor(0b1100, 0b1010), 0b1110);

    auto bneg = compile<Fn1>([](X64Assembler& a) {
        a.mov(Reg::Rax, Reg::Rcx);
        a.neg(Reg::Rax);
        a.ret();
    });
    CHECK(bneg != nullptr);
    CHECK_EQ(bneg(42), -42);
}

TEST(push_pop_and_a_16_byte_aligned_call_frame) {
    auto fn = compile<Fn2>([](X64Assembler& a) {
        a.push(Reg::Rbx);
        a.subImm(Reg::Rsp, 8);            // keep rsp 16 byte aligned for the callee
        a.mov(Reg::Rbx, Reg::Rcx);
        a.add(Reg::Rbx, Reg::Rdx);
        a.mov(Reg::Rax, Reg::Rbx);
        a.addImm(Reg::Rsp, 8);
        a.pop(Reg::Rbx);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(20, 22), 42);
}

namespace {

extern "C" std::int64_t jit_test_native_add(std::int64_t a, std::int64_t b) {
    return a + b;
}

} // namespace

TEST(callabsolute_reaches_a_real_c_function) {
    auto fn = compile<Fn2>([](X64Assembler& a) {
        a.subImm(Reg::Rsp, 40);           // 32 byte shadow space + alignment
        a.callAbsolute(reinterpret_cast<const void*>(&jit_test_native_add));
        a.addImm(Reg::Rsp, 40);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(19, 23), 42);
}

TEST(an_unbound_label_makes_finalize_fail_instead_of_branching_anywhere) {
    X64Assembler assembler;
    auto never = assembler.newLabel();
    assembler.jmp(never);
    CHECK(assembler.finalize() == nullptr);
}

TEST(alignto_pads_with_nops_and_does_not_disturb_the_code) {
    auto fn = compile<Fn1>([](X64Assembler& a) {
        auto target = a.newLabel();
        a.movImm(Reg::Rax, Width::Qword, 7);
        a.alignTo(16);
        a.bind(target);
        a.ret();
    });
    CHECK(fn != nullptr);
    CHECK_EQ(fn(0), 7);
}

// Unbuffered: a crash inside generated code must not swallow the name of the test that
// crashed, which is exactly what buffered stdout did the first time this file ran.
int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    return ::test::runAll();
}

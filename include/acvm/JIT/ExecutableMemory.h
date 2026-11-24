//===--- ExecutableMemory.h - RW -> RX pages for generated code -----------===//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace acvm::jit {

/// A block of memory that can be written, then executed, but never both at once.
///
/// The three platforms disagree about how to express that, so each has its own path:
///
///   Windows   `VirtualAlloc(PAGE_READWRITE)` then `VirtualProtect(PAGE_EXECUTE_READ)`,
///             plus `FlushInstructionCache` because the CPU may hold stale lines from
///             the write.
///   macOS     `mmap(MAP_JIT)` and then `pthread_jit_write_protect_np`: the hardened
///             runtime refuses W+X outright, and MAP_JIT is the documented way to get
///             a region that can alternate.
///   Linux/BSD `mmap(PROT_READ|PROT_WRITE)` then `mprotect(PROT_READ|PROT_EXEC)` and
///             `__builtin___clear_cache` on architectures with split caches.
///
/// W^X is not decoration. A page that is writable and executable at the same time turns
/// any memory-corruption bug in the interpreter into arbitrary code execution, and on
/// macOS it simply will not map.
class ExecutableBlock {
public:
    /// Allocates `size` bytes, writable. Returns null on failure.
    static std::unique_ptr<ExecutableBlock> create(std::size_t size);

    ~ExecutableBlock();
    ExecutableBlock(const ExecutableBlock&) = delete;
    ExecutableBlock& operator=(const ExecutableBlock&) = delete;

    /// Writable view. Valid until `makeExecutable()` is called.
    [[nodiscard]] std::uint8_t* writable() { return m_code; }

    /// Switches the whole block to read+execute and flushes the instruction cache.
    /// Returns false if the platform refused to change the protection.
    bool makeExecutable();

    /// Callable view. Only meaningful after `makeExecutable()` succeeded.
    [[nodiscard]] void* entry() const { return m_code; }
    [[nodiscard]] std::size_t size() const { return m_size; }

    /// True when the platform let us switch protection. A false here means generated
    /// code cannot be run at all, and the caller must fall back to the interpreter.
    [[nodiscard]] static bool supported();

private:
    ExecutableBlock() = default;

    std::uint8_t* m_code = nullptr;
    std::size_t m_size = 0;
    bool m_executable = false;
};

} // namespace acvm::jit

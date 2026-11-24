//===--- ExecutableMemory.cpp - W^X pages, one path per platform ---------===//
#include "acvm/JIT/ExecutableMemory.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace acvm::jit {

namespace {

/// Page size, queried once. mmap wants a multiple of this; asking for an exact byte
/// count that happens to fall short of a page is a silent failure on some systems.
std::size_t pageSize() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
#else
    static const std::size_t size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return size;
#endif
}

std::size_t roundUp(std::size_t value, std::size_t multiple) {
    return ((value + multiple - 1) / multiple) * multiple;
}

} // namespace

bool ExecutableBlock::supported() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    // Nothing to probe on Linux: mmap with these flags is always available.
    return true;
#endif
}

std::unique_ptr<ExecutableBlock> ExecutableBlock::create(std::size_t size) {
    if (size == 0) {
        return nullptr;
    }
    const std::size_t rounded = roundUp(size, pageSize());

#if defined(_WIN32)
    void* raw = VirtualAlloc(nullptr, rounded, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (raw == nullptr) {
        return nullptr;
    }
    std::unique_ptr<ExecutableBlock> block(new ExecutableBlock());
    block->m_code = static_cast<std::uint8_t*>(raw);
    block->m_size = rounded;
    return block;

#elif defined(__APPLE__)
    // MAP_JIT is what the hardened runtime accepts; without it the mprotect below is
    // refused even for a process that would otherwise be allowed to JIT.
    void* raw = mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (raw == MAP_FAILED) {
        return nullptr;
    }
    std::unique_ptr<ExecutableBlock> block(new ExecutableBlock());
    block->m_code = static_cast<std::uint8_t*>(raw);
    block->m_size = rounded;
    return block;

#else
    void* raw = mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return nullptr;
    }
    std::unique_ptr<ExecutableBlock> block(new ExecutableBlock());
    block->m_code = static_cast<std::uint8_t*>(raw);
    block->m_size = rounded;
    return block;
#endif
}

bool ExecutableBlock::makeExecutable() {
    if (m_code == nullptr) {
        return false;
    }
#if defined(_WIN32)
    DWORD previous = 0;
    if (VirtualProtect(m_code, m_size, PAGE_EXECUTE_READ, &previous) == 0) {
        return false;
    }
    // The CPU is allowed to serve instructions from a stale line that it fetched
    // while the page was writable, so the write has to be published explicitly.
    FlushInstructionCache(GetCurrentProcess(), m_code, m_size);
    m_executable = true;
    return true;

#elif defined(__APPLE__)
    if (mprotect(m_code, m_size, PROT_READ | PROT_EXEC) != 0) {
        return false;
    }
    // On Apple silicon the write protect toggle is what actually opens and closes
    // the window; mprotect alone is not enough under the hardened runtime.
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache(reinterpret_cast<char*>(m_code),
                            reinterpret_cast<char*>(m_code + m_size));
    m_executable = true;
    return true;

#else
    if (mprotect(m_code, m_size, PROT_READ | PROT_EXEC) != 0) {
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(m_code),
                            reinterpret_cast<char*>(m_code + m_size));
    m_executable = true;
    return true;
#endif
}

ExecutableBlock::~ExecutableBlock() {
    if (m_code == nullptr) {
        return;
    }
#if defined(_WIN32)
    VirtualFree(m_code, 0, MEM_RELEASE);
#else
    munmap(m_code, m_size);
#endif
}

} // namespace acvm::jit

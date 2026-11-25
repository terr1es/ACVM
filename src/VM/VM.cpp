//===--- VM.cpp - Memory, scheduling and the interpreter loop ----------===//
#include "acvm/VM/VM.h"

#include "acvm/IR/Disassembler.h"
#include "acvm/Sema/Builtin.h"
#include "VMInternal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace acvm {
using namespace vminternal;
namespace {

constexpr std::uint64_t kPageShift = 16;
constexpr std::uint64_t kPageSize = std::uint64_t{1} << kPageShift;
constexpr std::uint64_t kPageMask = kPageSize - 1;

double halfToDouble(std::uint16_t bits) {
    const unsigned sign = (bits >> 15) & 1u;
    const unsigned exponent = (bits >> 10) & 0x1Fu;
    const unsigned mantissa = bits & 0x3FFu;

    double value = 0.0;
    if (exponent == 0) {
        value = std::ldexp(static_cast<double>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa == 0 ? std::numeric_limits<double>::infinity()
                             : std::numeric_limits<double>::quiet_NaN();
    } else {
        value = std::ldexp(static_cast<double>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -value : value;
}

std::uint16_t doubleToHalf(double value) {
    if (std::isnan(value)) {
        return 0x7E00;
    }
    const unsigned sign = std::signbit(value) ? 1u : 0u;
    if (std::isinf(value)) {
        return static_cast<std::uint16_t>((sign << 15) | 0x7C00);
    }
    double magnitude = std::fabs(value);
    if (magnitude == 0.0) {
        return static_cast<std::uint16_t>(sign << 15);
    }
    int exponent = 0;
    double fraction = std::frexp(magnitude, &exponent); // magnitude = fraction * 2^exponent
    exponent -= 1;
    if (exponent < -14) {
        const double scaled = magnitude * std::ldexp(1.0, 24);
        return static_cast<std::uint16_t>((sign << 15) | static_cast<unsigned>(std::lround(scaled)));
    }
    if (exponent > 15) {
        return static_cast<std::uint16_t>((sign << 15) | 0x7C00);
    }
    const unsigned mantissa =
        static_cast<unsigned>(std::lround((fraction * 2.0 - 1.0) * 1024.0));
    return static_cast<std::uint16_t>((sign << 15) | (static_cast<unsigned>(exponent + 15) << 10) |
                                      (mantissa & 0x3FFu));
}

} // namespace

struct VM::PageStore {
    std::unordered_map<std::uint64_t, std::unique_ptr<std::uint8_t[]>> pages;

    /// Direct-mapped cache in front of the hash map. A program's working set
    /// touches a handful of pages repeatedly, so this turns almost every
    /// access into a compare plus a pointer load instead of a hash lookup.
    ///
    /// 128 entries, not 32. The narrow case (a handful of live pages) was already
    /// covered by 32, but the expensive case was not: an 8 MB array is 128 pages,
    /// and marshalling it walks all 128 twice -- once in, once out. With 32 slots
    /// each page evicts another before it is ever reused, so the hash runs for all
    /// 256 accesses. 128 holds the whole walk.
    ///
    /// The map stays because the address space is sparse and wide, not compact:
    /// the stack starts at 0x1000'0000 (page 4096) and the arena at 0x8000'0000
    /// (page 32768). A flat vector indexed by page number would need one slot per
    /// page between 0 and 32768 no matter how little the program touches, and the
    /// address space is not bounded by kMaxMemoryBytes -- that is only the budget
    /// for bytes actually allocated.
    static constexpr unsigned kCacheEntries = 32;
    struct CacheEntry {
        std::uint64_t index = ~std::uint64_t{0};
        std::uint8_t* page = nullptr;
    };
    CacheEntry cache[kCacheEntries];

    /// Pages are handed out on first touch, so a program that walks a pointer
    /// across the address space would keep asking for 64 KB blocks until the
    /// machine swaps to death. Cap the total and let the interpreter trap on
    /// the next instruction instead of taking the operating system down.
    static constexpr std::uint64_t kMaxPages = VM::kMaxMemoryBytes / kPageSize;

    /// Page budget; `setMemoryLimit` lowers it (never raises it past kMaxPages).
    std::uint64_t maxPages = kMaxPages;
    std::uint64_t pageCount = 0;
    bool exhausted = false;
    /// Handed out once the cap is hit, so a caller that ignores `exhausted`
    /// cannot dereference null; the run traps before it can matter.
    std::uint8_t scratch[kPageSize] = {};

    std::uint8_t* get(std::uint64_t index, bool create) {
        CacheEntry& entry = cache[index % kCacheEntries];
        if (entry.index == index) {
            // A cached *miss* only answers a reader. A writer asking for the same
            // page has to fall through and allocate it.
            //
            // This is not hypothetical, and it was a real access violation: the
            // native-call path read-copies an array into a scratch buffer and
            // write-copies it back, so the first write to a freshly `alloc`ed
            // buffer finds its own miss in the cache. Returning the cached null
            // made `memcpy` write to address 0 -- uncatchable, since that is the
            // process, not the interpreter. Any native function that fills an
            // uninitialised buffer hit it; local arrays escaped because their page
            // already existed.
            if (entry.page != nullptr || !create) {
                return entry.page;
            }
        }

        const auto it = pages.find(index);
        if (it != pages.end()) {
            entry.index = index;
            entry.page = it->second.get();
            return entry.page;
        }
        if (!create) {
            // Remember the miss too: consecutive reads of untouched memory
            // should not each pay for a hash lookup.
            entry.index = index;
            entry.page = nullptr;
            return nullptr;
        }
        if (pageCount >= maxPages) {
            exhausted = true;
            entry.index = ~std::uint64_t{0};   // do not cache the scratch page
            return scratch;
        }
        auto block = std::make_unique<std::uint8_t[]>(kPageSize);
        std::memset(block.get(), 0, kPageSize);
        std::uint8_t* raw = block.get();
        pages.emplace(index, std::move(block));
        ++pageCount;
        entry.index = index;
        entry.page = raw;
        return raw;
    }
};

VM::VM(std::ostream& out, std::ostream& err)
    : m_out(out), m_err(err), m_memory(new PageStore()) {}


//===----------------------------------------------------------------------===//
// Shared libraries
//===----------------------------------------------------------------------===//
//
// `library "x";` in a .ax file turns its bodyless functions into external ones.
// The .acvm records the library name and the symbol, and this is where they are
// resolved -- at run time, so adding a C function needs a new .dll/.so rather
// than a new compiler (docs/NATIVE.md).
//
// A module that uses no external functions never reaches this code, which is
// what keeps the common case self contained.

void* vmOpenLibrary(const std::string& name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::LoadLibraryA(name.c_str()));
#else
    return ::dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
}

void* vmLookupSymbol(void* handle, const std::string& symbol) {
    if (handle == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol.c_str()));
#else
    return ::dlsym(handle, symbol.c_str());
#endif
}

void vmCloseLibrary(void* handle) {
    if (handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

std::string vmExecutablePath() {
#if defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD length = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::string(buffer, static_cast<std::size_t>(length));
#elif defined(__APPLE__)
    char buffer[4096];
    std::uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    return std::string(buffer);
#else
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    return std::string(buffer, static_cast<std::size_t>(length));
#endif
}

namespace {

std::string loaderError() {
#if defined(_WIN32)
    return "the loader rejected it (error " + std::to_string(::GetLastError()) + ")";
#else
    const char* detail = ::dlerror();
    return detail != nullptr ? std::string(detail) : std::string("unknown loader error");
#endif
}

bool endsWith(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// Platform specific file names: `ext` means ext.dll on Windows, libext.so on
/// Linux, libext.dylib on macOS.
std::vector<std::string> libraryFileNames(const std::string& name) {
    // Each suffix is checked against the length first: `name.size() - 6` on a
    // short name wraps around, and `std::string::compare` with an out of range
    // position throws -- which used to abort the process for a five character
    // library name such as `library "abcde";`.
    if (endsWith(name, ".dll") || endsWith(name, ".so") || endsWith(name, ".dylib")) {
        return {name};
    }
    std::vector<std::string> candidates;
#if defined(_WIN32)
    candidates.push_back(name + ".dll");
#elif defined(__APPLE__)
    candidates.push_back("lib" + name + ".dylib");
    candidates.push_back(name + ".dylib");
#else
    candidates.push_back("lib" + name + ".so");
    candidates.push_back(name + ".so");
#endif
    return candidates;
}

} // namespace

VM::~VM() {
    for (void* handle : m_libraryHandles) {
        if (handle != nullptr) {
            // 卸载之前先问库要不要收拾自己。
            //
            // 这不是礼貌，是必需：`parallel` 会缓存一个常驻线程池，worker 在
            // `SleepConditionVariableCS` / `Sleep` 里长眠。`FreeLibrary` 之后它们
            // 的代码页被解除映射，下一次醒来返回时 PC 就指向已释放的地址 ——
            // 实测崩溃报 `access=execute`、目标落在 MEM_FREE 区、地址形如
            // 0x7FF9...（系统 DLL 的基址段）。每十几次运行出一次。
            //
            // 钩子是可选的：库没导出就跳过，普通库不受影响。
            using ShutdownFn = void (*)();
            auto shutdown = reinterpret_cast<ShutdownFn>(
                vmLookupSymbol(handle, "acvm_stdlib_shutdown"));
            if (shutdown != nullptr) {
                shutdown();
            }
        }
        vmCloseLibrary(handle);
    }
}

void* VM::resolveExternal(const Function& function, std::size_t functionIndex,
                          std::string& error) {
    if (m_module == nullptr) {
        error = "internal error: no module loaded";
        return nullptr;
    }

    // Cache first: a resolved symbol never changes while the module is loaded.
    //
    // The two vectors are kept the same length; reading one while the other is
    // shorter is how the first version of this cache segfaulted. The size check
    // is on the resolved flags because that is the vector being indexed.
    if (functionIndex < m_externalResolved.size() && m_externalResolved[functionIndex]) {
        return m_externalSymbols[functionIndex];
    }
    const int library = function.externalLibrary;
    if (library < 0 || static_cast<std::size_t>(library) >= m_module->externalLibraries.size()) {
        error = "internal error: bad external library index";
        return nullptr;
    }

    if (m_libraryHandles.size() < m_module->externalLibraries.size()) {
        m_libraryHandles.resize(m_module->externalLibraries.size(), nullptr);
    }

    if (m_libraryHandles[static_cast<std::size_t>(library)] == nullptr) {
        const std::string& declared = m_module->externalLibraries[static_cast<std::size_t>(library)];
        std::string lastDetail;
        for (const std::string& candidate : libraryFileNames(declared)) {
            // Explicit directories first (module directory, then the
            // toolchain's), then the platform loader path.
            bool opened = false;
            for (const std::string& directory : m_libraryPath) {
                void* handle = vmOpenLibrary(directory + "/" + candidate);
                if (handle != nullptr) {
                    m_libraryHandles[static_cast<std::size_t>(library)] = handle;
                    opened = true;
                    break;
                }
            }
            if (opened) {
                break;
            }
            void* handle = vmOpenLibrary(candidate);
            if (handle != nullptr) {
                m_libraryHandles[static_cast<std::size_t>(library)] = handle;
                break;
            }
            lastDetail = loaderError();
        }
        if (m_libraryHandles[static_cast<std::size_t>(library)] == nullptr) {
            error = "cannot load library '" + declared + "' (" + lastDetail +
                    "); build it next to the module or add it to the loader search path";
            return nullptr;
        }
    }

    void* handle = m_libraryHandles[static_cast<std::size_t>(library)];
    void* symbol = vmLookupSymbol(handle, function.externalSymbol);
    if (symbol == nullptr) {
        error = "library '" + m_module->externalLibraries[static_cast<std::size_t>(library)] +
                "' does not export '" + function.externalSymbol +
                "'; it must have C linkage (extern \"C\")";
        return nullptr;
    }

    // Remember it. Failures are not cached: a missing symbol is a deployment
    // error the caller reports, and re-attempting it keeps that path simple.
    if (m_module != nullptr && functionIndex < m_module->functions.size()) {
        if (m_externalResolved.size() < m_module->functions.size()) {
            m_externalSymbols.resize(m_module->functions.size(), nullptr);
            m_externalResolved.resize(m_module->functions.size(), 0);
        }
        m_externalSymbols[functionIndex] = symbol;
        m_externalResolved[functionIndex] = 1;
    }
    return symbol;
}

//===----------------------------------------------------------------------===//
// Debugger attachment
//===----------------------------------------------------------------------===//

void VM::setDebugHooks(const DebugHooks* hooks) {
    if (hooks == nullptr || hooks->apiVersion != kDebugApiVersion) {
        m_debugHooks = DebugHooks{};
        m_debugAttached = false;
        m_debugAborted = false;
        return;
    }
    m_debugHooks = *hooks;
    m_debugAttached = true;
    m_debugAborted = false;
    refreshDebugInfo();
}

void VM::refreshDebugInfo() {
    // Rebuilt whenever the module changes, because every callback closes over
    // `this` and resolves against `m_module`.
    m_debugInfo = DebugInfo{};
    m_debugInfo.apiVersion = kDebugApiVersion;
    m_debugInfo.host = this;
    m_debugInfo.functionCount = [](void* host) -> std::int32_t {
        const VM* vm = static_cast<const VM*>(host);
        return vm->m_module == nullptr ? 0
                                       : static_cast<std::int32_t>(vm->m_module->functions.size());
    };
    m_debugInfo.functionName = [](void* host, std::int32_t index) -> const char* {
        VM* vm = static_cast<VM*>(host);
        if (vm->m_module == nullptr || index < 0 ||
            static_cast<std::size_t>(index) >= vm->m_module->functions.size()) {
            return "";
        }
        return vm->m_module->functions[static_cast<std::size_t>(index)].name.c_str();
    };
    m_debugInfo.functionHasCode = [](void* host, std::int32_t index) -> std::int32_t {
        const VM* vm = static_cast<const VM*>(host);
        if (vm->m_module == nullptr || index < 0 ||
            static_cast<std::size_t>(index) >= vm->m_module->functions.size()) {
            return 0;
        }
        return vm->m_module->functions[static_cast<std::size_t>(index)].code.empty() ? 0 : 1;
    };
    m_debugInfo.instructionCount = [](void* host, std::int32_t index) -> std::int32_t {
        const VM* vm = static_cast<const VM*>(host);
        if (vm->m_module == nullptr || index < 0 ||
            static_cast<std::size_t>(index) >= vm->m_module->functions.size()) {
            return 0;
        }
        return static_cast<std::int32_t>(
            vm->m_module->functions[static_cast<std::size_t>(index)].code.size());
    };
    m_debugInfo.opcodeName = [](void* host, std::int32_t opcode) -> const char* {
        VM* vm = static_cast<VM*>(host);
        // `opCodeName` returns a view into a static table, but the ABI is
        // `const char*`, so a wrapper keeps the lifetime obvious.
        const std::string_view name = opCodeName(static_cast<OpCode>(opcode));
        vm->m_debugScratch.assign(name);
        return vm->m_debugScratch.c_str();
    };
    m_debugInfo.instructionText = [](void* host, std::int32_t index, std::int32_t ip) -> const char* {
        VM* vm = static_cast<VM*>(host);
        if (vm->m_module == nullptr || index < 0 || ip < 0 ||
            static_cast<std::size_t>(index) >= vm->m_module->functions.size()) {
            return "";
        }
        const Function& function = vm->m_module->functions[static_cast<std::size_t>(index)];
        if (static_cast<std::size_t>(ip) >= function.code.size()) {
            return "";
        }
        std::ostringstream text;
        formatInstruction(*vm->m_module, function, static_cast<std::size_t>(ip), text);
        vm->m_debugScratch = text.str();
        return vm->m_debugScratch.c_str();
    };
    m_debugInfo.sourceFile = [](void* host, std::int32_t index, std::int32_t ip) -> const char* {
        VM* vm = static_cast<VM*>(host);
        if (vm->m_module == nullptr || index < 0 || ip < 0 ||
            static_cast<std::size_t>(index) >= vm->m_module->functions.size()) {
            return "";
        }
        const Function& function = vm->m_module->functions[static_cast<std::size_t>(index)];
        const SourceLine at = function.lineAt(static_cast<std::size_t>(ip));
        if (!at.isValid() || at.file >= vm->m_module->sourceFiles.size()) {
            return "";
        }
        return vm->m_module->sourceFiles[at.file].c_str();
    };
    m_debugInfo.sourceLine = [](void* host, std::int32_t index, std::int32_t ip) -> std::int32_t {
        const VM* vm = static_cast<const VM*>(host);
        if (vm->m_module == nullptr || index < 0 || ip < 0 ||
            static_cast<std::size_t>(index) >= vm->m_module->functions.size()) {
            return 0;
        }
        const Function& function = vm->m_module->functions[static_cast<std::size_t>(index)];
        const SourceLine at = function.lineAt(static_cast<std::size_t>(ip));
        return at.isValid() ? static_cast<std::int32_t>(at.line) : 0;
    };
    m_debugInfo.entryFunctionName = [](void* host) -> const char* {
        VM* vm = static_cast<VM*>(host);
        if (vm->m_module == nullptr || vm->m_module->entryPoint < 0 ||
            static_cast<std::size_t>(vm->m_module->entryPoint) >=
                vm->m_module->functions.size()) {
            return "";
        }
        return vm->m_module->functions[static_cast<std::size_t>(vm->m_module->entryPoint)]
            .name.c_str();
    };
}

//===----------------------------------------------------------------------===//
// Memory
//===----------------------------------------------------------------------===//

bool VM::memoryExhausted() const { return m_memory->exhausted; }

void VM::setMemoryLimit(std::uint64_t bytes) {
    const std::uint64_t pages = bytes / kPageSize;
    m_memory->maxPages = pages == 0 ? 1 : (pages > PageStore::kMaxPages ? PageStore::kMaxPages : pages);
}


bool VM::readBytes(std::uint64_t addr, void* dst, std::size_t len) const {

    auto* out = static_cast<std::uint8_t*>(dst);

    // Fast path: the access fits inside one page, which is the common case.
    const std::uint64_t firstOffset = addr & kPageMask;
    if (firstOffset + len <= kPageSize) {
        const std::uint8_t* page = m_memory->get(addr >> kPageShift, false);
        if (page != nullptr) {
            std::memcpy(out, page + firstOffset, len);
        } else {
            std::memset(out, 0, len);
        }
        return true;
    }

    while (len > 0) {
        const std::uint64_t pageIndex = addr >> kPageShift;
        const std::uint64_t offset = addr & kPageMask;
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(len, kPageSize - offset));
        const std::uint8_t* page = m_memory->get(pageIndex, false);
        if (page != nullptr) {
            std::memcpy(out, page + offset, chunk);
        } else {
            std::memset(out, 0, chunk);
        }
        addr += chunk;
        out += chunk;
        len -= chunk;
    }
    return true;
}

bool VM::writeBytes(std::uint64_t addr, const void* src, std::size_t len) {
    const auto* in = static_cast<const std::uint8_t*>(src);

    const std::uint64_t firstOffset = addr & kPageMask;
    if (firstOffset + len <= kPageSize) {
        std::memcpy(m_memory->get(addr >> kPageShift, true) + firstOffset, in, len);
        return true;
    }

    while (len > 0) {
        const std::uint64_t pageIndex = addr >> kPageShift;
        const std::uint64_t offset = addr & kPageMask;
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(len, kPageSize - offset));
        std::uint8_t* page = m_memory->get(pageIndex, true);
        std::memcpy(page + offset, in, chunk);
        addr += chunk;
        in += chunk;
        len -= chunk;
    }
    return true;
}

bool VM::zeroBytes(std::uint64_t addr, std::size_t len) {
    std::vector<std::uint8_t> zeros(std::min<std::size_t>(len, 4096), 0);
    while (len > 0) {
        const std::size_t chunk = std::min(len, zeros.size());
        writeBytes(addr, zeros.data(), chunk);
        addr += chunk;
        len -= chunk;
    }
    return true;
}

const MemType& VM::memType(int index) const {
    static const MemType fallback{};
    if (m_module == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= m_module->memTypes.size()) {
        return fallback;
    }
    return m_module->memTypes[static_cast<std::size_t>(index)];
}

bool VM::loadScalar(std::uint64_t addr, int typeIndex, Value& out) const {
    const MemType& type = memType(typeIndex);
    switch (type.kind) {
    case MemType::Kind::Int:
    case MemType::Kind::Bool:
    case MemType::Kind::Char: {
        if (type.size == 0 || type.size > 16) {
            return false;
        }
        // Sizes 1/2/4/8 cover every scalar the front end emits; reading them
        // with one memcpy instead of a byte loop is worth a lot in a hot loop.
        unsigned __int128 raw = 0;
        switch (type.size) {
        case 1: {
            std::uint8_t value = 0;
            readBytes(addr, &value, 1);
            raw = value;
            break;
        }
        case 2: {
            std::uint16_t value = 0;
            readBytes(addr, &value, 2);
            raw = value;
            break;
        }
        case 4: {
            std::uint32_t value = 0;
            readBytes(addr, &value, 4);
            raw = value;
            break;
        }
        case 8: {
            std::uint64_t value = 0;
            readBytes(addr, &value, 8);
            raw = value;
            break;
        }
        default: {
            std::uint8_t buffer[16] = {};
            readBytes(addr, buffer, type.size);
            for (int i = static_cast<int>(type.size) - 1; i >= 0; --i) {
                raw = (raw << 8) | buffer[i];
            }
            break;
        }
        }
        const unsigned bits = type.bits == 0 ? static_cast<unsigned>(type.size * 8) : type.bits;
        raw &= maskFor(bits);
        Int128 value = static_cast<Int128>(raw);
        if (!type.isUnsigned && bits < 128) {
            if ((raw & (static_cast<unsigned __int128>(1) << (bits - 1))) != 0) {
                value = static_cast<Int128>(raw | ~maskFor(bits));
            }
        }
        out = Value::fromInt(value);
        return true;
    }
    case MemType::Kind::Float: {
        if (type.size == 8) {
            double value = 0;
            readBytes(addr, &value, sizeof(value));
            out = Value::fromFloat(value);
            return true;
        }
        if (type.size == 4) {
            float value = 0;
            readBytes(addr, &value, sizeof(value));
            out = Value::fromFloat(static_cast<double>(value));
            return true;
        }
        if (type.size == 2) {
            std::uint16_t bits = 0;
            readBytes(addr, &bits, sizeof(bits));
            out = Value::fromFloat(halfToDouble(bits));
            return true;
        }
        return false;
    }
    case MemType::Kind::Ptr: {
        std::uint64_t value = 0;
        readBytes(addr, &value, sizeof(value));
        out = Value::fromPtr(value);
        return true;
    }
    }
    return false;
}

bool VM::storeScalar(std::uint64_t addr, int typeIndex, const Value& value) {
    const MemType& type = memType(typeIndex);
    switch (type.kind) {
    case MemType::Kind::Int:
    case MemType::Kind::Bool:
    case MemType::Kind::Char: {
        if (type.size == 0 || type.size > 16) {
            return false;
        }
        const unsigned bits = type.bits == 0 ? static_cast<unsigned>(type.size * 8) : type.bits;
        unsigned __int128 raw = static_cast<unsigned __int128>(value.asInt());
        raw &= maskFor(bits);
        switch (type.size) {
        case 1: {
            const std::uint8_t narrowed = static_cast<std::uint8_t>(raw);
            writeBytes(addr, &narrowed, 1);
            return true;
        }
        case 2: {
            const std::uint16_t narrowed = static_cast<std::uint16_t>(raw);
            writeBytes(addr, &narrowed, 2);
            return true;
        }
        case 4: {
            const std::uint32_t narrowed = static_cast<std::uint32_t>(raw);
            writeBytes(addr, &narrowed, 4);
            return true;
        }
        case 8: {
            const std::uint64_t narrowed = static_cast<std::uint64_t>(raw);
            writeBytes(addr, &narrowed, 8);
            return true;
        }
        default: {
            std::uint8_t buffer[16] = {};
            for (unsigned i = 0; i < type.size; ++i) {
                buffer[i] = static_cast<std::uint8_t>((raw >> (8 * i)) & 0xFF);
            }
            writeBytes(addr, buffer, type.size);
            return true;
        }
        }
    }
    case MemType::Kind::Float: {
        const double number = value.asFloat();
        if (type.size == 8) {
            writeBytes(addr, &number, sizeof(number));
            return true;
        }
        if (type.size == 4) {
            const float narrowed = static_cast<float>(number);
            writeBytes(addr, &narrowed, sizeof(narrowed));
            return true;
        }
        if (type.size == 2) {
            const std::uint16_t bits = doubleToHalf(number);
            writeBytes(addr, &bits, sizeof(bits));
            return true;
        }
        return false;
    }
    case MemType::Kind::Ptr: {
        const std::uint64_t pointer = value.asPtr();
        writeBytes(addr, &pointer, sizeof(pointer));
        return true;
    }
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Allocation
//===----------------------------------------------------------------------===//

std::uint64_t VM::heapAlloc(std::uint64_t size, bool zeroed) {
    if (size == 0) {
        size = 1;
    }
    size = (size + 7) / 8 * 8;

    for (HeapBlock& block : m_heapBlocks) {
        if (block.free && block.size >= size) {
            block.free = false;
            if (zeroed) {
                zeroBytes(block.addr, block.size);
            }
            return block.addr;
        }
    }

    const std::uint64_t addr = m_heapTop;
    m_heapTop += size;
    m_heapTop = (m_heapTop + 15) / 16 * 16;
    m_heapBlocks.push_back(HeapBlock{addr, size, false});
    if (zeroed) {
        zeroBytes(addr, size);
    }
    return addr;
}

void VM::heapFree(std::uint64_t addr) {
    if (addr == 0) {
        return;
    }
    for (HeapBlock& block : m_heapBlocks) {
        if (!block.free && addr >= block.addr && addr < block.addr + block.size) {
            block.free = true;
            break;
        }
    }
    // Coalesce adjacent free blocks.
    std::sort(m_heapBlocks.begin(), m_heapBlocks.end(),
              [](const HeapBlock& lhs, const HeapBlock& rhs) { return lhs.addr < rhs.addr; });
    for (std::size_t i = 0; i + 1 < m_heapBlocks.size();) {
        if (m_heapBlocks[i].free && m_heapBlocks[i + 1].free &&
            m_heapBlocks[i].addr + m_heapBlocks[i].size == m_heapBlocks[i + 1].addr) {
            m_heapBlocks[i].size += m_heapBlocks[i + 1].size;
            m_heapBlocks.erase(m_heapBlocks.begin() + static_cast<std::ptrdiff_t>(i + 1));
            continue;
        }
        ++i;
    }
}

std::uint64_t VM::arenaAlloc(Process& process, std::uint64_t size, std::uint64_t align) {
    if (align < 1) {
        align = 1;
    }
    process.arenaOffset = (process.arenaOffset + align - 1) / align * align;
    const std::uint64_t addr = process.arenaBase + process.arenaOffset;
    process.arenaOffset += size;
    if (process.arenaOffset > (std::uint64_t{16} << 20)) {
        return 0; // arena exhausted
    }
    return addr;
}

//===----------------------------------------------------------------------===//
// Strings
//===----------------------------------------------------------------------===//

namespace {

/// Bytes compared per `readBytes` call. Big enough that a short string is one call,
/// small enough to sit on the stack.
constexpr std::size_t kCompareChunk = 256;

}  // namespace

int VM::compareStrings(Value lhs, Value rhs) const {
    const auto descriptorOf = [this](Value value, std::uint64_t& data, std::uint64_t& length) {
        data = 0;
        length = 0;
        if (value.tag() != Value::Tag::Addr && value.tag() != Value::Tag::Ptr) {
            return false;
        }
        const std::uint64_t descriptor = value.asPtr();
        if (!readBytes(descriptor, &data, 8) || !readBytes(descriptor + 8, &length, 8)) {
            return false;
        }
        return true;
    };

    std::uint64_t leftData = 0;
    std::uint64_t leftLength = 0;
    std::uint64_t rightData = 0;
    std::uint64_t rightLength = 0;
    if (!descriptorOf(lhs, leftData, leftLength) ||
        !descriptorOf(rhs, rightData, rightLength)) {
        return 0;   // neither is a string: treat as equal, matching readString's {}
    }
    if (leftLength == 0 || leftData == 0) {
        return (rightLength == 0 || rightData == 0) ? 0 : -1;
    }
    if (rightLength == 0 || rightData == 0) {
        return 1;
    }

    const std::size_t shared = static_cast<std::size_t>(
        leftLength < rightLength ? leftLength : rightLength);
    std::uint8_t leftChunk[kCompareChunk];
    std::uint8_t rightChunk[kCompareChunk];
    std::size_t done = 0;
    while (done < shared) {
        const std::size_t take =
            std::min(kCompareChunk, shared - done);
        if (!readBytes(leftData + done, leftChunk, take) ||
            !readBytes(rightData + done, rightChunk, take)) {
            return 0;
        }
        const int order = std::memcmp(leftChunk, rightChunk, take);
        if (order != 0) {
            return order < 0 ? -1 : 1;
        }
        done += take;
    }
    // Every shared byte matched, so the shorter one sorts first.
    if (leftLength == rightLength) {
        return 0;
    }
    return leftLength < rightLength ? -1 : 1;
}

bool VM::equalStrings(Value lhs, Value rhs) const {
    const auto descriptorOf = [this](Value value, std::uint64_t& data, std::uint64_t& length) {
        data = 0;
        length = 0;
        if (value.tag() != Value::Tag::Addr && value.tag() != Value::Tag::Ptr) {
            return false;
        }
        const std::uint64_t descriptor = value.asPtr();
        if (!readBytes(descriptor, &data, 8) || !readBytes(descriptor + 8, &length, 8)) {
            return false;
        }
        return true;
    };

    std::uint64_t leftData = 0;
    std::uint64_t leftLength = 0;
    std::uint64_t rightData = 0;
    std::uint64_t rightLength = 0;
    if (!descriptorOf(lhs, leftData, leftLength) ||
        !descriptorOf(rhs, rightData, rightLength)) {
        return true;
    }
    const bool leftEmpty = leftLength == 0 || leftData == 0;
    const bool rightEmpty = rightLength == 0 || rightData == 0;
    if (leftEmpty || rightEmpty) {
        return leftEmpty && rightEmpty;
    }
    if (leftLength != rightLength) {
        return false;
    }

    // Same length: compare the whole thing in chunks. A length mismatch is already
    // decided above, so there is nothing left to compare after the loop.
    std::uint8_t leftChunk[kCompareChunk];
    std::uint8_t rightChunk[kCompareChunk];
    std::size_t done = 0;
    const std::size_t total = static_cast<std::size_t>(leftLength);
    while (done < total) {
        const std::size_t take = std::min(kCompareChunk, total - done);
        if (!readBytes(leftData + done, leftChunk, take) ||
            !readBytes(rightData + done, rightChunk, take)) {
            return false;
        }
        if (std::memcmp(leftChunk, rightChunk, take) != 0) {
            return false;
        }
        done += take;
    }
    return true;
}

std::string VM::readString(Value value) const {
    // A string is always addressed, never held inline, and two tag kinds can
    // carry that address: `Addr` for a descriptor that lives in a local or
    // global slot, and `Ptr` for the result of a call returning an aggregate --
    // the epilogue hands back the destination pointer it wrote to. Accepting
    // only `Addr` made `print(join(...))` print nothing at all, because the
    // argument silently read as the empty string.
    if (value.tag() != Value::Tag::Addr && value.tag() != Value::Tag::Ptr) {
        return {};
    }
    const std::uint64_t descriptor = value.asPtr();
    std::uint64_t data = 0;
    std::uint64_t length = 0;
    readBytes(descriptor, &data, 8);
    readBytes(descriptor + 8, &length, 8);
    if (length == 0 || data == 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    readBytes(data, result.data(), static_cast<std::size_t>(length));
    return result;
}

std::uint64_t VM::makeStringInArena(Process& process, const std::string& text) {
    const std::uint64_t descriptor = arenaAlloc(process, 16 + text.size() + 1, 8);
    if (descriptor == 0) {
        return 0;
    }
    const std::uint64_t data = descriptor + 16;
    const std::uint64_t length = text.size();
    writeBytes(descriptor, &data, 8);
    writeBytes(descriptor + 8, &length, 8);
    if (!text.empty()) {
        writeBytes(data, text.data(), text.size());
    }
    return descriptor;
}

std::uint64_t VM::makeStringOnHeap(const std::string& text) {
    const std::uint64_t descriptor = heapAlloc(16 + text.size() + 1, false);
    const std::uint64_t data = descriptor + 16;
    const std::uint64_t length = text.size();
    writeBytes(descriptor, &data, 8);
    writeBytes(descriptor + 8, &length, 8);
    if (!text.empty()) {
        writeBytes(data, text.data(), text.size());
    }
    return descriptor;
}

//===----------------------------------------------------------------------===//
// Setup and scheduling
//===----------------------------------------------------------------------===//

bool VM::prepare(const Module& module, std::string& error) {
    m_module = &module;
    // Before anything runs: a debugger resolves function names through this table.
    refreshDebugInfo();
    m_heapTop = kHeapBase;
    m_heapBlocks.clear();
    m_stringAddresses.assign(module.strings.size(), 0);
    m_eventWasTrue.assign(module.events.size(), false);
    m_budget = 4000ull * 1000ull * 1000ull;
    m_instructions = 0;
    for (std::uint64_t& count : m_opcodeCounts) {
        count = 0;
    }

    zeroBytes(kGlobalBase, std::max<std::uint32_t>(module.globalDataSize, 8));

    std::uint64_t cursor = kStringBase;
    for (std::size_t i = 0; i < module.strings.size(); ++i) {
        const std::string& text = module.strings[i];
        const std::uint64_t descriptor = cursor;
        const std::uint64_t data = descriptor + 16;
        const std::uint64_t length = text.size();
        writeBytes(descriptor, &data, 8);
        writeBytes(descriptor + 8, &length, 8);
        if (!text.empty()) {
            writeBytes(data, text.data(), text.size());
        }
        m_stringAddresses[i] = descriptor;
        cursor = (data + length + 8) / 8 * 8;
    }

    (void)error;
    return true;
}

bool VM::startProcess(Process& process, const Function* entry, std::uint64_t stackBase,
                      std::uint64_t arenaBase) {
    process.frames.clear();
    process.stack.clear();
    process.stack.reserve(4096);
    process.frames.reserve(256);
    process.arenaMarks.clear();
    process.stackBase = stackBase;
    process.stackTop = stackBase;
    process.arenaBase = arenaBase;
    process.arenaOffset = 0;
    process.finished = false;

    const std::uint64_t frameSize = std::max<std::uint64_t>(entry->frameSize, 8);
    zeroBytes(process.stackTop, frameSize);
    process.frames.push_back(Frame{entry, 0, process.stackTop});
    process.stackTop += frameSize;
    return true;
}

bool VM::run(const Module& module, std::string& error) {
    error.clear();
    if (!prepare(module, error)) {
        return false;
    }

    // The debugger learns the program here, before the global initialiser runs:
    // a breakpoint inside a global's initialiser is unusual but legal, and it
    // should behave like any other.
    if (m_debugAttached && m_debugHooks.onStart != nullptr) {
        m_debugHooks.onStart(m_debugHooks.user, &m_debugInfo);
    }

    if (module.initFunction >= 0) {
        if (!runFunctionToCompletion(module.initFunction, error)) {
            return false;
        }
    }

    m_processes.clear();
    m_externalSymbols.clear();
    m_externalResolved.clear();

    m_processes.emplace_back();
    Process& main = m_processes.back();
    main.isMain = true;
    if (module.entryPoint < 0 ||
        static_cast<std::size_t>(module.entryPoint) >= module.functions.size()) {
        error = "no 'main' function";
        return false;
    }
    startProcess(main, &module.functions[static_cast<std::size_t>(module.entryPoint)], kStackBase,
                 kArenaBase);

    for (std::size_t i = 0; i < module.processes.size(); ++i) {
        const int index = module.processes[i];
        if (index < 0 || static_cast<std::size_t>(index) >= module.functions.size()) {
            continue;
        }
        m_processes.emplace_back();
        Process& process = m_processes.back();
        startProcess(process, &module.functions[static_cast<std::size_t>(index)],
                     kStackBase + (i + 1) * kStackStride,
                     kArenaBase + (i + 1) * kArenaStride);
    }

    while (true) {
        bool anyRunnable = false;
        for (Process& process : m_processes) {
            if (process.finished) {
                continue;
            }
            anyRunnable = true;
            if (!runQuantum(process, error)) {
                // The trap message is already in `error`; the debugger gets it
                // verbatim so it can show exactly what the VM would have printed.
                if (m_debugAttached && m_debugHooks.onTrap != nullptr) {
                    m_debugHooks.onTrap(m_debugHooks.user, error.c_str());
                }
                return false;
            }
            if (process.isMain && process.finished) {
                if (m_debugAttached && m_debugHooks.onFinish != nullptr) {
                    m_debugHooks.onFinish(m_debugHooks.user);
                }
                return true;
            }
        }
        if (!anyRunnable) {
            if (m_debugAttached && m_debugHooks.onFinish != nullptr) {
                m_debugHooks.onFinish(m_debugHooks.user);
            }
            return true;
        }
    }
}

bool VM::runQuantum(Process& process, std::string& error) {
    while (true) {
        const Step result = step(process, error);
        if (result == Step::Trap) {
            return false;
        }
        if (result == Step::Finished) {
            process.finished = true;
            return true;
        }
        if (result == Step::SafePoint) {
            if (!evaluateEvents(process, error)) {
                return false;
            }
            return true;
        }
    }
}

bool VM::runFunctionToCompletion(int functionIndex, std::string& error) {
    if (m_module == nullptr || functionIndex < 0 ||
        static_cast<std::size_t>(functionIndex) >= m_module->functions.size()) {
        error = "invalid function index";
        return false;
    }
    Process scratch;
    const std::uint64_t scratchStack = kStackBase + 0x0F00'0000;
    const std::uint64_t scratchArena = kArenaBase + 0x0F00'0000;
    startProcess(scratch, &m_module->functions[static_cast<std::size_t>(functionIndex)],
                 scratchStack, scratchArena);

    while (!scratch.finished) {
        const Step result = step(scratch, error);
        if (result == Step::Trap) {
            return false;
        }
        if (result == Step::Finished) {
            scratch.finished = true;
        }
    }
    return true;
}

bool VM::evaluateEvents(Process& current, std::string& error) {
    if (m_module == nullptr || m_insideEvent || m_module->events.empty()) {
        return true;
    }
    m_insideEvent = true;

    for (std::size_t i = 0; i < m_module->events.size(); ++i) {
        const int conditionIndex = i < m_module->eventConditions.size()
                                       ? m_module->eventConditions[i]
                                       : -1;
        const int handlerIndex = m_module->events[i];
        bool triggered = false;

        if (conditionIndex >= 0) {
            Process scratch;
            const std::uint64_t scratchStack = kStackBase + 0x0E00'0000;
            const std::uint64_t scratchArena = kArenaBase + 0x0E00'0000;
            startProcess(scratch,
                         &m_module->functions[static_cast<std::size_t>(conditionIndex)],
                         scratchStack, scratchArena);
            while (!scratch.finished) {
                const Step result = step(scratch, error);
                if (result == Step::Trap) {
                    m_insideEvent = false;
                    return false;
                }
                if (result == Step::Finished) {
                    scratch.finished = true;
                }
            }
            if (!scratch.stack.empty()) {
                triggered = scratch.stack.back().asInt() != 0;
            }
        }

        const bool wasTrue = m_eventWasTrue[i];
        m_eventWasTrue[i] = triggered;
        if (triggered && !wasTrue && handlerIndex >= 0) {
            Process scratch;
            const std::uint64_t scratchStack = kStackBase + 0x0D00'0000;
            const std::uint64_t scratchArena = kArenaBase + 0x0D00'0000;
            startProcess(scratch, &m_module->functions[static_cast<std::size_t>(handlerIndex)],
                         scratchStack, scratchArena);
            while (!scratch.finished) {
                const Step result = step(scratch, error);
                if (result == Step::Trap) {
                    m_insideEvent = false;
                    return false;
                }
                if (result == Step::Finished) {
                    scratch.finished = true;
                }
            }
        }
    }

    (void)current;
    m_insideEvent = false;
    return true;
}

bool VM::trap(std::string& error, TrapKind kind, const std::string& detail) {
    error = trapKindName(kind);
    if (!detail.empty()) {
        error += ": " + detail;
    }
    return false;
}

} // namespace acvm

namespace acvm {

//===--- bridge for generated code ---------------------------------------===//
//
// The three operations the JIT is allowed. Each one mirrors an interpreter path exactly;
// if these disagree with VMStep.cpp the differential test reports it as a difference in
// program output, which is the point.

bool VM::JitBridge::loadSlot(VM& vm, std::uint64_t address, int typeIndex,
                             std::int64_t& out) {
    Value value;
    if (!vm.loadScalar(address, typeIndex, value)) {
        return false;
    }
    if (value.tag() == Value::Tag::Float) {
        return false;
    }
    // A pointer-typed load has to be read as a pointer: the payload is the same 64 bits,
    // but a Value tagged Ptr and one tagged Int are not interchangeable, and reading a
    // string's address as an integer gives a number that no longer works as an address.
    out = vm.memType(typeIndex).isPtr()
              ? static_cast<std::int64_t>(value.asPtr())
              : static_cast<std::int64_t>(value.asInt());
    return true;
}

bool VM::JitBridge::storeSlot(VM& vm, std::uint64_t address, int typeIndex,
                              std::int64_t value) {
    return vm.storeScalar(address, typeIndex,
                          vm.memType(typeIndex).isPtr()
                              ? Value::fromPtr(static_cast<std::uint64_t>(value))
                              : Value::fromInt(static_cast<Int128>(value)));
}

bool VM::JitBridge::loadThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                                       std::int64_t& out) {
    if (address == 0) {
        return false;
    }
    Value value;
    if (!vm.loadScalar(address, typeIndex, value) || value.tag() == Value::Tag::Float) {
        return false;
    }
    out = vm.memType(typeIndex).isPtr()
              ? static_cast<std::int64_t>(value.asPtr())
              : static_cast<std::int64_t>(value.asInt());
    return true;
}

bool VM::JitBridge::storeThroughPointer(VM& vm, std::uint64_t address, int typeIndex,
                                        std::int64_t value) {
    if (address == 0) {
        return false;
    }
    return vm.storeScalar(address, typeIndex,
                          vm.memType(typeIndex).isPtr()
                              ? Value::fromPtr(static_cast<std::uint64_t>(value))
                              : Value::fromInt(static_cast<Int128>(value)));
}

bool VM::JitBridge::readRaw64(VM& vm, std::uint64_t address, std::int64_t& out) {
    std::uint64_t raw = 0;
    if (!vm.readBytes(address, &raw, sizeof(raw))) {
        return false;
    }
    out = static_cast<std::int64_t>(raw);
    return true;
}

bool VM::JitBridge::constantString(VM& vm, int index, std::int64_t& out) {
    // `prepare()` materialised every string literal once, at `kStringBase`, and nothing
    // ever moves or frees one -- the same property that makes pinning frame pages sound.
    // So this is a lookup rather than an allocation, and the address it returns stays
    // valid for the life of the process.
    //
    // The interpreter's CONST_STR pushes exactly this address, tagged as a pointer. Using
    // one helper for both is what keeps the compiled path from inventing its own notion of
    // where a literal lives, which is the mistake that made a compiled callee read an
    // empty string earlier.
    const std::size_t slot = static_cast<std::size_t>(index);
    if (index < 0 || slot >= vm.m_stringAddresses.size()) {
        return false;
    }
    out = static_cast<std::int64_t>(vm.m_stringAddresses[slot]);
    return true;
}

void VM::JitBridge::zeroThroughPointer(VM& vm, std::uint64_t address,
                                       std::uint64_t size) {
    // MEM_ZERO, verbatim: the null check lives in the generated code (so it can branch to
    // the trap exit) and this only writes. The interpreter ignores the result of
    // `zeroBytes` here too, because an address that fails to resolve was already caught by
    // the null check or is inside a page the process owns.
    vm.zeroBytes(address, static_cast<std::size_t>(size));
}

void VM::JitBridge::copyThroughPointers(VM& vm, std::uint64_t destination,
                                        std::uint64_t source, std::uint64_t size) {
    // MEM_COPY. Straight `readBytes`/`writeBytes` rather than `std::memcpy`: a VM address
    // is not a host pointer, and the page table has to be walked -- which is exactly the
    // thing that would go wrong if this reimplemented the copy instead of calling in.
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    if (!vm.readBytes(source, buffer.data(), buffer.size())) {
        return;
    }
    vm.writeBytes(destination, buffer.data(), buffer.size());
}

int VM::JitBridge::pinFramePages(VM& vm, std::uint64_t frameBase,
                                 std::uint64_t frameSize, void** out, int maxPages) {
    if (vm.m_memory == nullptr) {
        return 0;
    }
    const std::uint64_t firstPage = frameBase >> kPageShift;
    const std::uint64_t lastByte = frameBase + (frameSize == 0 ? 1 : frameSize) - 1;
    const std::uint64_t lastPage = lastByte >> kPageShift;
    const std::uint64_t count = lastPage - firstPage + 1;
    if (count > static_cast<std::uint64_t>(maxPages)) {
        return 0;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        // `true`: a frame slot may be written before it is read, so the page has to exist.
        std::uint8_t* page = vm.m_memory->get(firstPage + i, true);
        if (page == nullptr) {
            return 0;
        }
        out[i] = page;
    }
    return static_cast<int>(count);
}

bool VM::JitBridge::callFunction(VM& vm, void* process, int functionIndex, int argc,
                                 std::string* error) {
    auto& p = *static_cast<Process*>(process);
    if (vm.m_module == nullptr || functionIndex < 0 ||
        static_cast<std::size_t>(functionIndex) >= vm.m_module->functions.size()) {
        if (error != nullptr) {
            *error = "invalid call target";
        }
        return false;
    }
    const Function& callee = vm.m_module->functions[static_cast<std::size_t>(functionIndex)];

    // Arguments are already on the value stack, in order, because compiled code pushed
    // them there -- so the frame setup is the same one CALL performs. External libraries
    // and aggregates are outside what the lowering emits a call for, which is why this
    // path can assume ordinary value parameters.
    const std::uint64_t frameBase = p.stackTop;
    const std::uint64_t frameSize = std::max<std::uint64_t>(callee.frameSize, 8);
    if (p.frames.size() >= kMaxCallDepth) {
        if (error != nullptr) {
            *error = "call depth exceeded in '" + callee.name + "' (unbounded recursion?)";
        }
        return false;
    }

    std::vector<Value> arguments(static_cast<std::size_t>(argc));
    for (std::size_t i = static_cast<std::size_t>(argc); i-- > 0;) {
        if (p.stack.empty()) {
            if (error != nullptr) {
                *error = "stack underflow in CALL to '" + callee.name + "' (argc " +
                     std::to_string(argc) + ", stack " + std::to_string(p.stack.size()) + ")";
            }
            return false;
        }
        arguments[i] = p.stack.back();
        p.stack.pop_back();
    }

    vm.zeroBytes(frameBase, frameSize);
    p.stackTop += frameSize;
    for (std::size_t i = 0; i < arguments.size() && i < callee.params.size(); ++i) {
        const ParamInfo& param = callee.params[i];
        if (param.aggregate) {
            // An aggregate argument is the address of its descriptor, so the bytes have to
            // be copied into the callee's frame. Storing the pointer instead -- which is
            // what this did at first -- leaves the callee reading an empty string.
            std::vector<std::uint8_t> buffer(param.size);
            vm.readBytes(arguments[i].asPtr(), buffer.data(), param.size);
            vm.writeBytes(frameBase + param.offset, buffer.data(), param.size);
        } else {
            vm.storeScalar(frameBase + param.offset, param.memType, arguments[i]);
        }
    }

    p.frames.push_back(Frame{&callee, 0, frameBase});

    // Compiled callees return through their own epilogue and pop their own frame, so the
    // caller only has to check whether one is still there.
    if (JitFunction native = vm.m_jit.entryFor(*vm.m_module,
                                              static_cast<std::size_t>(functionIndex), *error)) {
        // A nested yield cannot be honoured: the compiled caller is mid-frame on its own
        // native stack and has nowhere to hand control back from. Treated as success.
        return native(&vm, process, frameBase, error) != 0;
    }

    // No compiled entry: run exactly this frame in the interpreter.
    //
    // The frame floor is what makes "exactly this frame" true. Without it, `step()` runs
    // until a safe point, which for a callee with no calls in it means it keeps going
    // after the callee returns -- straight into the *compiled* caller's bytecode. Both
    // implementations then run the same body: this is how `noisy(x)` ended up printing
    // twice and returning the value of the wrong call.
    const std::size_t baseDepth = p.frames.size();
    const std::size_t previousFloor = vm.m_frameFloor;
    vm.m_frameFloor = baseDepth;
    bool ok = true;
    while (p.frames.size() >= baseDepth && !p.finished) {
        const Step result = vm.step(p, *error);
        if (result == Step::Trap) {
            ok = false;
            break;
        }
        if (result == Step::Finished) {
            break;
        }
    }
    vm.m_frameFloor = previousFloor;
    return ok;
}

bool VM::JitBridge::pushValue(void* process, std::int64_t value, bool isPtr) {
    auto& p = *static_cast<Process*>(process);
    p.stack.push_back(isPtr ? Value::fromPtr(static_cast<std::uint64_t>(value))
                            : Value::fromInt(static_cast<Int128>(value)));
    return true;
}

bool VM::JitBridge::safePoint(VM& vm, void* process, std::uint64_t nextIp) {
    auto& p = *static_cast<Process*>(process);
    if (!p.frames.empty()) {
        // The interpreter resumes at the *next* instruction, because its own SAFE_POINT
        // handler has already advanced past itself when it writes ip back.
        p.frames.back().ip = static_cast<std::size_t>(nextIp);
    }
    // Nothing to switch to, so nothing to yield for. This is what keeps a single-process
    // program at full speed while still letting a scheduler with several processes run.
    if (vm.m_processes.size() <= 1) {
        return false;
    }
    return true;
}

bool VM::JitBridge::consumeBudget(VM& vm, std::uint64_t amount) {
    if (vm.m_budget <= amount) {
        vm.m_budget = 0;
        return true;
    }
    vm.m_budget -= amount;
    return false;
}

bool VM::JitBridge::popResult(void* process, std::int64_t* out, bool isPtr) {
    auto& p = *static_cast<Process*>(process);
    if (p.stack.empty()) {
        return false;
    }
    const Value value = p.stack.back();
    p.stack.pop_back();
    if (value.tag() == Value::Tag::Float) {
        return false;
    }
    *out = isPtr ? static_cast<std::int64_t>(value.asPtr())
                 : static_cast<std::int64_t>(value.asInt());
    return true;
}

void VM::JitBridge::returnToCaller(void* process, bool hasResult, std::int64_t result,
                                   bool resultIsPtr) {
    auto& p = *static_cast<Process*>(process);
    if (p.frames.empty()) {
        return;
    }
    const std::uint64_t callerBase = p.frames.back().base;
    p.frames.pop_back();
    p.stackTop = callerBase;
    if (hasResult) {
        p.stack.push_back(resultIsPtr
                              ? Value::fromPtr(static_cast<std::uint64_t>(result))
                              : Value::fromInt(static_cast<Int128>(result)));
    }
}

} // namespace acvm

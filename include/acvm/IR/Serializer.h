//===--- Serializer.h - .acvm bytecode container ------------------------===//
#pragma once

#include <cstddef>
#include <string>

namespace acvm {

struct Module;

/// Container format for compiled modules:
///
///   "ACVM" | u32 version | mem types | globals | strings |
///   functions | entry | global initialiser | process list | event list
///
/// All integers are little endian.
inline constexpr const char* kModuleMagic = "ACVM";
// v3 inserts STR_CMP after STR_EQ. Opcode numbers are serialised, so every
// opcode declared after STR_EQ shifted: a v2 module cannot be decoded correctly
// and is rejected rather than silently mis-executed.
// v5 records whether an external parameter is a `string`. v4 wrote the array flag
// only, so `acvm exec` marshalled a string as one scalar and the callee read the
// VM's descriptor address as characters. A v4 module is rejected for the same
// reason as v2: it would load and then crash, which is worse than refusing it.
inline constexpr unsigned kModuleVersion = 5;

bool saveModule(const Module& module, const std::string& path, std::string& error);
bool loadModule(Module& module, const std::string& path, std::string& error);

/// Reads a module that is already in memory, which is how a payload appended to an
/// executable is loaded: the bytes are there, and writing them to a temporary file first
/// would only add a file to clean up.
///
/// `name` appears in diagnostics, since the payload has no path of its own.
bool loadModuleFromMemory(Module& module, const void* data, std::size_t size,
                          const std::string& name, std::string& error);

} // namespace acvm

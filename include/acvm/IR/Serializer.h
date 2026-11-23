//===--- Serializer.h - .acvm bytecode container ------------------------===//
#pragma once

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
inline constexpr unsigned kModuleVersion = 3;   // v2 added external libraries

bool saveModule(const Module& module, const std::string& path, std::string& error);
bool loadModule(Module& module, const std::string& path, std::string& error);

} // namespace acvm

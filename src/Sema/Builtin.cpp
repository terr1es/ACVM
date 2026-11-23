//===--- Builtin.cpp -----------------------------------------------------===//
#include "acvm/Sema/Builtin.h"

#include <cstring>

namespace acvm {
namespace {

constexpr BuiltinEntry kEntries[] = {
    {"alloc", Builtin::Alloc},           {"alloc0", Builtin::Alloc0},
    {"free", Builtin::Free},             {"memcpy", Builtin::Memcpy},
    {"memmove", Builtin::Memmove},       {"dup", Builtin::Dup},
    {"makeString", Builtin::MakeString}, {"yield", Builtin::Yield},
    {"trap", Builtin::Trap},             {"print", Builtin::Print},
    {"eprint", Builtin::EPrint},         {"log", Builtin::Log},
    {"readLine", Builtin::ReadLine},     {"readToken", Builtin::ReadToken},
    {"argCount", Builtin::ArgCount},     {"argAt", Builtin::ArgAt},
    {"readInt", Builtin::ReadInt},       {"readFloat", Builtin::ReadFloat},
    {"itoa", Builtin::Itoa},             {"ftoa", Builtin::Ftoa},
    {"atoi", Builtin::Atoi},             {"atof", Builtin::Atof},
    {"indexOf", Builtin::IndexOf},       {"lastIndexOf", Builtin::LastIndexOf},
    {"substring", Builtin::Substring},   {"startsWith", Builtin::StartsWith},
    {"endsWith", Builtin::EndsWith},     {"trim", Builtin::Trim},
    {"toUpper", Builtin::ToUpper},       {"toLower", Builtin::ToLower},
    {"sqrt", Builtin::Sqrt},             {"fabs", Builtin::Fabs},
    {"floor", Builtin::Floor},           {"ceil", Builtin::Ceil},
    {"round", Builtin::Round},           {"trunc", Builtin::Trunc},
    {"sin", Builtin::Sin},               {"cos", Builtin::Cos},
    {"tan", Builtin::Tan},               {"asin", Builtin::Asin},
    {"acos", Builtin::Acos},             {"atan", Builtin::Atan},
    {"atan2", Builtin::Atan2},           {"exp", Builtin::Exp},
    {"log_e", Builtin::LogE},            {"log2", Builtin::Log2},
    {"log10", Builtin::Log10},           {"pow", Builtin::Pow},
    {"fmod", Builtin::Fmod},             {"min", Builtin::Min},
    {"max", Builtin::Max},               {"abs", Builtin::Abs},
    {"assert", Builtin::Assert},
    {"sortInts", Builtin::SortInts},       {"sortInt64s", Builtin::SortInt64s},
    {"sortFloats", Builtin::SortFloats},   {"reverseInts", Builtin::ReverseInts},
};

} // namespace

const char* builtinName(Builtin builtin) {
    for (const BuiltinEntry& entry : kEntries) {
        if (entry.id == builtin) {
            return entry.name;
        }
    }
    return "<builtin>";
}

const BuiltinEntry* builtinTable(std::size_t& count) {
    count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

bool isIntrinsicName(const char* name) {
    for (const BuiltinEntry& entry : kEntries) {
        if (std::strcmp(entry.name, name) == 0) {
            // Only the §8.1 intrinsics (plus trap / yield) are unconditional;
            // library functions also need an import in principle, but ACVM
            // keeps them always available.
            return true;
        }
    }
    return false;
}

} // namespace acvm

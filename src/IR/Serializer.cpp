//===--- Serializer.cpp -------------------------------------------------===//
#include "acvm/IR/Serializer.h"

#include "acvm/IR/Bytecode.h"

#include <cstring>
#include <fstream>
#include <string>
#include <utility>

namespace acvm {
namespace {

void writeU8(std::ostream& out, std::uint8_t value) {
    out.put(static_cast<char>(value));
}

void writeU16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xFFu),
                           static_cast<char>((value >> 8) & 0xFFu)};
    out.write(bytes, 2);
}

void writeU32(std::ostream& out, std::uint32_t value) {
    char bytes[4] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF),
                     static_cast<char>((value >> 16) & 0xFF),
                     static_cast<char>((value >> 24) & 0xFF)};
    out.write(bytes, 4);
}

void writeI32(std::ostream& out, std::int32_t value) { writeU32(out, static_cast<std::uint32_t>(value)); }

void writeU64(std::ostream& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        writeU8(out, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

void writeString(std::ostream& out, const std::string& text) {
    writeU32(out, static_cast<std::uint32_t>(text.size()));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void writeValue(std::ostream& out, const Value& value) {
    if (value.tag() == Value::Tag::Float) {
        writeU8(out, 1);
        std::uint64_t bits = 0;
        const double number = value.asFloat();
        std::memcpy(&bits, &number, sizeof(bits));
        writeU64(out, bits);
        return;
    }
    writeU8(out, 0);
    const unsigned __int128 raw = static_cast<unsigned __int128>(value.asInt());
    for (int i = 0; i < 16; ++i) {
        writeU8(out, static_cast<std::uint8_t>((raw >> (8 * i)) & 0xFF));
    }
}

bool readU8(std::istream& in, std::uint8_t& value) {
    char byte = 0;
    if (!in.get(byte)) {
        return false;
    }
    value = static_cast<std::uint8_t>(byte);
    return true;
}

bool readU16(std::istream& in, std::uint16_t& value) {
    unsigned char bytes[2];
    if (!in.read(reinterpret_cast<char*>(bytes), 2)) {
        return false;
    }
    value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                       (static_cast<std::uint16_t>(bytes[1]) << 8));
    return true;
}

bool readU32(std::istream& in, std::uint32_t& value) {
    unsigned char bytes[4];
    if (!in.read(reinterpret_cast<char*>(bytes), 4)) {
        return false;
    }
    value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
    return true;
}

bool readI32(std::istream& in, std::int32_t& value) {
    std::uint32_t raw = 0;
    if (!readU32(in, raw)) {
        return false;
    }
    value = static_cast<std::int32_t>(raw);
    return true;
}

bool readU64(std::istream& in, std::uint64_t& value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        std::uint8_t byte = 0;
        if (!readU8(in, byte)) {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte) << (8 * i);
    }
    return true;
}

bool readString(std::istream& in, std::string& text) {
    std::uint32_t length = 0;
    if (!readU32(in, length)) {
        return false;
    }
    text.resize(length);
    if (length == 0) {
        return true;
    }
    return static_cast<bool>(in.read(text.data(), static_cast<std::streamsize>(length)));
}

bool readValue(std::istream& in, Value& value) {
    std::uint8_t tag = 0;
    if (!readU8(in, tag)) {
        return false;
    }
    if (tag == 1) {
        std::uint64_t bits = 0;
        if (!readU64(in, bits)) {
            return false;
        }
        double number = 0;
        std::memcpy(&number, &bits, sizeof(number));
        value = Value::fromFloat(number);
        return true;
    }
    unsigned __int128 raw = 0;
    for (int i = 0; i < 16; ++i) {
        std::uint8_t byte = 0;
        if (!readU8(in, byte)) {
            return false;
        }
        raw |= static_cast<unsigned __int128>(byte) << (8 * i);
    }
    value = Value::fromInt(static_cast<Int128>(raw));
    return true;
}

} // namespace

bool saveModule(const Module& module, const std::string& path, std::string& error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot open '" + path + "' for writing";
        return false;
    }

    out.write(kModuleMagic, 4);
    writeU32(out, kModuleVersion);

    writeU32(out, static_cast<std::uint32_t>(module.memTypes.size()));
    for (const MemType& type : module.memTypes) {
        writeU8(out, static_cast<std::uint8_t>(type.kind));
        writeU8(out, type.size);
        writeU8(out, type.bits);
        writeU8(out, type.isUnsigned ? 1 : 0);
    }

    writeU32(out, static_cast<std::uint32_t>(module.globals.size()));
    for (const GlobalSlot& slot : module.globals) {
        writeString(out, slot.name);
        writeU32(out, slot.size);
        writeU32(out, slot.align);
        writeU32(out, slot.offset);
    }
    writeU32(out, module.globalDataSize);

    writeU32(out, static_cast<std::uint32_t>(module.strings.size()));
    for (const std::string& text : module.strings) {
        writeString(out, text);
    }

    // Source files (v4): the line tables in the functions index into this list,
    // so a `.acvm` can print `main.ax:42` after the sources are gone.
    writeU32(out, static_cast<std::uint32_t>(module.sourceFiles.size()));
    for (const std::string& file : module.sourceFiles) {
        writeString(out, file);
    }

    // External libraries (v2): the .acvm names the shared objects it needs but
    // does not contain them. Loading happens at run time, so a module with no
    // external functions stays self contained.
    writeU32(out, static_cast<std::uint32_t>(module.externalLibraries.size()));
    for (const std::string& library : module.externalLibraries) {
        writeString(out, library);
    }

    writeU32(out, static_cast<std::uint32_t>(module.functions.size()));
    for (const Function& function : module.functions) {
        writeString(out, function.name);
        writeU32(out, function.frameSize);
        writeU32(out, function.returnSize);
        writeI32(out, function.returnMemType);
        std::uint8_t flags = 0;
        if (function.returnsAggregate) flags |= 1;
        if (function.isProcess) flags |= 2;
        if (function.isEventCondition) flags |= 4;
        if (function.returnsValue) flags |= 8;
        if (function.isSynthetic) flags |= 16;
        if (function.isExternal()) flags |= 32;
        writeU8(out, flags);

        if (function.isExternal()) {
            writeI32(out, function.externalLibrary);
            writeString(out, function.externalSymbol);
            writeI32(out, function.externalReturnKind);
            writeU32(out, static_cast<std::uint32_t>(function.externalParams.size()));
            for (const ExternalParam& param : function.externalParams) {
                writeU8(out, param.isArray ? 1 : 0);
                writeU32(out, param.elemSize);
            }
        }

        writeU32(out, static_cast<std::uint32_t>(function.params.size()));
        for (const ParamInfo& param : function.params) {
            writeU32(out, param.offset);
            writeU32(out, param.size);
            writeI32(out, param.memType);
            writeU8(out, param.aggregate ? 1 : 0);
        }

        writeU32(out, static_cast<std::uint32_t>(function.constants.size()));
        for (const Value& value : function.constants) {
            writeValue(out, value);
        }

        writeU32(out, static_cast<std::uint32_t>(function.code.size()));
        for (std::size_t i = 0; i < function.code.size(); ++i) {
            const Instruction& inst = function.code[i];
            writeU8(out, static_cast<std::uint8_t>(inst.op));
            writeI32(out, inst.a);
            writeI32(out, inst.b);
            // Position travels with the instruction rather than in a second
            // section: a debugger reading the code cannot get them out of step.
            const SourceLine line = i < function.lines.size() ? function.lines[i] : SourceLine{};
            writeU32(out, line.line);
            writeU16(out, line.file);
        }
    }

    writeI32(out, module.entryPoint);
    writeI32(out, module.initFunction);

    writeU32(out, static_cast<std::uint32_t>(module.processes.size()));
    for (const int index : module.processes) {
        writeI32(out, index);
    }
    writeU32(out, static_cast<std::uint32_t>(module.events.size()));
    for (const int index : module.events) {
        writeI32(out, index);
    }
    writeU32(out, static_cast<std::uint32_t>(module.eventConditions.size()));
    for (const int index : module.eventConditions) {
        writeI32(out, index);
    }

    if (!out) {
        error = "failed while writing '" + path + "'";
        return false;
    }
    return true;
}

bool loadModule(Module& module, const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }

    char magic[4] = {};
    if (!in.read(magic, 4) || std::memcmp(magic, kModuleMagic, 4) != 0) {
        error = "'" + path + "' is not an ACVM module";
        return false;
    }

    std::uint32_t version = 0;
    if (!readU32(in, version)) {
        error = "unexpected end of file in '" + path + "'";
        return false;
    }
    if (version != kModuleVersion) {
        error = "unsupported module version " + std::to_string(version) + " (expected " +
                std::to_string(kModuleVersion) + ")";
        return false;
    }

    module = Module{};

    std::uint32_t count = 0;
    if (!readU32(in, count)) {
        error = "truncated memory type table";
        return false;
    }
    module.memTypes.resize(count);
    for (MemType& type : module.memTypes) {
        std::uint8_t kind = 0;
        if (!readU8(in, kind) || !readU8(in, type.size) || !readU8(in, type.bits)) {
            error = "truncated memory type table";
            return false;
        }
        std::uint8_t isUnsigned = 0;
        if (!readU8(in, isUnsigned)) {
            error = "truncated memory type table";
            return false;
        }
        type.kind = static_cast<MemType::Kind>(kind);
        type.isUnsigned = isUnsigned != 0;
    }

    if (!readU32(in, count)) {
        error = "truncated global table";
        return false;
    }
    module.globals.resize(count);
    for (GlobalSlot& slot : module.globals) {
        if (!readString(in, slot.name) || !readU32(in, slot.size) || !readU32(in, slot.align) ||
            !readU32(in, slot.offset)) {
            error = "truncated global table";
            return false;
        }
    }
    if (!readU32(in, module.globalDataSize)) {
        error = "truncated global table";
        return false;
    }

    if (!readU32(in, count)) {
        error = "truncated string table";
        return false;
    }
    module.strings.resize(count);
    for (std::string& text : module.strings) {
        if (!readString(in, text)) {
            error = "truncated string table";
            return false;
        }
    }

    std::uint32_t sourceFileCount = 0;
    if (!readU32(in, sourceFileCount)) {
        error = "truncated source file table";
        return false;
    }
    module.sourceFiles.resize(sourceFileCount);
    for (std::string& file : module.sourceFiles) {
        if (!readString(in, file)) {
            error = "truncated source file table";
            return false;
        }
    }

    std::uint32_t libraryCount = 0;
    if (!readU32(in, libraryCount)) {
        error = "truncated external library table";
        return false;
    }
    module.externalLibraries.resize(libraryCount);
    for (std::string& library : module.externalLibraries) {
        if (!readString(in, library)) {
            error = "truncated external library table";
            return false;
        }
    }

    if (!readU32(in, count)) {
        error = "truncated function table";
        return false;
    }
    module.functions.resize(count);
    for (Function& function : module.functions) {
        if (!readString(in, function.name) || !readU32(in, function.frameSize) ||
            !readU32(in, function.returnSize) || !readI32(in, function.returnMemType)) {
            error = "truncated function header";
            return false;
        }
        std::uint8_t flags = 0;
        if (!readU8(in, flags)) {
            error = "truncated function header";
            return false;
        }
        function.returnsAggregate = (flags & 1) != 0;
        function.isProcess = (flags & 2) != 0;
        function.isEventCondition = (flags & 4) != 0;
        function.returnsValue = (flags & 8) != 0;
        function.isSynthetic = (flags & 16) != 0;

        if ((flags & 32) != 0) {
            if (!readI32(in, function.externalLibrary) ||
                !readString(in, function.externalSymbol) ||
                !readI32(in, function.externalReturnKind)) {
                error = "truncated external function header";
                return false;
            }
            std::uint32_t externalParamCount = 0;
            if (!readU32(in, externalParamCount)) {
                error = "truncated external parameter table";
                return false;
            }
            function.externalParams.resize(externalParamCount);
            for (ExternalParam& param : function.externalParams) {
                std::uint8_t isArray = 0;
                if (!readU8(in, isArray) || !readU32(in, param.elemSize)) {
                    error = "truncated external parameter table";
                    return false;
                }
                param.isArray = isArray != 0;
            }
        }

        std::uint32_t paramCount = 0;
        if (!readU32(in, paramCount)) {
            error = "truncated parameter table";
            return false;
        }
        function.params.resize(paramCount);
        for (ParamInfo& param : function.params) {
            std::uint8_t aggregate = 0;
            if (!readU32(in, param.offset) || !readU32(in, param.size) ||
                !readI32(in, param.memType) || !readU8(in, aggregate)) {
                error = "truncated parameter table";
                return false;
            }
            param.aggregate = aggregate != 0;
        }

        std::uint32_t constantCount = 0;
        if (!readU32(in, constantCount)) {
            error = "truncated constant pool";
            return false;
        }
        function.constants.resize(constantCount);
        for (Value& value : function.constants) {
            if (!readValue(in, value)) {
                error = "truncated constant pool";
                return false;
            }
        }

        std::uint32_t codeCount = 0;
        if (!readU32(in, codeCount)) {
            error = "truncated code section";
            return false;
        }
        function.code.resize(codeCount);
        function.lines.resize(codeCount);
        for (std::size_t i = 0; i < function.code.size(); ++i) {
            Instruction& inst = function.code[i];
            std::uint8_t op = 0;
            if (!readU8(in, op) || !readI32(in, inst.a) || !readI32(in, inst.b) ||
                !readU32(in, function.lines[i].line) || !readU16(in, function.lines[i].file)) {
                error = "truncated code section";
                return false;
            }
            inst.op = static_cast<OpCode>(op);
        }
    }

    if (!readI32(in, module.entryPoint) || !readI32(in, module.initFunction)) {
        error = "truncated module header";
        return false;
    }

    if (!readU32(in, count)) {
        error = "truncated process table";
        return false;
    }
    module.processes.resize(count);
    for (int& index : module.processes) {
        if (!readI32(in, index)) {
            error = "truncated process table";
            return false;
        }
    }

    if (!readU32(in, count)) {
        error = "truncated event table";
        return false;
    }
    module.events.resize(count);
    for (int& index : module.events) {
        if (!readI32(in, index)) {
            error = "truncated event table";
            return false;
        }
    }

    if (!readU32(in, count)) {
        error = "truncated event table";
        return false;
    }
    module.eventConditions.resize(count);
    for (int& index : module.eventConditions) {
        if (!readI32(in, index)) {
            error = "truncated event table";
            return false;
        }
    }

    if (module.entryPoint < 0) {
        error = "'" + path + "' does not define 'main'";
        return false;
    }
    return true;
}

} // namespace acvm

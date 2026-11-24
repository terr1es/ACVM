//===--- Disassembler.cpp ------------------------------------------------===//
#include "acvm/IR/Disassembler.h"

#include "acvm/IR/Bytecode.h"

#include "acvm/Sema/Builtin.h"

#include <iomanip>
#include <ostream>
#include <string>

namespace acvm {
namespace {

std::string constantText(const Function& function, std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= function.constants.size()) {
        return "<bad constant>";
    }
    const Value& value = function.constants[static_cast<std::size_t>(index)];
    if (value.tag() == Value::Tag::Float) {
        return formatFloat(value.asFloat());
    }
    return formatInt128(value.asInt());
}

std::string memTypeText(const Module& module, std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= module.memTypes.size()) {
        return "?";
    }
    const MemType& type = module.memTypes[static_cast<std::size_t>(index)];
    switch (type.kind) {
    case MemType::Kind::Float:
        return "f" + std::to_string(type.bits);
    case MemType::Kind::Ptr:
        return "ptr";
    case MemType::Kind::Bool:
        return "bool";
    case MemType::Kind::Char:
        return "char";
    case MemType::Kind::Int:
        break;
    }
    return (type.isUnsigned ? "u" : "i") + std::to_string(type.bits);
}

} // namespace

void formatInstruction(const Module& module, const Function& function, std::size_t index,
                       std::ostream& out) {
    if (index >= function.code.size()) {
        return;
    }
    const Instruction& inst = function.code[index];
    // The mnemonic belongs to this function, not to `dumpFunction`: both the
    // listing and the debugger show it in the same column, and a formatter that
    // omitted it would print bare operands.
    out << std::left << std::setw(13) << opCodeName(inst.op) << std::right;
    switch (inst.op) {
    case OpCode::ConstInt:
    case OpCode::ConstFloat:
        out << "[" << inst.a << "] = " << constantText(function, inst.a);
        break;
    case OpCode::ConstStr:
        if (inst.a >= 0 && static_cast<std::size_t>(inst.a) < module.strings.size()) {
        out << '"' << module.strings[static_cast<std::size_t>(inst.a)] << '"';
        } else {
        out << "<bad string>";
        }
        break;
    case OpCode::LocalAddr:
    case OpCode::MemCopy:
    case OpCode::MemZero:
        out << inst.a;
        break;
    case OpCode::CopyStep:
        out << "*frame+" << (inst.b & 0x0FFF'FFFF) << "++ = *frame+" << inst.a << "++ ("
        << (((inst.b >> 28) & 0x7) + 1) << " bytes)";
        break;
    case OpCode::DerefLocal:
        out << "frame+" << inst.a << " -> " << memTypeText(module, inst.b);
        break;
    case OpCode::StepLocal:
        out << "frame+" << inst.a << " += " << (inst.b >> 8) << ' '
        << memTypeText(module, inst.b & 0xFF);
        break;
    case OpCode::LoadLocal:
    case OpCode::StoreLocal:
        out << "frame+" << inst.a << ' ' << memTypeText(module, inst.b & 0xFF);
        if ((inst.b & 0x100) != 0) {
        out << " keep";
        }
        break;
    case OpCode::LoadGlobal:
    case OpCode::StoreGlobal:
        if (inst.a >= 0 && static_cast<std::size_t>(inst.a) < module.globals.size()) {
        out << module.globals[static_cast<std::size_t>(inst.a)].name;
        } else {
        out << "<bad global>";
        }
        out << ' ' << memTypeText(module, inst.b & 0xFF);
        if ((inst.b & 0x100) != 0) {
        out << " keep";
        }
        break;
    case OpCode::GlobalAddr:
        if (inst.a >= 0 && static_cast<std::size_t>(inst.a) < module.globals.size()) {
        out << module.globals[static_cast<std::size_t>(inst.a)].name;
        } else {
        out << "<bad global>";
        }
        break;
    case OpCode::Load:
    case OpCode::Store:
    case OpCode::StoreKeep:
    case OpCode::Add:
    case OpCode::Sub:
    case OpCode::Mul:
    case OpCode::Div:
    case OpCode::Mod:
    case OpCode::Neg:
    case OpCode::BitNot:
    case OpCode::And:
    case OpCode::Or:
    case OpCode::Xor:
    case OpCode::Shl:
    case OpCode::Shr:
    case OpCode::Eq:
    case OpCode::Ne:
    case OpCode::Lt:
    case OpCode::Le:
    case OpCode::Gt:
    case OpCode::Ge:
        out << memTypeText(module, inst.a);
        break;
    case OpCode::Cast:
        out << memTypeText(module, inst.a) << " -> " << memTypeText(module, inst.b);
        break;
    case OpCode::MemEq:
        out << inst.a << " bytes";
        break;
    case OpCode::Jump:
    case OpCode::JumpIfFalse:
    case OpCode::JumpIfTrue:
        out << "-> " << inst.a;
        break;
    case OpCode::Call:
        if (inst.a >= 0 && static_cast<std::size_t>(inst.a) < module.functions.size()) {
        out << module.functions[static_cast<std::size_t>(inst.a)].name << ", argc "
            << inst.b;
        } else {
        out << "<bad target>, argc " << inst.b;
        }
        break;
    case OpCode::Builtin:
        out << builtinName(static_cast<Builtin>(inst.a));
        if (inst.b < 0) {
        out << " string";
        } else if (inst.b > 0) {
        out << ' ' << memTypeText(module, inst.b);
        }
        break;
    case OpCode::Concat:
        // Both operands, in source order. Printing only the right one made a
        // bug where the left scalar was silently dropped look like a correct
        // `CONCAT string` in the listing.
        out << (inst.b == 0 ? std::string("string") : memTypeText(module, inst.b - 1));
        out << " + ";
        out << (inst.a < 0 ? std::string("string") : memTypeText(module, inst.a));
        break;
    case OpCode::Trap:
        out << trapKindName(static_cast<TrapKind>(inst.a));
        break;
    default:
        break;
    }
}

void dumpFunction(const Module& module, const Function& function, std::ostream& out) {
    out << "function " << function.name << " (args=" << function.params.size()
        << ", frame=" << function.frameSize << ")\n";

    if (!function.constants.empty()) {
        out << "  constants:\n";
        for (std::size_t i = 0; i < function.constants.size(); ++i) {
            out << "    [" << i << "] " << constantText(function, static_cast<std::int32_t>(i))
                << '\n';
        }
    }

    out << "  code:\n";
    for (std::size_t i = 0; i < function.code.size(); ++i) {
        out << "    " << std::setw(4) << std::setfill('0') << i << std::setfill(' ') << "  ";
        const SourceLine at = function.lineAt(i);
        if (at.isValid()) {
            std::string file = "?";
            if (at.file < module.sourceFiles.size()) {
                file = module.sourceFiles[at.file];
                const std::size_t slash = file.find_last_of("/\\");
                if (slash != std::string::npos) {
                    file = file.substr(slash + 1);
                }
            }
            out << std::left << std::setw(16) << (file + ":" + std::to_string(at.line))
                << std::right;
        } else {
            out << std::left << std::setw(16) << "-" << std::right;
        }
        // The mnemonic and its operands both come from `formatInstruction`, so the
        // listing and the debugger cannot drift apart.
        formatInstruction(module, function, i, out);
        out << '\n';
    }
}

void dumpModule(const Module& module, std::ostream& out) {
    std::size_t userFunctions = 0;
    std::size_t syntheticFunctions = 0;
    for (const Function& function : module.functions) {
        if (function.isSynthetic) {
            ++syntheticFunctions;
        } else {
            ++userFunctions;
        }
    }

    out << "module: " << userFunctions << " function(s), " << module.globals.size()
        << " global(s)";
    if (syntheticFunctions != 0) {
        out << " (+" << syntheticFunctions << " compiler-generated)";
    }
    out << '\n';

    if (!module.globals.empty()) {
        out << "globals:\n";
        for (const GlobalSlot& slot : module.globals) {
            out << "  " << slot.name << " offset=" << slot.offset << " size=" << slot.size << '\n';
        }
    }

    for (const Function& function : module.functions) {
        if (function.isSynthetic) {
            continue;
        }
        out << '\n';
        dumpFunction(module, function, out);
    }

    if (syntheticFunctions != 0) {
        out << "\ncompiler-generated code (not part of the program's source):\n";
        for (const Function& function : module.functions) {
            if (!function.isSynthetic) {
                continue;
            }
            out << '\n';
            dumpFunction(module, function, out);
        }
    }
}

} // namespace acvm

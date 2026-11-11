#include <file.h>
#include <disasm.h>
#include <fmt/format.h>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 5) {
        fmt::print(stderr, "Usage: em2-disasm image.bin image-base address count\n");
        return 2;
    }
    const auto image = LoadFile(argv[1]);
    const uint64_t base = std::strtoull(argv[2], nullptr, 0);
    const uint64_t address = std::strtoull(argv[3], nullptr, 0);
    const uint64_t count = std::strtoull(argv[4], nullptr, 0);
    if (address < base || address - base > image.size() || count > (image.size() - (address - base)) / 4) return 1;
    for (uint64_t i = 0; i < count; ++i) {
        auto pc = address + i * 4;
        ppc_insn instruction{};
        ppc::Disassemble(reinterpret_cast<const uint32_t*>(image.data() + pc - base), pc, instruction);
        fmt::print("{:08X}: {:08X}  {} {}\n", pc, instruction.instruction,
            instruction.opcode ? instruction.opcode->name : "???", instruction.op_str);
    }
}

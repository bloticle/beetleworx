#include <recompiler.h>
#include <fstream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    struct Fixture { const char* name; uint32_t word; };
    const Fixture fixtures[] = {
        {"addc", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(10u<<1)},
        {"addme", (31u<<26)|(3u<<21)|(4u<<16)|(234u<<1)},
        {"subfze", (31u<<26)|(3u<<21)|(4u<<16)|(200u<<1)},
        {"lhzu", (41u<<26)|(3u<<21)|(4u<<16)|2},
        {"lhau", (43u<<26)|(3u<<21)|(4u<<16)|2},
        {"lfsu", (49u<<26)|(3u<<21)|(4u<<16)|4},
        {"stfsu", (53u<<26)|(3u<<21)|(4u<<16)|4},
        {"lfdu", (51u<<26)|(3u<<21)|(4u<<16)|8},
        {"stfdu", (55u<<26)|(3u<<21)|(4u<<16)|8},
        {"vandc", (4u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|0x444},
        {"pack_half2", (6u<<26)|(3u<<21)|(4u<<11)|(3u<<18)|(1u<<16)|(3u<<6)|1552u},
        {"unpack_half2", (6u<<26)|(3u<<21)|(4u<<11)|(12u<<16)|2032u},
        {"unpack_half4", (6u<<26)|(3u<<21)|(4u<<11)|(20u<<16)|2032u},
        {"lvx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(103u<<1)},
        {"stvx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(231u<<1)},
        {"lvlx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(519u<<1)},
        {"lvrx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(551u<<1)},
        {"lwarx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(20u<<1)},
        {"stwcx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(150u<<1)|1u},
        {"ldarx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(84u<<1)},
        {"stdcx", (31u<<26)|(3u<<21)|(4u<<16)|(5u<<11)|(214u<<1)|1u},
        {"dcbz", (31u<<26)|(4u<<16)|(5u<<11)|(1014u<<1)},
        {"dcbzl", (31u<<26)|(1u<<21)|(4u<<16)|(5u<<11)|(1014u<<1)},
        {"mftb", 0x7C6C42E6u},
    };
    constexpr uint32_t counter_tail[] = {
        0x7CA903A6u, // mtctr r5
        0x98830000u, // stb r4,0(r3)
        0x4F400020u, // bdzlr-
        0x98830001u, // stb r4,1(r3)
        0x4F400020u, // bdzlr-
        0x98830002u, // stb r4,2(r3)
        0x4E800020u,
    };
    const size_t counter_offset = sizeof(fixtures)/sizeof(fixtures[0])*8;
    constexpr size_t jump_size = 44;
    const size_t jump_offset = counter_offset + sizeof(counter_tail);
    Recompiler compiler;
    compiler.image.base = 0x82000000;
    compiler.image.size = jump_offset + jump_size;
    compiler.image.data = std::make_unique<uint8_t[]>(compiler.image.size);
    compiler.image.Map(".text", 0, compiler.image.size, SectionFlags_Code, compiler.image.data.get());
    for (size_t i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]); ++i) {
        auto* code = reinterpret_cast<uint32_t*>(compiler.image.data.get() + i*8);
        code[0] = ByteSwap(fixtures[i].word);
        code[1] = ByteSwap(0x4E800020u);
        compiler.image.symbols.emplace(fmt::format("fixture_{}", fixtures[i].name), compiler.image.base+i*8, 8, Symbol_Function);
    }
    compiler.out = "#include \"ppc_recomp_shared.h\"\n";
    for (size_t i = 0; i < std::size(counter_tail); ++i)
        reinterpret_cast<uint32_t*>(compiler.image.data.get() + counter_offset)[i] = ByteSwap(counter_tail[i]);
    compiler.image.symbols.emplace("fixture_counter_tail", compiler.image.base+counter_offset, sizeof(counter_tail), Symbol_Function);
    compiler.config.setJmpAddress = 0x82002000;
    compiler.config.longJmpAddress = 0x82002010;
    auto branch = [&](uint32_t from, uint32_t to) { return 0x48000001u | ((to-from)&0x03FFFFFCu); };
    uint32_t jump_code[] = {
        0x38801234u, 0x38600200u,
        branch(compiler.image.base+jump_offset+8, compiler.config.setJmpAddress),
        0x2C030000u, 0x40820018u,
        0x3BE00063u, 0x3821FFF0u, 0x38800007u, 0x38600200u,
        branch(compiler.image.base+jump_offset+36, compiler.config.longJmpAddress),
        0x4E800020u,
    };
    for (size_t i=0; i<std::size(jump_code); ++i)
        reinterpret_cast<uint32_t*>(compiler.image.data.get()+jump_offset)[i]=ByteSwap(jump_code[i]);
    compiler.image.symbols.emplace("fixture_jump",compiler.image.base+jump_offset,jump_size,Symbol_Function);
    for (size_t i = 0; i < sizeof(fixtures)/sizeof(fixtures[0]); ++i)
        if (!compiler.Recompile(Function{compiler.image.base+i*8,8})) return 1;
    auto counter_function = Function::Analyze(compiler.image.data.get()+counter_offset, sizeof(counter_tail), compiler.image.base+counter_offset);
    if (counter_function.size != sizeof(counter_tail) || !compiler.Recompile(counter_function)) return 1;
    if (!compiler.Recompile(Function{compiler.image.base+jump_offset,jump_size})) return 1;
    std::ofstream stream(argv[1]);
    stream << compiler.out;
    return stream ? 0 : 1;
}

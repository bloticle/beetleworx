// Additional native PPC lowering needed by EM2. Included inside the instruction switch.
case PPC_INST_BDZF:
case PPC_INST_BDNZT:
{
    static const char* bits[] = {"lt", "gt", "eq", "so"};
    const bool zero = insn.opcode->id == PPC_INST_BDZF;
    println("\t--{}.u64;", ctr());
    println("\tif ({}.u32 {} 0 && {}{}.{}) {{", ctr(), zero ? "==" : "!=", zero ? "!" : "", cr(insn.operands[0] / 4), bits[insn.operands[0] % 4]);
    if (insn.operands[1] >= fn.base && insn.operands[1] < fn.base + fn.size)
        println("\t\tgoto loc_{:X};", insn.operands[1]);
    else {
        printFunctionCall(insn.operands[1]);
        println("\t\treturn;");
    }
    println("\t}}");
    break;
}
case PPC_INST_EQV:
    println("\t{}.u64 = ~({}.u64 ^ {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
    if (strchr(insn.opcode->name, '.')) println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
    break;
case PPC_INST_ADDC:
    println("\t{}.u64 = uint64_t({}.u32) + {}.u32;", temp(), r(insn.operands[1]), r(insn.operands[2]));
    println("\t{}.ca = {}.u64 >> 32;", xer(), temp());
    println("\t{}.u64 = {}.u64 + {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
    if (strchr(insn.opcode->name, '.')) println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
    break;
case PPC_INST_ADDME:
case PPC_INST_SUBFZE:
{
    const bool add = insn.opcode->id == PPC_INST_ADDME;
    println("\t{}.u64 = uint64_t({}{}.u32) + {}.ca{};", temp(), add ? "" : "~", r(insn.operands[1]), xer(), add ? " + 0xFFFFFFFFull" : "");
    println("\t{}.u64 = {}{}.u64 + {}.ca{};", r(insn.operands[0]), add ? "" : "~", r(insn.operands[1]), xer(), add ? " - 1" : "");
    println("\t{}.ca = {}.u64 >> 32;", xer(), temp());
    if (strchr(insn.opcode->name, '.')) println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
    break;
}
case PPC_INST_LHZU:
case PPC_INST_LHAU:
case PPC_INST_LFSU:
case PPC_INST_LFDU:
case PPC_INST_LBZUX:
case PPC_INST_LHZUX:
case PPC_INST_LWZUX:
case PPC_INST_LWAUX:
case PPC_INST_LDUX:
case PPC_INST_LFSUX:
case PPC_INST_LFDUX:
case PPC_INST_STHU:
case PPC_INST_STFSU:
case PPC_INST_STFDU:
case PPC_INST_STBUX:
case PPC_INST_STHUX:
case PPC_INST_STDUX:
case PPC_INST_STFSUX:
{
    const std::string name = insn.opcode->name;
    const bool indexed = name.back() == 'x';
    const bool store = name[0] == 's';
    const bool floating = name.find('f') != std::string::npos;
    const bool single = name.find("fs") != std::string::npos;
    const bool sign = name.find("ha") != std::string::npos || name.find("wa") != std::string::npos;
    const int width = name.find('b') != std::string::npos ? 8 : name.find('h') != std::string::npos ? 16 :
        name.find('w') != std::string::npos || single ? 32 : 64;
    const auto ra = insn.operands[indexed ? 1 : 2];
    if (floating) printSetFlushMode(false);
    if (indexed) println("\t{} = {}.u32 + {}.u32;", ea(), r(ra), r(insn.operands[2]));
    else println("\t{} = {}.u32 + {};", ea(), r(ra), int32_t(insn.operands[1]));
    const auto dest = floating ? f(insn.operands[0]) : r(insn.operands[0]);
    if (store) {
        if (single) println("\t{}.f32 = float({}.f64);", temp(), dest);
        println("\tPPC_STORE_U{}({}, {}.u{});", width, ea(), single ? temp() : dest, width);
    } else if (single) {
        println("\t{}.u32 = PPC_LOAD_U32({});", temp(), ea());
        println("\t{}.f64 = double({}.f32);", dest, temp());
    } else if (sign) {
        println("\t{}.s64 = int{}_t(PPC_LOAD_U{}({}));", dest, width, width, ea());
    } else {
        println("\t{}.u64 = PPC_LOAD_U{}({});", dest, width, ea());
    }
    println("\t{}.u64 = {};", r(ra), ea());
    break;
}
case PPC_INST_LHBRX:
    print("\t{}.u64 = __builtin_bswap16(PPC_LOAD_U16(", r(insn.operands[0]));
    if (insn.operands[1]) print("{}.u32 + ", r(insn.operands[1]));
    println("{}.u32));", r(insn.operands[2]));
    break;
case PPC_INST_LVEHX:
    print("\t{} = (", ea());
    if (insn.operands[1]) print("{}.u32 + ", r(insn.operands[1]));
    println("{}.u32) & ~1u;", r(insn.operands[2]));
    println("\t{}.u16[7 - (({} & 15) >> 1)] = PPC_LOAD_U16({});", v(insn.operands[0]), ea(), ea());
    break;
case PPC_INST_FRSQRTE:
    printSetFlushMode(false);
    println("\t{}.f64 = 1.0 / sqrt({}.f64);", f(insn.operands[0]), f(insn.operands[1]));
    break;
case PPC_INST_DCBST:
    println("\t// Guest cache writeback: coherent host memory.");
    break;
case PPC_INST_VNOR:
case PPC_INST_VNOR128:
    println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_xor_si128(simde_mm_or_si128(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)), simde_mm_set1_epi32(-1)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
    break;
case PPC_INST_VCMPGTUW:
    for (int i = 0; i < 4; ++i)
        println("\t{}.u32[{}] = {}.u32[{}] > {}.u32[{}] ? 0xFFFFFFFFu : 0;", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
    if (strchr(insn.opcode->name, '.')) println("\t{}.setFromMask(simde_mm_load_ps({}.f32), 0xF);", cr(6), v(insn.operands[0]));
    break;
case PPC_INST_VCTUXS:
case PPC_INST_VCFPUXWS128:
    printSetFlushMode(true);
    println("\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_vctuxs(simde_mm_mul_ps(simde_mm_load_ps({}.f32), simde_mm_set1_ps({}.0f))));", v(insn.operands[0]), v(insn.operands[1]), uint64_t(1) << insn.operands[2]);
    break;
case PPC_INST_VSRO:
case PPC_INST_VSRO128:
case PPC_INST_VSLO128:
    println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_{}(simde_mm_load_si128((simde__m128i*){}.u8), simde_mm_load_si128((simde__m128i*){}.u8)));", v(insn.operands[0]), insn.opcode->id == PPC_INST_VSLO128 ? "vslo" : "vsro", v(insn.operands[1]), v(insn.operands[2]));
    break;
case PPC_INST_VSLH:
case PPC_INST_VSRAH:
    for (int i = 0; i < 8; ++i)
        println("\t{}.{}16[{}] = {}.{}16[{}] {} ({}.u16[{}] & 15);", v(insn.operands[0]), insn.opcode->id == PPC_INST_VSRAH ? "s" : "u", i, v(insn.operands[1]), insn.opcode->id == PPC_INST_VSRAH ? "s" : "u", i, insn.opcode->id == PPC_INST_VSRAH ? ">>" : "<<", v(insn.operands[2]), i);
    break;
case PPC_INST_VSPLTISH:
    println("\tsimde_mm_store_si128((simde__m128i*){}.u16, simde_mm_set1_epi16(int16_t({})));", v(insn.operands[0]), int32_t(insn.operands[1]));
    break;
case PPC_INST_VPKSWSS128:
    println("\tsimde_mm_store_si128((simde__m128i*){}.s16, simde_mm_packs_epi32(simde_mm_load_si128((simde__m128i*){}.s32), simde_mm_load_si128((simde__m128i*){}.s32)));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
    break;
case PPC_INST_VSUBUWM:
    println("\tsimde_mm_store_si128((simde__m128i*){}.u32, simde_mm_sub_epi32(simde_mm_load_si128((simde__m128i*){}.u32), simde_mm_load_si128((simde__m128i*){}.u32)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
    break;
case PPC_INST_VSUBUWS:
    for (int i = 0; i < 4; ++i) {
        println("\tctx.vscr_sat |= {}.u32[{}] < {}.u32[{}];", v(insn.operands[1]), i, v(insn.operands[2]), i);
        println("\t{}.u32[{}] = {}.u32[{}] < {}.u32[{}] ? 0 : {}.u32[{}] - {}.u32[{}];", v(insn.operands[0]), i, v(insn.operands[1]), i, v(insn.operands[2]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
    }
    break;
case PPC_INST_VADDSWS:
case PPC_INST_VSUBSHS:
{
    const bool add = insn.opcode->id == PPC_INST_VADDSWS;
    const int width = add ? 32 : 16;
    const int64_t low = -(int64_t(1) << (width - 1));
    const int64_t high = (int64_t(1) << (width - 1)) - 1;
    for (int i = 0; i < 128 / width; ++i) {
        println("\t{}.s64 = int64_t({}.s{}[{}]) {} {}.s{}[{}];", temp(), v(insn.operands[1]), width, i, add ? "+" : "-", v(insn.operands[2]), width, i);
        println("\tctx.vscr_sat |= {}.s64 < {}ll || {}.s64 > {}ll;", temp(), low, temp(), high);
        println("\t{}.s{}[{}] = int{}_t({}.s64 < {}ll ? {}ll : {}.s64 > {}ll ? {}ll : {}.s64);", v(insn.operands[0]), width, i, width, temp(), low, low, temp(), high, high, temp());
    }
    break;
}

"""Localize registers in profiled integer video kernels.

Keep original arithmetic, branches, guest accesses and save/restore helpers.
Leaf caches publish before restore/return. Symbol-loop prefixes publish before
returning to original control flow; guest calls stay outside the cached prefix.
Original translations remain linked for comparisons.
"""
import argparse
import hashlib
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('sources', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
result = '#include "ppc_recomp_shared.h"\n#include "video_blocks.h"\n'
for kernel, (address, saved, digest) in enumerate([
    ('83343AC8', 21, '5ce76a9fd79f682ec41befdbc6c10d056b8872b1c86625b06ce23a228f75cf08'),
    ('833441A8', 19, 'ac79eea6662d6ebe3717768e7260b76dd2bb233929156ee820f666b89caa678f'),
    ('83344528', 14, '01cc05fd3e2e5064d3400cc55af07dda619bb89211adb7883c9e0d3da526697d'),
]):
    source = (a.sources / 'ppc_recomp.545.cpp').read_text(encoding='utf-8')
    matches = re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_' + address + r'\) \{\n.*?^}', source, re.M | re.S)
    if len(matches) != 1 or hashlib.sha256(matches[0].encode()).hexdigest() != digest:
        raise SystemExit(f'Review changed video block {address} before updating its hash')
    body = matches[0]
    begin = body.index(f'\t__savegprlr_{saved}(ctx, base);') + len(f'\t__savegprlr_{saved}(ctx, base);')
    core = body[begin:-1]
    fields = sorted(set(re.findall(r'\bctx\.(\w+)', core)))
    if any(not re.fullmatch(r'r\d+|cr6|xer|ctr|lr|v63', field) for field in fields):
        raise SystemExit('Unexpected cached register field')
    restore = f'__restgprlr_{saved}'
    calls = re.findall(r'^\t(\w+)\(ctx, base\);$', core, re.M)
    expected = [restore]
    if calls != expected or core.count('return;') != expected.count(restore) or 'PPC_CALL_INDIRECT_FUNC' in core:
        raise SystemExit('Review changed guest call/return boundaries')
    if core.count(f'{restore}(ctx, base);\n\treturn;') != expected.count(restore):
        raise SystemExit('Restore must be immediately followed by return')
    prefix = body[:begin].replace(f'PPC_FUNC_IMPL(__imp__sub_{address}) {{',
        f'PPC_FUNC_IMPL(sub_{address}) {{\n\tif (!em2::NativeVideoBlocksEnabled()) {{ __imp__sub_{address}(ctx, base); return; }}'
        f'\n\tif (em2::VideoBlockVerificationEnabled() && em2::VerifyVideoBlock(ctx, base, __imp__sub_{address}, sub_{address}, {kernel})) return;')
    declarations = ''.join(f'\n\tauto local_{field} = ctx.{field};' for field in fields)
    # Protect call statements from field substitution, then insert synchronization.
    for index, call in enumerate(calls):
        core = core.replace(f'\t{call}(ctx, base);', f'__EM2_CALL_{index}__', 1)
    if re.search(r'\b\w+\(ctx\s*[,)]', core):
        raise SystemExit('Unrecognized context-taking call')
    localized = re.sub(r'\bctx\.(\w+)', lambda m: 'local_' + m[1], core)
    flush = ''.join(f'\tctx.{field} = local_{field};\n' for field in fields)
    reload = ''.join(f'\tlocal_{field} = ctx.{field};\n' for field in fields)
    for index, call in enumerate(calls):
        localized = localized.replace(f'__EM2_CALL_{index}__', flush + f'\t{call}(ctx, base);\n' + (reload if call != restore else ''))
    result += f'PPC_EXTERN_FUNC(__imp__sub_{address});\n' + prefix + declarations + localized + '}\n'

# Keep the symbol decoder's short paths and guest fill call in original code.
# Localize only long loop prefixes, retaining the final original iteration.
source = (a.sources / 'ppc_recomp.544.cpp').read_text(encoding='utf-8')
body = re.search(r'^PPC_FUNC_IMPL\(__imp__sub_8333C6C0\) \{\n.*?^}', source, re.M | re.S)[0]
if hashlib.sha256(body.encode()).hexdigest() != 'ee22aa8cd3844349c3770bfafc36f4ddd216137e67995b6447e36ed758fa4cc9':
    raise SystemExit('Review changed symbol decoder before updating hash')
begin = body.index('loc_8333C7B0:\n')
end = body.index('\t// cmpwi cr6,r23,-22', begin)
loop = body[begin:end]
if loop.count('goto loc_8333C938;') != 1 or loop.count('if (ctx.cr6.gt) goto loc_8333C7B0;') != 1:
    raise SystemExit('Review changed symbol loop exits')
if re.search(r'\b\w+\(ctx\s*[,)]|PPC_CALL_INDIRECT_FUNC|\breturn\b', loop):
    raise SystemExit('Symbol loop prefix must contain no guest calls or returns')
fields = sorted(set(re.findall(r'\bctx\.(\w+)', loop)))
if any(not re.fullmatch(r'r\d+|cr6|xer', field) for field in fields):
    raise SystemExit('Unexpected symbol prefix field')
declarations = ''.join(f'\tauto local_{field} = ctx.{field};\n' for field in fields)
flush = ''.join(f'\tctx.{field} = local_{field};\n' for field in fields)
loop = loop.replace('if (ctx.cr6.gt) goto loc_8333C7B0;', 'if (ctx.r23.s32 > 1) goto loc_8333C7B0;')
loop = re.sub(r'\bctx\.(\w+)', lambda m: 'local_' + m[1], loop)
loop = loop.replace('goto loc_8333C938;', '{\n' + flush + '\treturn true;\n\t}')
result += 'static __declspec(noinline) bool SymbolLoopPrefix(PPCContext& ctx,uint8_t* base) {\n\tPPC_FUNC_PROLOGUE();\n\tuint32_t ea{};\n' + declarations + loop + flush + '\treturn false;\n}\n'
body = body.replace('PPC_FUNC_IMPL(__imp__sub_8333C6C0) {',
    'PPC_FUNC_IMPL(sub_8333C6C0) {\n\tif (em2::VideoBlockVerificationEnabled() && em2::NativeVideoBlocksEnabled() && em2::VerifyVideoBlock(ctx, base, __imp__sub_8333C6C0, sub_8333C6C0, 3)) return;')
body = body.replace('loc_8333C7B0:\n', 'loc_8333C7B0:\n\tif (ctx.r23.s32 > 16 && em2::NativeVideoBlocksEnabled() && SymbolLoopPrefix(ctx, base)) goto loc_8333C938;\n')
result += 'PPC_EXTERN_FUNC(__imp__sub_8333C6C0);\n' + body + '\n'

a.output.parent.mkdir(parents=True, exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding='utf-8') != result:
    a.output.write_text(result, encoding='utf-8')

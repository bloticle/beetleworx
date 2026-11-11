"""Retain the translated memcpy's exact exits, replacing only copy-loop prefixes.

The checked source hash makes changes to the original routine fail closed until
its loops and register effects are reviewed again. The installed SDK and original
translation are untouched; both paths link into the same game executable.
"""
import argparse
import hashlib
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("sources", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
matches = []
for source in args.sources.glob("ppc_recomp.*.cpp"):
    content = source.read_text(encoding="utf-8")
    if "PPC_FUNC_IMPL(__imp__sub_82B6A550)" in content:
        matches.extend(re.findall(r"^PPC_FUNC_IMPL\(__imp__sub_82B6A550\) \{\n.*?^\}", content, re.M | re.S))
if len(matches) != 1:
    raise SystemExit("Expected one original 82B6A550 implementation")
body = matches[0]
if hashlib.sha256(body.encode()).hexdigest() != "30d7aa4a5e24392655f4a0f383effb9df0bae16a9c4d04e802213b1b37bdcf75":
    raise SystemExit("Original memcpy changed; review copy-loop register effects before updating its hash")
body = body.replace("PPC_FUNC_IMPL(__imp__sub_82B6A550) {", "PPC_FUNC_IMPL(sub_82B6A550) {\n\tif (!em2::NativeMemoryCopyEnabled()) { __imp__sub_82B6A550(ctx, base); return; }")
for label, step, register in [("82B6A5F4", 8, 7), ("82B6A690", 8, 7), ("82B6A6EC", 128, 6), ("82B6A7C0", 4, 7)]:
    anchor = f"\tctx.ctr.u64 = ctx.r{register}.u64;\nloc_{label}:"
    if body.count(anchor) != 1:
        raise SystemExit(f"Copy-loop anchor {label} changed")
    minimum = max(2, (24 + step - 1) // step + 1)
    body = body.replace(anchor, f"\tctx.ctr.u64 = ctx.r{register}.u64;\n\tif (ctx.ctr.u64 >= {minimum}) em2::SkipNativeCopyPrefix(ctx, base, {step});\nloc_{label}:")
output = '#include "ppc_recomp_shared.h"\n#include "memory_copy.h"\nPPC_EXTERN_FUNC(__imp__sub_82B6A550);\n' + body + "\n"
args.output.parent.mkdir(parents=True, exist_ok=True)
if not args.output.exists() or args.output.read_text(encoding="utf-8") != output:
    args.output.write_text(output, encoding="utf-8")

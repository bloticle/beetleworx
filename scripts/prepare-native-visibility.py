"""Keep the original scene-scan callbacks/exits, batching only invisible flags."""
import argparse
import hashlib
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("sources", type=Path)
p.add_argument("output", type=Path)
a = p.parse_args()
matches = []
for path in a.sources.glob("ppc_recomp.*.cpp"):
    text = path.read_text(encoding="utf-8")
    if "PPC_FUNC_IMPL(__imp__sub_8256F948)" in text:
        matches += re.findall(r"^PPC_FUNC_IMPL\(__imp__sub_8256F948\) \{\n.*?^\}", text, re.M | re.S)
if len(matches) != 1:
    raise SystemExit("Expected one original scene-scan routine")
body = matches[0]
if hashlib.sha256(body.encode()).hexdigest() != "c561d29d6130ac24a569c16efb2bc21b36b394b38fa6cddba7751d66b291d442":
    raise SystemExit("Original scene scan changed; review callback order and loop register effects")
body = body.replace("PPC_FUNC_IMPL(__imp__sub_8256F948) {", "PPC_FUNC_IMPL(sub_8256F948) {\n\tif (!em2::NativeVisibilityScanEnabled()) { __imp__sub_8256F948(ctx, base); return; }")
anchor = "loc_8256FA2C:"
if body.count(anchor) != 1:
    raise SystemExit("Scene-scan loop anchor changed")
start, end = body.index(anchor), body.index("loc_8256FAF4:")
scalar = body[start:end]
# Loop versioning avoids a probe/guard on every subsequent dense entry. The
# copy has exactly the original instructions and only renames internal labels.
scalar = re.sub(r"loc_([0-9A-F]{8})", r"loc_\1_SCALAR", scalar)
body = body[:end] + "\tgoto loc_8256FAF4;\n" + scalar + body[end:]
body = body.replace(anchor, anchor + "\n\tif (!(ctx.r24.u32 & 15u)) {\n\t\tconst int batch = em2::SkipNativeInvisibleBatch(ctx, base);\n\t\tif (batch < 0) goto loc_8256FA2C_SCALAR;\n\t\tif (batch > 0) {\n\t\t\tif (!ctx.r24.s32) goto loc_8256FAF4;\n\t\t\tgoto loc_8256FA2C;\n\t\t}\n\t}",1)
out = '#include "ppc_recomp_shared.h"\n#include "visibility_scan.h"\nPPC_EXTERN_FUNC(__imp__sub_8256F948);\n' + body + "\n"
a.output.parent.mkdir(parents=True, exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding="utf-8") != out:
    a.output.write_text(out, encoding="utf-8")

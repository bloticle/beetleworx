"""Preserve native shader packets/exits; bypass redundant VMX copy prefixes."""
import argparse
import hashlib
from pathlib import Path
import re

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("sources", type=Path)
p.add_argument("output", type=Path)
a = p.parse_args()
specs = {
    "826BF668": ("31140bca246213c6db2bc5b118e684841c13e6c741c13a4310e14789704ec997", "826BF770", "r28", "SkipShaderUploadPrefix"),
    "826BF2D0": ("1534ca7436e540b96e31fe842943744006ecd422febcd81816746e1c428b7c04", "826BF368", "r11", "SkipScalarShaderUploadPrefix"),
    "826BF510": ("0434c255a61e2f699e53c6a36c257a1fb7164530d7889031e8eff21232a53dd4", "826BF614", "r26", "SkipFetchShaderUploadPrefix"),
}
matches = {name: [] for name in specs}
for path in a.sources.glob("ppc_recomp.*.cpp"):
    text = path.read_text(encoding="utf-8")
    for name in specs:
        if f"PPC_FUNC_IMPL(__imp__sub_{name})" in text:
            matches[name] += re.findall(rf"^PPC_FUNC_IMPL\(__imp__sub_{name}\) \{{\n.*?^\}}", text, re.M | re.S)
out = '#include "ppc_recomp_shared.h"\n#include "shader_upload.h"\n'
for name, (digest, label, count, helper) in specs.items():
    if len(matches[name]) != 1:
        raise SystemExit(f"Expected one original shader upload {name}")
    body = matches[name][0]
    if hashlib.sha256(body.encode()).hexdigest() != digest:
        raise SystemExit(f"Original shader upload {name} changed; review copy order and register effects")
    body = body.replace(f"PPC_FUNC_IMPL(__imp__sub_{name}) {{", f"PPC_FUNC_IMPL(sub_{name}) {{")
    if name == "826BF510":
        body = body.replace("\tPPC_FUNC_PROLOGUE();", "\tif (!em2::FetchUploadHasLongRun(ctx.r4.u64)) { __imp__sub_826BF510(ctx, base); return; }\n\tPPC_FUNC_PROLOGUE();", 1)
    anchor = f"loc_{label}:"
    if body.count(anchor) != 1:
        raise SystemExit(f"Shader upload loop anchor {label} changed")
    body = body.replace(anchor, f"\tif (ctx.{count}.u32 >= 8 && em2::NativeShaderUploadEnabled()) em2::{helper}(ctx, base);\n" + anchor)
    out += f"PPC_EXTERN_FUNC(__imp__sub_{name});\n" + body + "\n"
a.output.parent.mkdir(parents=True, exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding="utf-8") != out:
    a.output.write_text(out, encoding="utf-8")

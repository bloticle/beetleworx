"""Reject incomplete translations even when the upstream CLI exits successfully."""
import collections
import json
import re
import struct
import tomllib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
log = (root / "analysis/recompile.log").read_text()
problems = [line for line in log.splitlines() if any(term in line for term in (
    "Unrecognized instruction", "Unable to decode", "trying to jump outside", "Unexpected", "ERROR:"))]
functions = collections.Counter()
sources = list((root / "generated/ppc").glob("ppc_recomp.*.cpp"))
switch_sources=[]
for path in sources:
    text = path.read_text()
    functions.update(re.findall(r"PPC_FUNC_IMPL\((\w+)\)", text))
    if "__builtin_debugtrap" in text or "// ERROR" in text:
        problems.append(f"Unimplemented translation in {path.name}")
    for match in re.finditer(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-Fa-f]+)\)",text):
        address=int(match[1],16)
        if 0x828B8000<=address<0x828B8684 or 0x82B58000<=address<0x82B59620:
            end=text.find("PPC_FUNC_IMPL(",match.end())
            switch_sources.append((address,text[match.start():end if end>=0 else len(text)]))
problems.extend(f"Duplicate definition: {name}" for name, count in functions.items() if count != 1)
switch_regressions=[]
# These dispatches used to call null host functions for legitimate local cases.
# Verify original table words and guards, configuration, and actual emitted
# local branches together; a table declaration alone does not prove recovery.
image=(root/"private/image.bin").read_bytes()
image_base=json.loads((root/"analysis/image.json").read_text())["base"]
configured={s["base"]:s for s in tomllib.loads((root/"config/em2_switch_tables.toml").read_text()).get("switch",[])}
for head,register,count,guard,guard_word,branch,branch_word in (
    (0x828B8670,11,16,0x828B8654,0x2B0B000F,0x828B866C,0x419902E0),
    (0x82B5960C,25,12,0x82B595C0,0x2B19000B,0x82B59608,0x41990088),
):
    labels=list(struct.unpack_from(f">{count}I",image,head+24-image_base))
    record=configured.get(head)
    owner=max((s for s in switch_sources if s[0]<=head),key=lambda s:s[0],default=(0,""))[1]
    valid=(struct.unpack_from(">I",image,guard-image_base)[0]==guard_word and
        struct.unpack_from(">I",image,branch-image_base)[0]==branch_word and
        record is not None and record["r"]==register and record["labels"]==labels and
        all(re.search(rf"case 0x{target:08X}:\s*goto loc_{target:08X};",owner) and f"loc_{target:08X}:" in owner for target in labels))
    switch_regressions.append({"base":f"0x{head:08X}","register":register,"entries":count,"local_targets":len(set(labels)),"original_guard_table_and_emitted_branches_equal":bool(valid)})
    if not valid:problems.append(f"Level-transition local switch regression at {head:08X}")
summary = {"implementations": len(functions), "translation_units": len(sources),
           "source_bytes": sum(path.stat().st_size for path in sources), "switch_regressions":switch_regressions,"problems": problems}
(root / "analysis/translation-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
if not sources or problems:
    raise SystemExit("Translation audit failed")

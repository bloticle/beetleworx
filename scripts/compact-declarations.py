"""Keep only the PPC declarations used by each generated translation unit.

The stock shared header declares all 140k functions in every file. This pass
changes declarations only; translated instruction bodies stay intact.
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
count = 0
for path in (root / "generated/ppc").glob("ppc_recomp.*.cpp"):
    source = path.read_text()
    marker = '#include "ppc_recomp_shared.h"'
    if marker not in source:
        continue
    calls = set(re.findall(r"\b([A-Za-z_]\w*)\(ctx,\s*base\);", source))
    # Forward declarations preserve C linkage for the public weak wrappers.
    calls.update(re.findall(r"PPC_WEAK_FUNC\((\w+)\)", source))
    declarations = '#include "ppc_context.h"\n' + '\n'.join(f'PPC_EXTERN_FUNC({name});' for name in sorted(calls))
    path.write_text(source.replace(marker, declarations, 1))
    count += 1
print(f"Compacted declarations for {count} translation units")

"""Extract one generated function into a compiler/debugger reproducer."""
import sys
from pathlib import Path
root = Path(__file__).resolve().parents[1]
for path in (root / "generated/ppc").glob("ppc_recomp.*.cpp"):
    source = path.read_text()
    needle = f"PPC_FUNC_IMPL(__imp__{sys.argv[1]})"
    start = source.find(needle)
    if start >= 0:
        end = source.find("\n}\n", start) + 3
        output = '#include "ppc_recomp_shared.h"\n' + source[start:end]
        Path(sys.argv[2]).write_text(output)
        print(output)
        break
else:
    raise SystemExit("Function not found")

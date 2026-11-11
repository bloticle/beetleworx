"""Keep the native cast's callback/stack effects; localize its pure RTTI walk."""
import argparse
import hashlib
from pathlib import Path
import re

p = argparse.ArgumentParser()
p.add_argument('sources', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
matches = []
for path in a.sources.glob('ppc_recomp.*.cpp'):
    matches += re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_82551560\) \{\n.*?^\}', path.read_text(encoding='utf-8'), re.M | re.S)
if len(matches) != 1 or hashlib.sha256(matches[0].encode()).hexdigest() != 'dc8da8ad40f33bba225330db7e6a5792e2de90eeaffa68e13ac74567079f1f0c':
    raise SystemExit('Native type check changed; review getter, stack, flags and return paths')
body = matches[0]
begin = body.index('\t// cmplwi cr6,r3,0\n')
end = body.index('loc_825515A0:')
body = body[:begin] + '''
    // The loop has no guest calls. Every exit leaves CR6 equal (either the
    // class comparison matched, or the null comparison did). Avoid publishing
    // intermediate class pointers and comparison bits into PPCContext.
    {
        uint32_t type = ctx.r3.u32;
        const uint32_t wanted = ctx.r31.u32;
        while (type && type != wanted) type = PPC_LOAD_U32(type + 4);
        ctx.cr6.compare<uint32_t>(0, 0, ctx.xer);
        if (type) goto loc_825515B8;
    }
''' + body[end:]
body = body.replace('PPC_FUNC_IMPL(__imp__sub_82551560)', 'PPC_FUNC(EM2LocalTypeCheck)', 1)
out = '#include "ppc_recomp_shared.h"\n' + body + '\n'
a.output.parent.mkdir(parents=True, exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding='utf-8') != out:
    a.output.write_text(out, encoding='utf-8')

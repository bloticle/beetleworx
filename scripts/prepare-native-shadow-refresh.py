"""Keep native shadow updates, bypassing the unchanged-selection return after resize."""
import argparse, hashlib, re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sources',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
matches=[]
for path in a.sources.glob('ppc_recomp.*.cpp'):
    matches+=re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_829BA908\) \{\n.*?^\}',path.read_text(encoding='utf-8'),re.M|re.S)
if len(matches)!=1 or hashlib.sha256(matches[0].encode()).hexdigest()!='a1145f78f8537842bf52b815925fdb6e2943ed4af2765d8f0da71f0104c54f4d':
    raise SystemExit('Native shadow preparation changed; review owned-view loop and stationary skip')
body=matches[0]
anchor='if (!ctx.cr6.eq) goto loc_829BA93C;'
if body.count(anchor)!=1:raise SystemExit('Native unchanged-selection shadow return changed')
body=body.replace(anchor,'if (!ctx.cr6.eq && !em2::ShadowOwnerNeedsRefresh(base,ctx.r23.u32)) goto loc_829BA93C;')
body=body.replace('PPC_FUNC_IMPL(__imp__sub_829BA908)','PPC_FUNC(EM2RefreshShadowViews)',1)
out='#include "ppc_recomp_shared.h"\n#include "shadow_resize.h"\nPPC_EXTERN_FUNC(EM2RefreshShadowViews);\n'+body+'\n'
a.output.parent.mkdir(parents=True,exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding='utf-8')!=out:a.output.write_text(out,encoding='utf-8')

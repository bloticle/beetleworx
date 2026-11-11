"""Specialize material binding for host integer bit instructions; retain native body."""
import argparse,hashlib,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sources',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
matches=[]
for path in a.sources.glob('ppc_recomp.*.cpp'):
    matches+=re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_82C8A498\) \{\n.*?^\}',path.read_text(encoding='utf-8'),re.M|re.S)
if len(matches)!=1 or hashlib.sha256(matches[0].encode()).hexdigest()!='a76f5a5324c5279b29695d717318d9990cb786e21dd3214caeae3ec7629e57a7':
    raise SystemExit('Original material binding changed; review integer, vector and callback effects')
body=matches[0]
for anchor,helper,target in [('82C8A7C4','SkipEmptyMaterialVertexStage','82C8ADBC'),('82C8AF04','SkipEmptyMaterialPixelStage','82C8B4FC')]:
    marker=f'\tif (ctx.cr6.eq) goto loc_{anchor};'
    if body.count(marker)!=1:raise SystemExit(f'Material empty-pass anchor {anchor} changed')
    body=body.replace(marker,marker+f'\n\tif (em2::{helper}(ctx, base)) goto loc_{target};')
out='#include "ppc_recomp_shared.h"\n#include "material_binding.h"\n#include "guest_word_read.h"\n'
for name,features,barrier in [('EM2MaterialBitOps','bmi,bmi2,lzcnt',None),('EM2MaterialEmpty','',None),
    ('EM2MaterialWordReads','',False),('EM2MaterialBitOpsWordReads','bmi,bmi2,lzcnt',False),
    ('EM2MaterialBarrierReads','',True),('EM2MaterialBitOpsBarrierReads','bmi,bmi2,lzcnt',True),
    ('EM2MaterialLikelyReads','','likely'),('EM2MaterialBitOpsLikelyReads','bmi,bmi2,lzcnt','likely')]:
    attribute=f'__attribute__((target("{features}"),noinline))' if features else '__attribute__((noinline))'
    variant=body
    if barrier is not None:
        helper='em2::LikelyGuestWord' if barrier=='likely' else f'em2::ExplicitGuestWord<{str(barrier).lower()}>'
        variant,count=re.subn(r'PPC_LOAD_U32\(([^()\n]+)\)',lambda m:f'{helper}({m[1]},base)',variant)
        if count!=body.count('PPC_LOAD_U32(') or count!=155:raise SystemExit('Material word-read sites changed; review access ordering')
    out+=variant.replace('PPC_FUNC_IMPL(__imp__sub_82C8A498)', f'{attribute} PPC_FUNC({name})',1)+'\n'
a.output.parent.mkdir(parents=True,exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding='utf-8')!=out:a.output.write_text(out,encoding='utf-8')

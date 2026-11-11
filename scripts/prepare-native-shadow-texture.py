"""Retain native texture creation, rejecting invalid shadow metadata allocations."""
import argparse, hashlib, re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sources',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
matches=[]
for path in a.sources.glob('ppc_recomp.*.cpp'):
    matches+=re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_8267C260\) \{\n.*?^\}',path.read_text(encoding='utf-8'),re.M|re.S)
if len(matches)!=1 or hashlib.sha256(matches[0].encode()).hexdigest()!='8a51d198267efd0c521866c75107c6e59a8c5ea3ac27e5b4452e127e8fe63f1d':
    raise SystemExit('Native texture creation changed; review allocation, ownership and failure epilogue')
body=matches[0]
patches={
    'ctx.lr = 0x8267C2FC;\n\tsub_826550C8(ctx, base);':
    'ctx.lr = 0x8267C2FC;\n\tsub_826550C8(ctx, base);\n'
    '\tif (!em2::ValidateShadowMetadataAllocation(ctx.r3.u32,40)) {ctx.r3.u64=0;goto loc_8267C454;}',
    'ctx.lr = 0x8267C384;\n\tsub_826548D0(ctx, base);':
    'ctx.lr = 0x8267C384;\n\tsub_826548D0(ctx, base);\n'
    '\tif (!em2::ValidateShadowMetadataAllocation(ctx.r3.u32,52)) {\n'
    '\t\tctx.r3.u64=ctx.r31.u32;ctx.r4.u64=1;__imp__sub_8267CC20(ctx,base);\n'
    '\t\tctx.r3.u64=0;goto loc_8267C454;\n\t}',
}
for anchor,replacement in patches.items():
    if body.count(anchor)!=1:raise SystemExit('Native allocation anchor changed')
    body=body.replace(anchor,replacement)
body=body.replace('PPC_FUNC_IMPL(__imp__sub_8267C260)','PPC_FUNC(EM2CheckedShadowTexture)',1)
fixture=body.replace('EM2CheckedShadowTexture','EM2ShadowTextureAllocationFixture',1)
anchor='if (ctx.cr6.eq) goto loc_8267C2E0;'
if fixture.count(anchor)!=1:raise SystemExit('Optional format validation guard changed')
fixture=fixture.replace(anchor,'goto loc_8267C2EC; // Diagnostic: begin after device format validation.',1)
out='#include "ppc_recomp_shared.h"\n#include "shadow_resources.h"\nPPC_EXTERN_FUNC(__imp__sub_8267CC20);\nPPC_EXTERN_FUNC(EM2CheckedShadowTexture);\nPPC_EXTERN_FUNC(EM2ShadowTextureAllocationFixture);\n'+body+'\n'+fixture+'\n'
out+='#include "native_texture_retirement.h"\n'
for address,name,digest,caller in [
    ('8267CC20','EM2RetireTextureData','6f6d48b8ae85c0f3d69740115b42e872ea4c26071d4b03fadc85d8dd5489f3a5','8267CC74'),
    ('8267BDB8','EM2RetireTextureDataBase','06688dc906eab9e8a6b758ce4dcefa130f4c9164e3d3f200f050fe3ed7b743fc','8267BE04'),
]:
    found=[]
    for path in a.sources.glob('ppc_recomp.*.cpp'):
        found+=re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_'+address+r'\) \{\n.*?^\}',path.read_text(encoding='utf-8'),re.M|re.S)
    if len(found)!=1 or hashlib.sha256(found[0].encode()).hexdigest()!=digest:
        raise SystemExit('Native texture destructor changed; review header/resource ownership and deferred cleanup')
    destructor=found[0]
    anchor='ctx.lr = 0x'+caller+';\n\tsub_82654790(ctx, base);'
    if destructor.count(anchor)!=1:raise SystemExit('Native texture header free site changed')
    destructor=destructor.replace(anchor,'ctx.lr = 0x'+caller+';\n\tif (!em2::DeferQueuedTextureHeaderFree(ctx,base)) sub_82654790(ctx, base);')
    destructor=destructor.replace('PPC_FUNC_IMPL(__imp__sub_'+address+')','PPC_FUNC('+name+')',1)
    out+='PPC_EXTERN_FUNC('+name+');\n'+destructor+'\n'
a.output.parent.mkdir(parents=True,exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding='utf-8')!=out:a.output.write_text(out,encoding='utf-8')

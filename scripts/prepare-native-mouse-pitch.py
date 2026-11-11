"""Retain native pitch bodies with scoped elapsed-time input thresholds."""
import argparse, hashlib, re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sources',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
matches=[]
for path in a.sources.glob('ppc_recomp.*.cpp'):
    matches+=re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_82A517D0\) \{\n.*?^\}',path.read_text(encoding='utf-8'),re.M|re.S)
if len(matches)!=1 or hashlib.sha256(matches[0].encode()).hexdigest()!='d74aa4b0c692544d9ae6a8caee41e8304165b740962453a54eec724567dfe0cc':
    raise SystemExit('Native pitch body changed; review manual input threshold and camera constraints')
original=matches[0]
anchor='\t// fcmpu cr6,f12,f13\n\tctx.cr6.compare(ctx.f12.f64, ctx.f13.f64);\n\t// ble cr6,0x82a519cc'
strong='\t// fcmpu cr6,f13,f0\n\tctx.cr6.compare(ctx.f13.f64, ctx.f0.f64);\n\t// bgt cr6,0x82a51c30'
velocity='\t// fcmpu cr6,f13,f0\n\tctx.cr6.compare(ctx.f13.f64, ctx.f0.f64);\n\t// bge cr6,0x82a51c3c'
for label,site in [('manual',anchor),('strong input',strong),('velocity',velocity)]:
    if original.count(site)!=1:raise SystemExit(f'Native {label} pitch comparison changed')
def variant(name,mouse):
    body=original.replace(anchor,anchor.replace('ctx.f13.f64);','0.0);' if mouse else 'em2::NativePitchMotionThreshold(ctx.f13.f64));'))
    body=body.replace(strong,strong.replace('ctx.f0.f64);','em2::NativePitchMotionThreshold(ctx.f0.f64));'))
    body=body.replace(velocity,velocity.replace('ctx.f0.f64);','em2::NativePitchMotionThreshold(ctx.f0.f64,true));'))
    return body.replace('PPC_FUNC_IMPL(__imp__sub_82A517D0)',f'PPC_FUNC({name})',1)
out='#include "ppc_recomp_shared.h"\n#include "camera_timing.h"\nPPC_EXTERN_FUNC(EM2MousePitch);\nPPC_EXTERN_FUNC(EM2TimedPitch);\n'
out+=variant('EM2MousePitch',True)+'\n'+variant('EM2TimedPitch',False)+'\n'
a.output.parent.mkdir(parents=True,exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding='utf-8')!=out:a.output.write_text(out,encoding='utf-8')

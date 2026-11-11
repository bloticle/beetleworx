"""Coalesce matrix output stores; retain native arithmetic/register results."""
import argparse
import hashlib
from pathlib import Path
import re

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sources',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
matches=[]
for path in a.sources.glob('ppc_recomp.*.cpp'):
    source=path.read_text(encoding='utf-8')
    matches+=re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_82B9D350\) \{\n.*?^\}',source,re.M|re.S)
if len(matches)!=1:
    raise SystemExit('Expected one original matrix multiply 82B9D350')
body=matches[0]
# Exact original reviewed for load-before-store ordering and four aligned rows.
EXPECTED_SHA='61769b5c4819a13731905c9f60d2bc5e6b8b619e67c636633d2be12ea8bcfd4c'
if hashlib.sha256(body.encode()).hexdigest()!=EXPECTED_SHA:
    raise SystemExit('Original matrix multiply changed; review its complete register and memory effects')
body=body.replace('PPC_FUNC_IMPL(__imp__sub_82B9D350)', 'static __declspec(noinline) PPC_FUNC(MatrixStoreBody)',1)
# These operand orders come from the unchanged original LLVM-23 object.
# LLVM may commute commutative intrinsics after unrelated store changes,
# which changes NaN payloads. Keep the original instruction-level choices.
arithmetic={'multiply':0,'add':0}
load=r'simde_mm_load_ps\(ctx\.v(\d+)\.f32\)'
def madd(match):
    destination,a,b,acc=match.groups()
    if destination in {'4','13','7','0'}:a,b=b,a
    product=f'em2::MatrixOrderedMultiply(simde_mm_load_ps(ctx.v{a}.f32), simde_mm_load_ps(ctx.v{b}.f32))'
    accumulator=f'simde_mm_load_ps(ctx.v{acc}.f32)'
    terms=(product,accumulator) if destination=='12' else (accumulator,product)
    arithmetic['multiply']+=1;arithmetic['add']+=1
    return f'simde_mm_store_ps(ctx.v{destination}.f32, em2::MatrixOrderedAdd({terms[0]}, {terms[1]}));'
body=re.sub(r'simde_mm_store_ps\(ctx\.v(\d+)\.f32, simde_mm_add_ps\(simde_mm_mul_ps\('+load+', '+load+r'\), '+load+r'\)\);',madd,body)
def multiply(match):
    destination,a,b=match.groups();arithmetic['multiply']+=1
    return f'simde_mm_store_ps(ctx.v{destination}.f32, em2::MatrixOrderedMultiply(simde_mm_load_ps(ctx.v{a}.f32), simde_mm_load_ps(ctx.v{b}.f32)));'
body=re.sub(r'simde_mm_store_ps\(ctx\.v(\d+)\.f32, simde_mm_mul_ps\('+load+', '+load+r'\)\);',multiply,body)
if arithmetic!={'multiply':16,'add':12}:
    raise SystemExit(f'Matrix arithmetic sequence changed: {arithmetic}')
seen={}
def store(match):
    vector=match.group(1)
    seen[vector]=seen.get(vector,0)+1
    if seen[vector]!=1:
        return '\t// Remaining row words were written together above.'
    return '\t// Four native big-endian word stores, with the same row bytes.\n'+match.group(0).split('\n')[0]+'\n'+ \
        '\tem2::StoreNativeMatrixRow(ea, simde_mm_load_si128((simde__m128i*)ctx.v'+vector+'.u8), base);'
body=re.sub(r'\tea = [^\n]+;\n\tPPC_STORE_U32\(ea, ctx\.v(6[0-3])\.u32\[3 - \(\(ea & 0xF\) >> 2\)\]\);',store,body)
if seen!={'63':4,'62':4,'61':4,'60':4}:
    raise SystemExit(f'Matrix output store groups changed: {seen}')
out='#include "ppc_recomp_shared.h"\n#include "matrix_math.h"\nPPC_EXTERN_FUNC(__imp__sub_82B9D350);\n'+body+'''\n
PPC_FUNC_IMPL(sub_82B9D350) {
    if (em2::NativeMatrixStoresEnabled() && em2::NativeMatrixStoresSupported(ctx.r3.u32)) MatrixStoreBody(ctx, base);
    else __imp__sub_82B9D350(ctx, base);
}
'''
a.output.parent.mkdir(parents=True,exist_ok=True)
if not a.output.exists() or a.output.read_text(encoding='utf-8')!=out:
    a.output.write_text(out,encoding='utf-8')

"""Read-only directory search across original EM1 packfiles; no runtime fallback."""
import argparse
import ctypes as c
import json
from pathlib import Path
from em1_rvz import RVZ
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--library',type=Path,required=True);p.add_argument('--image',type=Path,required=True);p.add_argument('fragment');p.add_argument('--output',type=Path)
a=p.parse_args();d=c.CDLL(str(a.library.resolve()));d.find_original_resources.argtypes=[c.c_wchar_p,c.c_char_p,c.POINTER(c.c_char_p),c.POINTER(c.c_uint32)];d.find_original_resources.restype=c.c_char_p
r=RVZ(a.image)
try:archives=[name for name in r.files(1) if name.lower().startswith('packfiles/') and name.lower().endswith('.pak')]
finally:r.close()
matches,skipped=c.c_char_p(),c.c_uint32();error=d.find_original_resources(str(a.image),a.fragment.lower().encode(),c.byref(matches),c.byref(skipped));assert not error,error
assert skipped.value==0,'Incomplete source inventory; missing resources cannot be inferred'
rows=[dict(archive=line.split('\t')[0],resource=line.split('\t')[1]) for line in matches.value.decode().splitlines()]
result=dict(scope='Original PAK directory inventory only; no asset substitution, arbitrary runtime archive fallback or readiness claim',fragment=a.fragment.lower(),packfile_directories=len(archives),skipped=skipped.value,matches=rows)
if a.output:a.output.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))

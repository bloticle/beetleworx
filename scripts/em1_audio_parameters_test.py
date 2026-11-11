"""Native audio scalar/RNG conversion against original-DOL reference fixtures."""
import argparse,ctypes as c,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--reference',type=Path,required=True);args=p.parse_args()
d=c.CDLL(str(Path(args.library).resolve()));U=c.POINTER(c.c_uint32);I=c.POINTER(c.c_int32)
d.fixed_parameters.argtypes=[U,I];d.fixed_parameters.restype=c.c_char_p
d.pitch_ratio.argtypes=[c.c_uint32,U];d.pitch_ratio.restype=c.c_char_p
d.random_parameters.argtypes=[U,c.c_size_t,U,c.POINTER(c.c_uint64),U];d.random_parameters.restype=c.c_char_p
fixed=json.loads((args.reference/'audio_parameters.json').read_text());random=json.loads((args.reference/'audio_random_parameters.json').read_text())
for case in fixed['parameters']:
 values=(c.c_uint32*4)(*case['input_bits']);out=(c.c_int32*4)();error=d.fixed_parameters(values,out);assert not error,error
 assert list(out)==case['native'],(case,list(out))
for case in fixed['pitch']:
 out=c.c_uint32();error=d.pitch_ratio(case['input_bits'],c.byref(out));assert not error,error
 assert out.value==case['ratio_bits'],(case,out.value)
for case in random['cases']:
 flat=[value for node in case['range_bits'] for pair in node for value in pair];values=(c.c_uint32*len(flat))(*flat)
 seed=c.c_uint32(case['seed']);draws=c.c_uint64();out=(c.c_uint32*4)()
 error=d.random_parameters(values,len(case['range_bits']),c.byref(seed),c.byref(draws),out);assert not error,error
 assert list(out)==case['output_bits'] and seed.value==case['seed_after'] and draws.value==case['draws'],(case,list(out),seed.value,draws.value)
for invalid in [0x7f800000,0xff800000,0x7fc00000]:
 out=c.c_uint32();assert d.pitch_ratio(invalid,c.byref(out))
 values=(c.c_uint32*4)(invalid,0,0,0);integers=(c.c_int32*4)();assert d.fixed_parameters(values,integers)
result=dict(static_cases=len(fixed['parameters']),pitch_cases=len(fixed['pitch']),random_cases=len(random['cases']),scope='Exact native-reference scalar conversion and per-node RNG draw ordering. Parent traversal, RTPC/state/bus execution and voice admission remain separate.')
Path('analysis/em1-compatibility/audio-parameters-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS:',result)

"""Validate mixing and zero-gain SDL output without launching the game."""
import argparse,ctypes,json
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--library',required=True);p.add_argument('--image',required=True);args=p.parse_args()
dll=ctypes.CDLL(str(Path(args.library).resolve()));dll.run.argtypes=[ctypes.c_wchar_p];dll.run.restype=ctypes.c_char_p
result=dll.run(args.image).decode();print(result);assert result.startswith('PASS:'),result
Path('analysis/em1-compatibility/audio-output-result.json').write_text(json.dumps(dict(result=result,backend='First compiled SDL driver; zero-gain hardware playback',scope='Original source sound, offline PCM gain/resampling, voice admission/handles and actual output callbacks. No authored node execution or in-game event routing.'),indent=2)+'\n')

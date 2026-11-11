"""Compare complete, bounded native CRT traces from an owned paired run."""
import argparse, collections, difflib, json, re
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('pair_root',type=Path);args=parser.parse_args()
event=re.compile(r'Native online CRT event: tick (\d+), index (\d+), seed (true|false), caller ([0-9A-F]{8}), state ([0-9A-F]{8}), result ([0-9A-F]{8})')
header=re.compile(r'Native gameplay observation v4:[^\n]*?epoch \d+, tick (\d+),[^\n]*?CRT calls/seeds (\d+)/(\d+)')
owner=re.compile(r'Native online CRT owner: tick (\d+), index (\d+), r29 ([0-9A-F]{8}) head ([0-9A-F]{8}), r30 ([0-9A-F]{8}) head ([0-9A-F]{8}), r31 ([0-9A-F]{8}) head ([0-9A-F]{8})')
stack=re.compile(r'Native online CRT stack: tick (\d+), index (\d+), returns ((?:[0-9A-F]{8} ?){6})')
schedule=re.compile(r'Native online CRT schedule: tick (\d+), index (\d+), scene ([0-9A-F]{8}), last ([0-9A-F]{8}), interval ([0-9A-F]{8}), budget ([0-9A-F]{8}), gate ([0-9A-F]{8})')
target=re.compile(r'Native online CRT target: tick (\d+), index (\d+), key ([0-9A-F]{8}), entity ([0-9A-F]{8}), handle ([0-9A-F]{8})')
peers={};summaries={}

for role in ('host','client'):
    text=(args.pair_root/role/'startup.log').read_text(errors='replace')
    ticks=collections.defaultdict(list)
    for m in event.finditer(text):
        ticks[int(m[1])].append({'index':int(m[2]),'seed':m[3]=='true','caller':m[4],'state':m[5],'result':m[6]})
    counts={int(m[1]):int(m[2])+int(m[3]) for m in header.finditer(text)}
    details=collections.defaultdict(dict)
    for m in owner.finditer(text):
        details[int(m[1])][int(m[2])]={'r29':m[3],'head29':m[4],'r30':m[5],'head30':m[6],'r31':m[7],'head31':m[8]}
    for m in stack.finditer(text):details[int(m[1])].setdefault(int(m[2]),{})['parents']=m[3].split()
    for m in schedule.finditer(text):details[int(m[1])].setdefault(int(m[2]),{})['schedule']=dict(zip(['scene','last','interval','budget','gate'],m.groups()[2:]))
    for m in target.finditer(text):details[int(m[1])].setdefault(int(m[2]),{})['target']=dict(zip(['key','entity','handle'],m.groups()[2:]))
    peers[role]={'ticks':dict(ticks),'owners':dict(details),'expected_counts':counts,'truncated':'Native online CRT trace reached its' in text}
for tick in sorted(set(peers['host']['ticks'])|set(peers['client']['ticks'])):
    a=peers['host']['ticks'].get(tick,[]);b=peers['client']['ticks'].get(tick,[])
    complete=all(not peers[r]['truncated'] and len(rows)==peers[r]['expected_counts'].get(tick,-1) and all(row['index']==i for i,row in enumerate(rows)) for r,rows in [('host',a),('client',b)])
    key=lambda e:(e['seed'],e['caller'],e['state'],e['result'])
    changes=[]
    for tag,i,j,k,l in difflib.SequenceMatcher(a=list(map(key,a)),b=list(map(key,b)),autojunk=False).get_opcodes():
        if tag!='equal':changes.append({'operation':tag,'host_range':[i,j],'client_range':[k,l],'host':a[i:j],'client':b[k:l]})
    summaries[tick]={'complete':complete,'host_count':len(a),'client_count':len(b),'identical':complete and a==b,'changes':changes}
result={'schema':1,'root':str(args.pair_root.resolve()),'scope':'Main native tick CRT events captured by the existing verifier; no worker or full-world determinism claim. Indices are checked for capture completeness.','peers':peers,'comparison':summaries}
output=args.pair_root/'native-random-traces.json';output.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'report':str(output),'ticks':{t:{k:v for k,v in s.items() if k!='changes'} for t,s in summaries.items()}},indent=2))

"""Reject native-linked vertex fetches that overwrite their later index input."""
import argparse
import json
from pathlib import Path
import re


def audit(log):
    groups = {}
    for kind, index, words in re.findall(
            r'(masked|dual) linked VS instruction (\d+): ([A-F0-9 ]+)', log.read_text()):
        groups.setdefault(kind, {})[int(index)] = [int(w, 16) for w in words.split()]
    if set(groups) != {'masked', 'dual'}:
        raise ValueError('Both masked and dual-UV linked vertex programs must be captured')
    result = []
    for kind, code in groups.items():
        cf = code[0][0]
        start, count, sequence = cf & 4095, (cf >> 12) & 7, (cf >> 16) & 4095
        writes, fetches, hazards = {}, [], []
        for i in range(count):
            index = start + i
            if ((sequence >> (2*i)) & 3) != 1:
                raise ValueError('Unaudited fetch clause scheduling')
            a,b,c = code[index]
            if a & 31 or (b >> 30) & 1:
                raise ValueError('Expected full vertex fetch')
            source, lane, dest = (a >> 5) & 63, a >> 30, (a >> 12) & 63
            changed = [j for j in range(4) if ((b >> (3*j)) & 7) != 7]
            if (source, lane) in writes:
                hazards.append({'read_instruction':index,'source_register':source,
                                'source_lane':lane,'overwritten_by':writes[(source,lane)]})
            fetches.append({'instruction':index,'index_register':source,'index_lane':lane,
                            'destination':dest,'written_lanes':changed})
            for j in changed:
                writes[(dest,j)] = index
        result.append({'kind':kind,'fetches':fetches,'hazards':hazards})
    return {'log':str(log),'programs':result,
            'scope':'Checks the captured full-fetch clause, not an arbitrary shader control-flow graph.'}


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=audit(a.log);a.output.write_text(json.dumps(r,indent=2)+'\n')
    count=sum(len(v['hazards']) for v in r['programs'])
    print(f"{len(r['programs'])} linked fetch clauses; {count} index-register overwrite hazards")
    raise SystemExit(bool(count))

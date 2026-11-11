"""Read-only independent inventory of EM1 authored level entry dependencies."""
import argparse, hashlib, importlib.util, json, tempfile
from pathlib import Path
from em1_rvz import RVZ

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("image",type=Path)
parser.add_argument("output",type=Path)
args=parser.parse_args()
spec=importlib.util.spec_from_file_location("pak",Path(__file__).with_name("game-pak.py"))
pak=importlib.util.module_from_spec(spec);spec.loader.exec_module(pak)
rvz=RVZ(args.image)
try:
    matches=[]
    for partition in range(len(rvz.parts)):
        files={name.lower():span for name,span in rvz.files(partition).items()}
        if "packfiles/levels.pak" in files:matches.append((partition,files))
    assert len(matches)==1
    partition,files=matches[0]
    raw=rvz.read_partition(partition,*files["packfiles/levels.pak"])
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/"levels.pak";path.write_bytes(raw)
        entries={name.replace(chr(92),"/").lower():data for name,data,_ in pak.read_pak(path)}
    levels=[]
    for path,data in sorted(entries.items()):
        if not path.startswith("levels/") or not path.endswith(".level"):continue
        fields={};issues=[]
        for line in data.decode("latin1").splitlines():
            line=line.strip()
            if not line or line.startswith("//"):continue
            if "=" not in line:issues.append("Malformed field: "+line);continue
            key,value=line.split("=",1);key=key.strip().lower();value=value.strip()
            if key in fields:issues.append("Duplicate field: "+key)
            fields[key]=value
        scenes=[fields["shared"]] if fields.get("shared") else []
        parts=[]
        for key in ("initial","interior"):
            if not fields.get(key):continue
            name=fields[key].lower().removesuffix(".part")
            part="levels/"+fields.get("folder","").lower()+"/"+name+".part"
            parts.append(part)
            if part not in entries:issues.append("Missing partition: "+part);continue
            section="scenes"
            for line in entries[part].decode("latin1").splitlines():
                line=line.strip()
                if not line:continue
                if line.startswith("//"):
                    tag=line[2:].strip().lower()
                    if tag=="soundbanks list" or tag.startswith("audio:"):section="audio"
                    elif tag.startswith("gsas:"):section="scenes"
                    elif tag.startswith(("interior:","destination:")):section="none"
                    continue
                if section=="scenes":scenes.append(line)
        requests=[]
        for scene in dict.fromkeys(scenes):
            gsa=scene.split(" -",1)[0].strip().replace(chr(92),"/").lower()
            archive="packfiles/"+gsa.rsplit("/",1)[-1].removesuffix(".gsa")+".pak"
            present=archive in files
            if not present:issues.append("Missing archive: "+archive)
            requests.append(dict(gsa=gsa,archive=archive,present=present))
        levels.append(dict(id=path[7:],fields=fields,entry_partitions=parts,entry_scenes=requests,issues=issues))
    result=dict(levels=levels,count=len(levels),source_sha256=hashlib.sha256(raw).hexdigest(),
        complete_entry_archive_sets=sum(not row["issues"] for row in levels),
        scope="Descriptor and source archive presence only; does not prove scene adaptation or native gameplay activation.")
    args.output.write_text(json.dumps(result,indent=2)+"\n")
    print(f"{result['count']} level descriptors; {result['complete_entry_archive_sets']} have complete entry archive sets")
finally:rvz.close()

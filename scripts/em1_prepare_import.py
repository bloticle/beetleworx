"""Prepare isolated EM1 scene compatibility data from an original Wii RVZ.

This is a development staging tool, not a playable-content installer. Physics,
mesh/physics assets and engine component compatibility remain incomplete.
Requires Python3.14 (bundled compression.zstd). Never overwrites a destination.
"""
import argparse,hashlib,importlib.util,json,tempfile
from pathlib import Path,PurePosixPath
from em1_rvz import RVZ
from em1_scene_upgrade import upgrade

def prepare(image,output,archive="packfiles/dbc_1st_zonec.pak",textures=False):
    if output.exists():raise FileExistsError("Choose a new isolated destination")
    rvz=RVZ(image)
    try:
        matches=[]
        for partition in range(len(rvz.parts)):
            files=rvz.files(partition)
            if archive in files:matches.append((partition,files[archive]))
        if len(matches)!=1:raise ValueError("Archive must occur in exactly one partition")
        partition,span=matches[0];source=rvz.read_partition(partition,*span)
        disc_id=rvz.disc_id
    finally:rvz.close()
    spec=importlib.util.spec_from_file_location("game_pak",Path(__file__).with_name("game-pak.py"))
    pak=importlib.util.module_from_spec(spec);spec.loader.exec_module(pak)
    with tempfile.TemporaryDirectory(prefix="em1-import-") as scratch:
        snapshot=Path(scratch)/"source.pak";snapshot.write_bytes(source)
        entries=pak.read_pak(snapshot)
    prepared=[];reports=[];unconverted={};texture_files=[];texture_reports=[];seen=set()
    for name,data,kind in entries:
        path=PurePosixPath(name.replace("\\","/"))
        if path.is_absolute() or ".." in path.parts or ":" in str(path):raise ValueError("Unsafe archive path")
        if str(path).lower() in seen:raise ValueError("Duplicate archive path")
        seen.add(str(path).lower())
        if textures and data.startswith(b"Gamebryo File Format"):
            from em1_texture_upgrade import convert
            texture_data,texture_info=convert(data)
            if texture_info:
                texture_files.append((path,texture_data))
                texture_reports.append(dict(path=str(path),textures=texture_info,sha256=hashlib.sha256(texture_data).hexdigest()))
        if path.suffix.lower()==".bin":
            converted,report=upgrade(data)
            prepared.append((path,converted))
            report.update(path=str(path),source_sha256=hashlib.sha256(data).hexdigest(),output_sha256=hashlib.sha256(converted).hexdigest())
            reports.append(report)
        else:unconverted[path.suffix.lower()]=unconverted.get(path.suffix.lower(),0)+1
    manifest=dict(schema=1,engine_ready=False,source_disc_id=disc_id,source_partition=partition,
        source_archive=archive,source_archive_sha256=hashlib.sha256(source).hexdigest(),
        scene_count=len(prepared),scenes=reports,unconverted_assets=unconverted,
        texture_candidate_count=len(texture_files),texture_candidates=texture_reports,
        blockers=["Wii renderer assets require platform conversion", "Havok7 collision assets require EM2-compatible conversion", "Component and Lua behavior compatibility unverified"])
    output.mkdir(parents=True,exist_ok=False)
    for category,files in (("scenes",prepared),("texture_candidates",texture_files)):
        for path,data in files:
            dest=output/category/str(path);dest.parent.mkdir(parents=True,exist_ok=True)
            with dest.open("xb") as file:file.write(data)
    with (output/"em1_import_manifest.json").open("x") as file:json.dump(manifest,file,indent=2)
    return manifest

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image",type=Path);parser.add_argument("output",type=Path)
    parser.add_argument("--archive",default="packfiles/dbc_1st_zonec.pak")
    parser.add_argument("--textures",action="store_true",help="Also stage portable RGBA texture candidates (native loading unverified)")
    args=parser.parse_args();result=prepare(args.image,args.output,args.archive,args.textures)
    print(f"Prepared {result['scene_count']} scenes; engine_ready=false. See {args.output/'em1_import_manifest.json'}")

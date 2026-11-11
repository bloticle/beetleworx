"""Retain the pinned native file factory behind an application wrapper."""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path

SOURCE_SHA='547d347bf1c2dd1f3130174c25396d2cefe6f86c37e04e73bdd4dfbb1627384b'
SYMBOL='?OpenExisting@FileHandle@filesystem@rex@@SA?AV?$unique_ptr@VFileHandle@filesystem@rex@@U?$default_delete@VFileHandle@filesystem@rex@@@std@@@std@@AEBVpath@25@I@Z'
BRIDGE='?EM2OriginalOpenExisting@filesystem@rex@@YA?AV?$unique_ptr@VFileHandle@filesystem@rex@@U?$default_delete@VFileHandle@filesystem@rex@@@std@@@std@@AEBVpath@14@I@Z'
MEMBER='filesystem_win.cpp.obj'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    for name in ['ar','objcopy','nm']:p.add_argument('--'+name,required=True)
    a=p.parse_args();source=a.source.resolve();output=a.output.resolve()
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    if digest!=SOURCE_SHA:raise RuntimeError('Revalidate native file factory ABI for this core SDK archive')
    fingerprint=hashlib.sha256(Path(__file__).read_bytes()).hexdigest();manifest=output.with_suffix('.json')
    if output.exists() and manifest.exists():
        old=json.loads(manifest.read_text())
        if old.get('source_sha256')==digest and old.get('script_sha256')==fingerprint and old.get('output_sha256')==hashlib.sha256(output.read_bytes()).hexdigest():return
    def run(*args,cwd=None):return subprocess.check_output([str(x) for x in args],cwd=cwd,text=True)
    if run(a.ar,'t',source).splitlines().count(MEMBER)!=1:raise RuntimeError('Expected one native Windows filesystem object')
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        work=Path(temporary);run(a.ar,'x',source,MEMBER,cwd=work);obj=work/MEMBER
        if sum(line.endswith(' '+SYMBOL) for line in run(a.nm,'--defined-only',obj).splitlines())!=1:raise RuntimeError('Native file factory signature changed')
        run(a.objcopy,'--remove-section=.llvm_addrsig',obj)
        run(a.objcopy,'--redefine-sym='+SYMBOL+'='+BRIDGE,obj)
        if sum(line.endswith(' '+BRIDGE) for line in run(a.nm,'--defined-only',obj).splitlines())!=1:raise RuntimeError('Native file factory bridge missing')
        staged=work/output.name;shutil.copyfile(source,staged);run(a.ar,'r',staged,obj);shutil.copyfile(staged,output)
    manifest.write_text(json.dumps({'source_sha256':digest,'script_sha256':fingerprint,'output_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'symbol':SYMBOL,'bridge':BRIDGE},indent=2)+'\n')

if __name__=='__main__':main()

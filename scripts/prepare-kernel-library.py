"""Replace pinned kernel thread/wait objects without editing the installed SDK."""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
SOURCE_SHA='c25f4aedda3c6c7f90ea2bfcab932cd12c50f9d3cd456317f99ecca1cd18997b'
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);p.add_argument('--ar',required=True);a=p.parse_args()
 source=a.source.resolve();output=a.output.resolve();digest=hashlib.sha256(source.read_bytes()).hexdigest()
 if digest!=SOURCE_SHA:raise RuntimeError('Revalidate source-built native kernel thread/wait ABI for this SDK archive')
 fingerprint=hashlib.sha256(Path(__file__).read_bytes()).hexdigest();manifest=output.with_suffix('.json')
 if output.exists() and manifest.exists():
  old=json.loads(manifest.read_text())
  if old.get('source_sha256')==digest and old.get('script_sha256')==fingerprint and old.get('output_sha256')==hashlib.sha256(output.read_bytes()).hexdigest():return
 members=subprocess.check_output([a.ar,'t',str(source)],text=True).splitlines()
 removed=['content_manager.cpp.obj','xthread.cpp.obj','xobject.cpp.obj']
 if any(members.count(name)!=1 for name in removed):raise RuntimeError('Expected exactly one replaced kernel object of each kind')
 output.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
  staged=Path(temporary)/output.name;shutil.copyfile(source,staged);subprocess.run([a.ar,'d',str(staged),*removed],check=True);shutil.copyfile(staged,output)
 manifest.write_text(json.dumps({'source_sha256':digest,'script_sha256':fingerprint,'output_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'removed':removed},indent=2)+'\n')
if __name__=='__main__':main()

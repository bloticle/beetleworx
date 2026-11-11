"""Build the guarded native online autosave-notice UI change; original archive is read-only."""
import argparse, hashlib, json, runpy, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent

def run(*args):
    subprocess.run([str(x) for x in args],check=True,stdout=subprocess.DEVNULL,timeout=120)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir',type=Path,default=Path('D:/Modding/Builds/EM2X360'))
    args=parser.parse_args()
    original=args.game_dir/'packfiles/UIStartGame.pak'
    snippet=ROOT/'assets/pc-menu/OnlineSaveNotice.as'
    destination=ROOT/'private/pc-assets/packfiles/UIStartGame.pak'
    manifest=ROOT/'private/pc-assets/online-startup-manifest.json'
    fresh_profile=ROOT/'assets/pc-menu/OnlineFreshProfile.as'
    fingerprint=hashlib.sha256(Path(__file__).read_bytes()+(ROOT/'scripts/game-pak.py').read_bytes()+snippet.read_bytes()+fresh_profile.read_bytes()+(ROOT/'assets/pc-menu/OnlineSaveNoticeTrace.as').read_bytes()+original.read_bytes()).hexdigest()
    if manifest.exists() and destination.exists():
        prior=json.loads(manifest.read_text())
        if prior.get('fingerprint')==fingerprint and prior.get('sha256')==hashlib.sha256(destination.read_bytes()).hexdigest():
            print('Native online startup menu unchanged');return
    pak=runpy.run_path(str(ROOT/'scripts/game-pak.py'))
    entries=pak['read_pak'](original)
    member='ui/flash/startgame/startgame.gfx'
    work=ROOT/'private/online-startup-menu-build';work.mkdir(parents=True,exist_ok=True)
    source=work/'original.gfx';source.write_bytes(next(blob for name,blob,_ in entries if name.lower()==member))
    jar=ROOT/'analysis/ffdec/ffdec.jar'
    run('java','-jar',jar,'-export','script',work/'export',source)
    relative=Path('__Packages/em2/mainmenu/startgame/StartGame.as')
    text=(work/'export/scripts'/relative).read_text(encoding='utf-8-sig')
    start=text.index('   function showSavingMessage()')
    end=text.index('   function showAutoSavingMessage()',start)
    body=text[start:end]
    anchor='      this._spacer.adjust();'
    if body.count(anchor)!=1 or 'this._showingSavingMessage = true;' not in body or 'this.disable();' not in body:
        raise RuntimeError('Authored autosave information screen changed; refusing patch')
    text=text[:start]+body.replace(anchor,anchor+'\n'+snippet.read_text())+text[end:]
    signature='   function _startOnlineFlow(savegame)\n   {'
    if text.count(signature)!=1:raise RuntimeError('Unexpected authored fresh-profile web flow')
    text=text.replace(signature,signature+'\n'+fresh_profile.read_text())
    for signature,stage in [('   function enable()\n   {\n      super.enable();',1),('   function exitStartGame()\n   {',2),('   function _selectSlot(e)\n   {',3)]:
        if text.count(signature)!=1:raise RuntimeError('Unexpected native notice stage: '+signature)
        text=text.replace(signature,signature+'\n      this._em2OnlineNoticeTrace('+str(stage)+');')
    end=text.rfind('}')
    text=text[:end]+(ROOT/'assets/pc-menu/OnlineSaveNoticeTrace.as').read_text()+text[end:]
    target=work/'import'/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text,encoding='utf-8')
    modified=work/'modified.gfx'
    run('java','-jar',jar,'-importScript',source,modified,work/'import')
    run('java','-jar',jar,'-export','script',work/'roundtrip',modified)
    result=(work/'roundtrip/scripts'/relative).read_text(encoding='utf-8-sig')
    if any(result.count('_level0.SetOption(2501,'+str(choice)+')')!=1 for choice in (1,2,3)) or '_level0.IsSavedGamePresent()' not in result:
        raise RuntimeError('Guarded notice bridge did not survive ActionScript compilation')
    original_scripts=work/'export/scripts';roundtrip_scripts=work/'roundtrip/scripts'
    changed=[p.relative_to(original_scripts).as_posix() for p in original_scripts.rglob('*.as')
             if not (roundtrip_scripts/p.relative_to(original_scripts)).exists() or
             p.read_text(encoding='utf-8-sig')!=(roundtrip_scripts/p.relative_to(original_scripts)).read_text(encoding='utf-8-sig')]
    if changed!=[relative.as_posix()]:
        raise RuntimeError('Unexpected changes outside native StartGame class: '+repr(changed))
    expected=[(name,modified.read_bytes() if name.lower()==member else blob,kind) for name,blob,kind in entries]
    pak['write_pak'](expected,destination)
    manifest.write_text(json.dumps({'fingerprint':fingerprint,'sha256':hashlib.sha256(destination.read_bytes()).hexdigest(),'original_sha256':hashlib.sha256(original.read_bytes()).hexdigest(),'member':member,'roundtrip_verified':True,'changed_scripts':changed},indent=2)+'\n')
    print('Verified native online startup archive:',destination)

if __name__=='__main__':main()

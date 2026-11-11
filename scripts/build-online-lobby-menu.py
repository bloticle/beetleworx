"""Compile the authored native main-menu Online Multiplayer page into a private overlay."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import struct

ROOT=Path(__file__).resolve().parent.parent

def run(*arguments):
    subprocess.run([str(value) for value in arguments],check=True,stdout=subprocess.DEVNULL,timeout=120)

def replace_once(text,before,after):
    if text.count(before)!=1:raise RuntimeError('Native menu anchor changed: '+before[:80])
    return text.replace(before,after)

def gfx_tags(data):
    if data[:3]!=b'GFX' or len(data)<16:raise RuntimeError('Expected original uncompressed native GFx format')
    bits=data[8]>>3;offset=8+(5+4*bits+7)//8+4
    header=data[:4]+data[8:offset];tags=[]
    while offset<len(data):
        if offset+2>len(data):raise RuntimeError('Truncated GFx tag')
        value,=struct.unpack_from('<H',data,offset);offset+=2;kind=value>>6;size=value&63
        if size==63:
            if offset+4>len(data):raise RuntimeError('Truncated GFx tag length')
            size,=struct.unpack_from('<I',data,offset);offset+=4
        if offset+size>len(data):raise RuntimeError('Truncated GFx payload')
        tags.append((kind,data[offset:offset+size]));offset+=size
        if kind==0:break
    return header,tags

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir',type=Path,default=Path('D:/Modding/Builds/EM2X360'))
    args=parser.parse_args()
    original=args.game_dir/'packfiles/UIMainMenu.pak'
    snippet=ROOT/'assets/pc-menu/OnlineLobby.as'
    destination=ROOT/'private/pc-assets/packfiles/UIMainMenu.pak'
    manifest=ROOT/'private/pc-assets/online-lobby-manifest.json'
    fingerprint=hashlib.sha256(Path(__file__).read_bytes()+snippet.read_bytes()+(ROOT/'scripts/game-pak.py').read_bytes()+original.read_bytes()).hexdigest()
    if destination.exists() and manifest.exists():
        old=json.loads(manifest.read_text())
        if old.get('fingerprint')==fingerprint and old.get('sha256')==hashlib.sha256(destination.read_bytes()).hexdigest():
            print('Native online lobby menu unchanged');return
    pak=runpy.run_path(str(ROOT/'scripts/game-pak.py'))
    entries=pak['read_pak'](original);member='ui/flash/mainmenu/mainmenu.gfx'
    work=ROOT/'private/online-lobby-menu-build';work.mkdir(parents=True,exist_ok=True)
    source=work/'original.gfx';source.write_bytes(next(blob for name,blob,_ in entries if name.lower()==member))
    jar=ROOT/'analysis/ffdec/ffdec.jar'
    run('java','-jar',jar,'-export','script',work/'export',source)
    relative=Path('__Packages/em2/mainmenu/TopMenu.as')
    text=(work/'export/scripts'/relative).read_text(encoding='utf-8-sig')
    start=text.index('      if(_loc3_)\n');end=text.index('   function _itemSelected()',start)
    text=text[:start]+'      if(_level0.SetOption(7203,-1)==1) { this.b_5.disable(); this.b_5._visible=false; } else { this.b_5.enable(); this.b_5._visible=true; }\n   }\n'+text[end:]
    text=replace_once(text,'   function _itemSelected()\n   {','   function _itemSelected()\n   {\n      if(this._em2Online) { this._em2OnlineSelect(); return; }')
    text=replace_once(text,'this.b_5.label = "**LEVEL SELECT";','this.b_5.label = "Online Multiplayer";')
    text=replace_once(text,'      this.dispatchEvent({type:em2.Events.LEVEL_SELECT_LOAD});\n      this.b_5._rollout();','      this._em2OnlineOpen();\n      this.b_5._rollout();')
    text=replace_once(text,'   function _goBack()\n   {','   function _goBack()\n   {\n      if(this._em2Online) { if(this._em2OnlineEditingName) { this._em2OnlineEndName(false); return; } if(this._em2OnlineHostPage) { this._em2OnlineHostPage = false; this._em2OnlineRefresh(); } else if(this._em2OnlineState != 7) this._em2OnlineClose(); return; }')
    text=replace_once(text,'      _level0.APressed = this._itemSelectedListener;','''      _level0.APressed = this._itemSelectedListener;
      this._em2OnlineStartPump = this.createEmptyMovieClip("_em2OnlineStartPump",this.getNextHighestDepth());
      this._em2OnlineStartPump.onEnterFrame = function()
      {
         if(_level0.SetOption == undefined) return;
         var owner = this._parent; this.removeMovieClip();
         if(_level0.SetOption(7203,-1) != 1 && _level0.SetOption(2705,-1) == 1 && !owner._em2OnlineOpenedOnce)
         {
            owner._em2OnlineOpenedOnce = true; owner._em2OnlineOpen();
         }
      };''')
    text=text[:text.rfind('}')]+snippet.read_text()+text[text.rfind('}'):]
    target=work/'import'/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text,encoding='utf-8')
    modified=work/'modified.gfx';run('java','-jar',jar,'-importScript',source,modified,work/'import')
    run('java','-jar',jar,'-export','script',work/'roundtrip',modified)
    result=(work/'roundtrip/scripts'/relative).read_text(encoding='utf-8-sig')
    for marker in ('Online Multiplayer','function _em2OnlineOpen()','function _em2OnlineBeginName()','_level0.SetOption(2727,2)','_level0.SetOption(2727,-1)','_level0.SetOption(2725,0)','_level0.SetOption(2700,2)','_level0.SetOption(2701,-1)', '_level0.SetOption(7203,-1) == 1', '_level0.SetOption(7203,-1) != 1', 'Auto Internet port: On', 'Copy Internet invite', '_level0.SetOption(2715,1)', 'Visibility: Invite only', '_level0.SetOption(2700,16)', '_level0.SetOption(2716,-1)'):
        if marker not in result:raise RuntimeError('Native lobby bridge lost during ActionScript compilation: '+marker)
    before=work/'export/scripts';after=work/'roundtrip/scripts'
    changed=[p.relative_to(before).as_posix() for p in before.rglob('*.as')
             if not (after/p.relative_to(before)).exists() or p.read_text(encoding='utf-8-sig')!=(after/p.relative_to(before)).read_text(encoding='utf-8-sig')]
    # FFDec's cross-class inference adds an exported "var hide" declaration to
    # StartScreen after importing TopMenu. Verify actual bytecode/assets rather
    # than trusting that unrelated decompiler-only declaration.
    old_header,old_tags=gfx_tags(source.read_bytes());new_header,new_tags=gfx_tags(modified.read_bytes())
    altered=[i for i,(a,b) in enumerate(zip(old_tags,new_tags)) if a!=b]
    if old_header!=new_header or len(old_tags)!=len(new_tags) or len(altered)!=1:
        raise RuntimeError('Expected exactly one native class bytecode tag change')
    original_tag=old_tags[altered[0]];compiled_tag=new_tags[altered[0]]
    if original_tag[0]!=59 or compiled_tag[0]!=59 or b'TopMenu\0' not in original_tag[1]:
        raise RuntimeError('The changed bytecode tag is not authored TopMenu')
    for item in changed:
        if item==relative.as_posix():continue
        p=Path(item);old=(before/p).read_text(encoding='utf-8-sig');new=(after/p).read_text(encoding='utf-8-sig')
        inference_only=(item=='__Packages/em2/mainmenu/StartScreen.as' and new.replace('   var hide;\n','')==old.replace('   var hide;\n','')) or (item=='__Packages/com/greensock/TweenLite.as' and old.replace('   var complete;\n','')==new.replace('   var complete;\n',''))
        if not inference_only:
            raise RuntimeError('Unexpected decompiler change outside TopMenu: '+item)
    pak['write_pak']([(name,modified.read_bytes() if name.lower()==member else blob,kind) for name,blob,kind in entries],destination)
    manifest.write_text(json.dumps(dict(fingerprint=fingerprint,sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),original_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),member=member,changed_scripts=[relative.as_posix()],decompiler_only_changes=[v for v in changed if v!=relative.as_posix()],changed_bytecode_tag=altered[0],all_other_tags_byte_identical=True,roundtrip_verified=True),indent=2)+'\n')
    print('Verified native Online Multiplayer archive:',destination)

if __name__=='__main__':main()

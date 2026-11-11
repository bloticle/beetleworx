"""Build the game's Scaleform options assets locally; preserve original PAKs."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import shutil
import hashlib
import json
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("game_pak", ROOT / "scripts/game-pak.py")
pak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pak)


def run(*args):
    command = [str(arg) for arg in args]
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace")
    print(result.stdout, end="", flush=True)
    # This host's HotSpot JIT has crashed in JarFile.getInputStream with an
    # illegal instruction. Retry that specific VM failure once without JIT.
    # Compile/import errors keep their ordinary nonzero exit status.
    if (result.returncode and command[0] == "java"
            and "A fatal error has been detected by the Java Runtime Environment" in result.stdout
            and "EXCEPTION_ILLEGAL_INSTRUCTION" in result.stdout):
        print("Java JIT crashed; retrying this menu build step in interpreter mode.", flush=True)
        subprocess.run([command[0], "-Xint", *command[1:]], check=True, cwd=ROOT)
    else:
        result.check_returncode()


def main():
    import runpy
    runpy.run_path(str(ROOT / 'scripts/generate-native-options.py'))['generate']()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, default=Path("D:/Modding/Builds/EM2X360"))
    parser.add_argument("--ffdec", type=Path, default=ROOT / "analysis/ffdec/ffdec.jar")
    parser.add_argument("--emr-glb", type=Path)
    args = parser.parse_args()
    if not args.ffdec.is_file():
        raise RuntimeError("JPEXS 26.4.0 is required; run scripts/bootstrap-pc-menu.ps1")
    local_glb = ROOT / "private/emr_mickey_source/mickeyEM2-rebrushedAnim.glb"
    emr_glb = args.emr_glb if args.emr_glb is not None else (local_glb if local_glb.is_file() else None)
    inputs = [ROOT / "scripts/emr_ability_clips.py", Path(__file__), ROOT / "scripts/game-pak.py", ROOT / "scripts/patch-player-behaviors.py", ROOT / "scripts/generate-native-options.py", ROOT / "assets/pc-menu/NativeOptions.as.in", ROOT / "runtime/engine_settings_catalog.h", ROOT / "assets/pc-menu/TVSettings.as", ROOT / "assets/pc-menu/PlayerControls.as",
              ROOT / "assets/pc-menu/CharacterSwitchHud.as",
              args.game_dir / "packfiles/UIOptions.pak", args.game_dir / "packfiles/UIPauseOptions.pak",
              args.game_dir / "packfiles/UIhud.pak", args.game_dir / "packfiles/_Dynamic.pak"]
    if emr_glb is not None:
        inputs.append(emr_glb)
    fingerprint = hashlib.sha256(b"".join(path.read_bytes() for path in inputs)).hexdigest()
    fingerprint = hashlib.sha256(fingerprint.encode()+(ROOT/'runtime/native_ability_events.h').read_bytes()).hexdigest()
    manifest_path = ROOT / "private/pc-assets/manifest.json"
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text())
        outputs_ok = all((ROOT / "private/pc-assets" / name).is_file() and
                         hashlib.sha256((ROOT / "private/pc-assets" / name).read_bytes()).hexdigest() == digest
                         for name, digest in manifest.get("outputs", {}).items())
        if manifest.get("fingerprint") == fingerprint and len(manifest.get("outputs", {})) == 4 and outputs_ok:
            print("In-game PC menu assets unchanged; reusing verified archives")
            return
    for archive, member in [("UIOptions.pak", "ui/flash/options/options.gfx"),
                            ("UIPauseOptions.pak", "ui/flash/pauseoptions/PauseOptions.gfx")]:
        files = pak.read_pak(args.game_dir / "packfiles" / archive)
        work = ROOT / "private/pc-menu-build" / Path(archive).stem
        work.mkdir(parents=True, exist_ok=True)
        source = work / "original.gfx"
        source.write_bytes(next(blob for name, blob, kind in files if name == member))
        exported = work / "export"
        run("java", "-jar", args.ffdec, "-export", "script", exported, source)
        options_path = exported / "scripts/__Packages/em2/options/Options.as"
        options = options_path.read_text(encoding="utf-8-sig")
        anchor = '_level0.Trace("Selected ==> " + this.tvsettings._list.activeItem._name);'
        if options.count(anchor) != 1:
            raise RuntimeError("Unexpected Options.as structure; refusing an unverified patch")
        options = options.replace(anchor, "if(this.tvsettings.selectActive()) return undefined;\n         " + anchor)
        back_anchor = 'function _goBack()\n   {'
        if options.count(back_anchor) != 1:
            raise RuntimeError('Native options Back handler changed')
        options = options.replace(back_anchor, back_anchor + '\n      if(this._displayState == this.TV_SETTINGS && this.tvsettings.back()) return undefined;\n      if(this._displayState == this.PLAYER_CONTROLS && this.playerControls.back()) return undefined;')
        options = options.replace('this.b_tvsettings.label = "UI_TVSettings";', 'this.b_tvsettings.label = "Settings & Mods";')
        options = options.replace('this.t_title.text = "UI_TVSettings";', 'this.t_title.text = "Settings & Mods";')
        options = options.replace('this.b_controller.label = "UI_User_Controls";', 'this.b_controller.label = "Controls & Camera";')
        options = options.replace('this.t_title.text = "UI_User_Controls";', 'this.t_title.text = "Controls & Camera";')
        options = options.replace('this.b_select.label = "UI_ToggleControls";', 'this.b_select.label = "Rebind";')
        imports = work / "import"
        package = imports / "__Packages/em2/options"
        package.mkdir(parents=True, exist_ok=True)
        (package / "Options.as").write_text(options, encoding="utf-8")
        shutil.copyfile(ROOT / "assets/pc-menu/TVSettings.as", package / "TVSettings.as")
        shutil.copyfile(ROOT / "assets/pc-menu/PlayerControls.as", package / "PlayerControls.as")
        modified = work / "modified.gfx"
        run("java", "-jar", args.ffdec, "-importScript", source, modified, imports)
        # The old subpage has unnamed static brightness charts. Remove only
        # those placements, retaining its original named text fields and font.
        xml_path = work / "menu.xml"
        run("java", "-jar", args.ffdec, "-swf2xml", modified, xml_path)
        document = ET.parse(xml_path)
        root = document.getroot()
        exports = [tag for tag in root.iter() if tag.get("type") == "ExportAssetsTag"]
        sprite_ids = [id_node.text for tag in exports
                      for id_node, name_node in zip(tag.find("tags"), tag.find("names"))
                      if name_node.text == "TVSettings"]
        if len(sprite_ids) != 1:
            raise RuntimeError("Expected one TVSettings sprite export")
        sprite = next(tag for tag in root.iter() if tag.get("type") == "DefineSpriteTag"
                      and tag.get("spriteId") == sprite_ids[0])
        tags = sprite.find("subTags")
        for tag in list(tags):
            if tag.get("type", "").startswith("PlaceObject") and tag.get("placeFlagHasName") == "false":
                tags.remove(tag)
        controller_ids = [id_node.text for tag in exports
                          for id_node, name_node in zip(tag.find("tags"), tag.find("names"))
                          if name_node.text == "ControllerSettings"]
        if len(controller_ids) != 1:
            raise RuntimeError("Expected one controller-settings sprite export")
        controller = next(tag for tag in root.iter() if tag.get("type") == "DefineSpriteTag"
                          and tag.get("spriteId") == controller_ids[0])
        controller_tags = controller.find("subTags")
        for tag in list(controller_tags):
            if tag.get("type", "").startswith("PlaceObject") and tag.get("name") not in ("t_c1", "t_title"):
                controller_tags.remove(tag)
        document.write(xml_path, encoding="utf-8", xml_declaration=True)
        run("java", "-jar", args.ffdec, "-xml2swf", xml_path, modified)
        result = [(name, modified.read_bytes() if name == member else blob, kind) for name, blob, kind in files]
        pak.write_pak(result, ROOT / "private/pc-assets/packfiles" / archive)
        print("Verified modified menu archive:", archive)
    files = pak.read_pak(args.game_dir / "packfiles/UIhud.pak")
    member = "ui/flash/hud/hud.gfx"
    work = ROOT / "private/pc-menu-build/UIhud"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "original.gfx"
    source.write_bytes(next(blob for name, blob, kind in files if name.lower() == member))
    exported = work / "export"
    run("java", "-jar", args.ffdec, "-export", "script", exported, source)
    hud = (exported / "scripts/__Packages/em2/hud/Hud.as").read_text(encoding="utf-8-sig")
    patches = {
        'this._oswaldControlled = value;':
        'this._oswaldControlled = value;\n      if(this._pcSelectedCharacter)\n      {\n         this.pcSelectedCharacter = 1;\n         return undefined;\n      }',
        'this.oswaldControlled = _level0.BindGameProperty("OswaldIsPlayerControlled",String(this),"oswaldControlled");':
        'this.oswaldControlled = _level0.BindGameProperty("OswaldIsPlayerControlled",String(this),"oswaldControlled");\n      this.pcSelectedCharacter = _level0.BindGameProperty("PCSelectedCharacter",String(this),"pcSelectedCharacter");',
        'function _updatePlayerHUDVisibility()\n   {':
        'function _updatePlayerHUDVisibility()\n   {\n      if(this._pcSelectedCharacter)\n      {\n         this._showSelectedCharacterHUD();\n         return undefined;\n      }',
        'this.oswaldCameraReticle._x = this._screenCenter + (this._screenCenter >> 1) - (_global.offsetX >> 1);':
        'this.oswaldCameraReticle._x = this._pcSelectedCharacter ? this._screenCenter : this._screenCenter + (this._screenCenter >> 1) - (_global.offsetX >> 1);',
    }
    for before, after in patches.items():
        if hud.count(before) != 1:
            raise RuntimeError("Unexpected Hud.as structure; refusing an unverified patch: " + before)
        hud = hud.replace(before, after)
    end = hud.rfind("}")
    hud = hud[:end] + (ROOT / "assets/pc-menu/CharacterSwitchHud.as").read_text() + hud[end:]
    imports = work / "import"
    package = imports / "__Packages/em2/hud"
    package.mkdir(parents=True, exist_ok=True)
    (package / "Hud.as").write_text(hud, encoding="utf-8")
    modified = work / "modified.gfx"
    run("java", "-jar", args.ffdec, "-importScript", source, modified, imports)
    pak.write_pak([(name, modified.read_bytes() if name.lower() == member else blob, kind)
                   for name, blob, kind in files], ROOT / "private/pc-assets/packfiles/UIhud.pak")
    behavior_module = runpy.run_path(str(ROOT / 'scripts/patch-player-behaviors.py'))
    patch_behavior = behavior_module['patch']
    dynamic = pak.read_pak(args.game_dir / 'packfiles/_Dynamic.pak')
    originals = {name.lower(): blob for name, blob, kind in dynamic}
    clips = []
    conversion_report = {'clips': [], 'source': 'retail animations'}
    if emr_glb is not None:
        convert = runpy.run_path(str(ROOT / 'scripts/emr_ability_clips.py'))['compile_glb_clips']
        clips, conversion_report, _ = convert(emr_glb,
            originals['characters/mickey/animations/mickey_tpose.hkx'],
            originals['characters/mickey/characters/mickey_rig_skin.hkx'])
    ability_clips = {name: blob for name, blob, kind in clips}
    modified_dynamic = []; graph_reports = []
    behaviors = {'characters/mickey/behaviors/mickeybehavior.hkx':'mickey', 'characters/oswald/behaviors/oswaldbehavior.hkx':'oswald'}
    metadata = {'characters/mickey/mickey_default.hkw': 'append_emr_hkw',
                'characters/mickey/characters/mickey.hkx': 'append_emr_character',
                'characters/mickey/mickeybehaviorproject.hkx': 'append_emr_project'}
    if not clips:
        metadata = {}
    patched_metadata = set()
    for name, blob, kind in dynamic:
        key = name.lower()
        if key in behaviors:
            actor = behaviors[key]
            blob, report = patch_behavior(blob, actor, ability_clips=ability_clips or None) if actor == 'mickey' else patch_behavior(blob, actor)
            graph_reports.append(report)
        if key in metadata:
            blob = behavior_module[metadata[key]](blob, ability_clips); patched_metadata.add(key)
        modified_dynamic.append((name,blob,kind))
    if len(graph_reports)!=2 or patched_metadata != set(metadata):
        raise RuntimeError('Both original graphs and all three Mickey clip dependency resources are required')
    if any(name.lower() in originals for name, blob, kind in clips):
        raise RuntimeError('Imported ability clips would replace an original animation')
    modified_dynamic.extend(clips)
    pak.write_pak(modified_dynamic,ROOT / 'private/pc-assets/packfiles/_Dynamic.pak')
    (ROOT / 'analysis/native-ability-graph-report.json').write_text(json.dumps(graph_reports,indent=2))
    (ROOT / 'analysis/emr-ability-conversion-report.json').write_text(json.dumps(conversion_report,indent=2))
    outputs = {"packfiles/" + name: hashlib.sha256((ROOT / "private/pc-assets/packfiles" / name).read_bytes()).hexdigest()
               for name in ("UIOptions.pak", "UIPauseOptions.pak", "UIhud.pak", "_Dynamic.pak")}
    manifest_path.write_text(json.dumps({"fingerprint": fingerprint, "outputs": outputs}, indent=2))


if __name__ == "__main__":
    main()

"""Bridge the installed SDK sampler, processor factory, constant reader and writer.

Only a build-directory archive is changed. The installed SDK stays untouched.
Renaming the definition in its single object keeps callers in other archive
members bound to the application wrapper, which delegates to the original.
The factory's vtable relocation selects a real derived command processor;
its original code, unwind information and debug references are retained.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from coff_bridge import redirect_vtable_definition,redirect_direct_call_definition,redirect_pso_creation,guard_pipeline_destruction

SYMBOL = '?GetSamplerParameters@D3D12TextureCache@d3d12@graphics@rex@@QEBA?ATSamplerParameters@1234@AEBUSamplerBinding@DxbcShader@34@@Z'
BRIDGE = 'EM2OriginalSamplerParameters'
MEMBER = 'texture_cache.cpp.obj'
FACTORY_MEMBER = 'graphics_system.cpp.obj'
FACTORY_SYMBOL = '?CreateCommandProcessor@D3D12GraphicsSystem@d3d12@graphics@rex@@MEAA?AV?$unique_ptr@VCommandProcessor@graphics@rex@@U?$default_delete@VCommandProcessor@graphics@rex@@@std@@@std@@XZ'
FACTORY_VTABLE = '??_7D3D12GraphicsSystem@d3d12@graphics@rex@@6B@'
FACTORY_SDK_SHA256 = 'bda95313e5f5a9c6db99c0e4363d9543c9e83cdd3ce477615c8ae77291eaa951'
UI_SDK_SHA256='43a9315705a876daf7dd46b0c81a93c55431ea9d6f45144ec1282983d001ed04'
ROOT_SYMBOL='?CreateRootSignature@util@d3d12@ui@rex@@YAPEAUID3D12RootSignature@@AEBVD3D12Provider@234@AEBUD3D12_ROOT_SIGNATURE_DESC@@@Z'
ROOT_BRIDGE='EM2OriginalCreateRootSignature'
PACKET_MEMBER='command_processor.cpp.obj' 
PACKET_SYMBOL='?ExecutePacketType0@CommandProcessor@graphics@rex@@IEAA_NPEAVRingBuffer@memory@3@I@Z'
WRITE_SYMBOL='?WriteRegister@D3D12CommandProcessor@d3d12@graphics@rex@@MEAAXII@Z'
WRITE_VTABLE='??_7D3D12CommandProcessor@d3d12@graphics@rex@@6B@'


def run(*args, cwd=None):
    return subprocess.check_output([str(x) for x in args], cwd=cwd, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--ui-source',type=Path,required=True)
    parser.add_argument('--ui-output',type=Path,required=True)
    parser.add_argument('--ar', required=True)
    parser.add_argument('--objcopy', required=True)
    parser.add_argument('--nm', required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    ui_source,ui_output=args.ui_source.resolve(),args.ui_output.resolve()
    ui_digest=hashlib.sha256(ui_source.read_bytes()).hexdigest()
    if ui_digest!=UI_SDK_SHA256:raise RuntimeError('Revalidate the root-signature ABI before bridging this UI SDK archive')
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != FACTORY_SDK_SHA256:
        raise RuntimeError('Revalidate the command-processor factory implementation and ABI for this SDK archive before bridging it')
    fingerprint = hashlib.sha256(Path(__file__).read_bytes() + Path(__file__).with_name('coff_bridge.py').read_bytes()).hexdigest()
    manifest = output.with_suffix('.json')
    if output.exists() and manifest.exists():
        old = json.loads(manifest.read_text())
        if ui_output.exists() and old.get('ui_source_sha256')==ui_digest and old.get('ui_output_sha256')==hashlib.sha256(ui_output.read_bytes()).hexdigest() and old.get('source_sha256') == digest and old.get('script_sha256') == fingerprint and old.get('output_sha256') == hashlib.sha256(output.read_bytes()).hexdigest():
            return
    output.parent.mkdir(parents=True, exist_ok=True)
    members = run(args.ar, 't', source).splitlines()
    if members.count(MEMBER) != 1:
        raise RuntimeError('Unsupported SDK archive: expected exactly one texture cache object')
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        work = Path(temporary)
        run(args.ar, 'x', source, MEMBER, cwd=work)
        obj = work / MEMBER
        symbols = run(args.nm, '--defined-only', obj)
        if sum(line.endswith(' '+SYMBOL) for line in symbols.splitlines()) != 1:
            raise RuntimeError('Unsupported SDK sampler ABI; refusing to rewrite the archive')
        run(args.objcopy, '--redefine-sym='+SYMBOL+'='+BRIDGE, obj)
        if BRIDGE not in run(args.nm, '--defined-only', obj):
            raise RuntimeError('Original sampler bridge was not exported')
        texture_bridges=[]
        for symbol,bridge in (
            ('?RequestSwapTexture@D3D12TextureCache@d3d12@graphics@rex@@QEAAPEAUID3D12Resource@@AEAUD3D12_SHADER_RESOURCE_VIEW_DESC@@AEAW4TextureFormat@xenos@34@@Z','EM2OriginalRequestSwapTexture'),
            ('?AreActiveTextureSRVKeysUpToDate@D3D12TextureCache@d3d12@graphics@rex@@QEBA_NPEBUTextureSRVKey@1234@PEBUTextureBinding@DxbcShader@34@_K@Z','EM2OriginalAreActiveTextureSRVKeysUpToDate'),
            ('?GetActiveTextureBindlessSRVIndex@D3D12TextureCache@d3d12@graphics@rex@@QEAAIAEBUTextureBinding@DxbcShader@34@@Z','EM2OriginalGetActiveTextureBindlessSRVIndex'),
            ('?WriteActiveTextureBindfulSRV@D3D12TextureCache@d3d12@graphics@rex@@QEAAXAEBUTextureBinding@DxbcShader@34@UD3D12_CPU_DESCRIPTOR_HANDLE@@@Z','EM2OriginalWriteActiveTextureBindfulSRV'),
            ('?WriteActiveTextureSRVKeys@D3D12TextureCache@d3d12@graphics@rex@@QEBAXPEAUTextureSRVKey@1234@PEBUTextureBinding@DxbcShader@34@_K@Z','EM2OriginalWriteActiveTextureSRVKeys')):
            if sum(line.endswith(' '+symbol) for line in run(args.nm,'--defined-only',obj).splitlines())!=1:raise RuntimeError('Native HDR texture binding ABI changed: '+symbol)
            run(args.objcopy,'--redefine-sym='+symbol+'='+bridge,obj)
            texture_bridges.append({'symbol':symbol,'bridge':bridge})
        staged = work / output.name
        shutil.copyfile(source, staged)
        run(args.ar, 'r', staged, obj)
        shared_member='shared_memory.cpp.obj'
        shared_symbol='?CompletedSubmissionUpdated@D3D12SharedMemory@d3d12@graphics@rex@@QEAAXXZ'
        shared_candidates=[]
        for index in range(1,members.count(shared_member)+1):
            directory=work/f'shared-upload-{index}';directory.mkdir()
            run(args.ar,'xN',index,source,shared_member,cwd=directory)
            candidate=directory/shared_member
            if any(line.endswith(' '+shared_symbol) for line in run(args.nm,'--defined-only',candidate).splitlines()):
                shared_candidates.append((index,candidate))
        if len(shared_candidates)!=1:raise RuntimeError('Expected one D3D12 shared-upload completion implementation')
        shared_index,shared_object=shared_candidates[0]
        run(args.objcopy,'--redefine-sym='+shared_symbol+'=EM2OriginalSharedUploadCompletion',shared_object)
        run(args.ar,'dN',shared_index,staged,shared_member)
        run(args.ar,'q',staged,shared_object)
        candidates = []
        for instance in range(1, members.count(FACTORY_MEMBER) + 1):
            candidate = work / f'factory-{instance}'
            candidate.mkdir()
            run(args.ar, 'xN', instance, source, FACTORY_MEMBER, cwd=candidate)
            factory_object = candidate / FACTORY_MEMBER
            if any(line.endswith(' ' + FACTORY_SYMBOL) for line in run(args.nm, '--defined-only', factory_object).splitlines()):
                candidates.append((instance, factory_object))
        if len(candidates) != 1:
            raise RuntimeError('Expected exactly one D3D12 command-processor factory')
        instance, factory_object = candidates[0]
        # Removing addrsig makes address significance conservative for this
        # small factory object; no stale symbol indices survive the split.
        run(args.objcopy, '--remove-section=.llvm_addrsig', factory_object)
        rewritten, factory_info = redirect_vtable_definition(factory_object.read_bytes(), FACTORY_SYMBOL,
            'EM2UnusedOriginalCommandProcessorFactory', FACTORY_VTABLE)
        factory_object.write_bytes(rewritten)
        run(args.ar, 'dN', instance, staged, FACTORY_MEMBER)
        run(args.ar, 'q', staged, factory_object)
        write_candidates=[]
        for write_instance in range(1,members.count(PACKET_MEMBER)+1):
            write_directory=work/f'write-{write_instance}';write_directory.mkdir()
            run(args.ar,'xN',write_instance,source,PACKET_MEMBER,cwd=write_directory)
            write_object=write_directory/PACKET_MEMBER
            if any(line.endswith(' '+WRITE_SYMBOL) for line in run(args.nm,'--defined-only',write_object).splitlines()):
                write_candidates.append((write_instance,write_object))
        if len(write_candidates)!=1:raise RuntimeError('Expected one D3D12 register writer')
        write_instance,write_object=write_candidates[0]
        run(args.objcopy,'--remove-section=.llvm_addrsig',write_object)
        write_bytes,write_info=redirect_vtable_definition(write_object.read_bytes(),WRITE_SYMBOL,'EM2OriginalD3DRegister',WRITE_VTABLE)
        write_info['method_symbol']=write_info.pop('factory_symbol')
        write_object.write_bytes(write_bytes)
        run(args.ar,'dN',write_instance,staged,PACKET_MEMBER)
        run(args.ar,'q',staged,write_object)
        packet_candidates=[]
        for packet_instance in range(1,members.count(PACKET_MEMBER)+1):
            packet_directory=work/f'packet-{packet_instance}'
            packet_directory.mkdir()
            run(args.ar,'xN',packet_instance,source,PACKET_MEMBER,cwd=packet_directory)
            packet_object=packet_directory/PACKET_MEMBER
            if any(line.endswith(' '+PACKET_SYMBOL) for line in run(args.nm,'--defined-only',packet_object).splitlines()):
                packet_candidates.append((packet_instance,packet_object))
        if len(packet_candidates)!=1:
            raise RuntimeError('Expected exactly one generic Type-0 packet handler')
        packet_instance,packet_object=packet_candidates[0]
        run(args.objcopy,'--remove-section=.llvm_addrsig',packet_object)
        packet_bytes,packet_info=redirect_direct_call_definition(packet_object.read_bytes(),PACKET_SYMBOL,'EM2OriginalPacketType0')
        type3_info=[]
        for name in ('SET_CONSTANT','SET_CONSTANT2','SET_SHADER_CONSTANTS'):
            symbol='?ExecutePacketType3_'+name+'@CommandProcessor@graphics@rex@@IEAA_NPEAVRingBuffer@memory@3@II@Z'
            packet_bytes,info=redirect_direct_call_definition(packet_bytes,symbol,'EM2Original'+name)
            type3_info.append(info)
        packet_object.write_bytes(packet_bytes)
        run(args.ar,'dN',packet_instance,staged,PACKET_MEMBER)
        run(args.ar,'q',staged,packet_object)
        run(args.ar, 's', staged)
        pipeline_dir=work/'pipeline';pipeline_dir.mkdir()
        if members.count('pipeline_cache.cpp.obj')!=1:raise RuntimeError('Expected one native pipeline-cache object')
        run(args.ar,'x',source,'pipeline_cache.cpp.obj',cwd=pipeline_dir)
        pipeline_object=pipeline_dir/'pipeline_cache.cpp.obj'
        run(args.objcopy,'--remove-section=.llvm_addrsig',pipeline_object)
        pipeline_bytes,pso_info=redirect_pso_creation(pipeline_object.read_bytes())
        pipeline_bytes,pipeline_retirement_info=guard_pipeline_destruction(pipeline_bytes)
        create_pipeline_symbol='?CreateD3D12Pipeline@PipelineCache@d3d12@graphics@rex@@AEAAPEAUID3D12PipelineState@@AEBUPipelineRuntimeDescription@1234@@Z'
        pipeline_bytes,pipeline_scope_info=redirect_direct_call_definition(pipeline_bytes,create_pipeline_symbol,'EM2OriginalCreateD3D12Pipeline',expected_calls=4)
        material_symbol='?TranslateAnalyzedShader@PipelineCache@d3d12@graphics@rex@@AEAA_NAEAVDxbcShaderTranslator@34@AEAVD3D12Translation@D3D12Shader@234@PEAUIDxbcConverter@@PEAUIDxcUtils@@PEAUIDxcCompiler@@@Z'
        pipeline_bytes,material_translate_info=redirect_direct_call_definition(pipeline_bytes,material_symbol,'EM2OriginalTranslateMaterial',expected_calls=3)
        pipeline_object.write_bytes(pipeline_bytes)
        load_symbol='?LoadShader@PipelineCache@d3d12@graphics@rex@@QEAAPEAVD3D12Shader@234@W4ShaderType@xenos@34@PEBII@Z'
        if sum(line.endswith(' '+load_symbol) for line in run(args.nm,'--defined-only',pipeline_object).splitlines())!=1:
            raise RuntimeError('Native shader loader ABI changed')
        run(args.objcopy,'--redefine-sym='+load_symbol+'=EM2OriginalLoadShader',pipeline_object)
        run(args.ar,'r',staged,pipeline_object)
        translator_member='translator.cpp.obj'
        translator_symbol='?TranslateAnalyzedShader@ShaderTranslator@graphics@rex@@QEAA_NAEAVTranslation@Shader@23@@Z'
        if members.count(translator_member)!=1:raise RuntimeError('Expected one native shader translator object')
        translator_dir=work/'shader-translator';translator_dir.mkdir()
        run(args.ar,'x',source,translator_member,cwd=translator_dir)
        translator_object=translator_dir/translator_member
        if sum(line.endswith(' '+translator_symbol) for line in run(args.nm,'--defined-only',translator_object).splitlines())!=1:raise RuntimeError('Native shader translator ABI changed')
        run(args.objcopy,'--remove-section=.llvm_addrsig',translator_object)
        run(args.objcopy,'--redefine-sym='+translator_symbol+'=EM2OriginalTranslateShader',translator_object)
        run(args.ar,'r',staged,translator_object)
        deferred_member='deferred_command_list.cpp.obj'
        deferred_symbol='?Execute@DeferredCommandList@d3d12@graphics@rex@@QEAAXPEAUID3D12GraphicsCommandList@@PEAUID3D12GraphicsCommandList1@@@Z'
        if members.count(deferred_member)!=1:raise RuntimeError('Expected one native deferred-command-list object')
        deferred_dir=work/'deferred';deferred_dir.mkdir()
        run(args.ar,'x',source,deferred_member,cwd=deferred_dir)
        deferred_object=deferred_dir/deferred_member
        if sum(line.endswith(' '+deferred_symbol) for line in run(args.nm,'--defined-only',deferred_object).splitlines())!=1:raise RuntimeError('Deferred execute ABI changed')
        run(args.objcopy,'--remove-section=.llvm_addrsig',deferred_object)
        run(args.objcopy,'--redefine-sym='+deferred_symbol+'=EM2OriginalDeferredExecute',deferred_object)
        writer_symbol='?WriteCommand@DeferredCommandList@d3d12@graphics@rex@@AEAAPEAXW4Command@1234@_K@Z'
        if sum(line.endswith(' '+writer_symbol) for line in run(args.nm,'--defined-only',deferred_object).splitlines())!=1:raise RuntimeError('Deferred writer ABI changed')
        run(args.objcopy,'--redefine-sym='+writer_symbol+'=EM2OriginalDeferredWriteCommand',deferred_object)
        run(args.ar,'r',staged,deferred_object)
        resolve_bridges=[]
        for member,symbol,bridge in (
            ('draw.cpp.obj','?GetResolveInfo@draw_util@graphics@rex@@YA_NAEBVRegisterFile@23@AEBVMemory@memory@3@AEAVTraceWriter@23@II_N3AEAUResolveInfo@123@@Z','EM2OriginalGetResolveInfo'),
            ('cache.cpp.obj','?MarkRangeAsResolved@TextureCache@graphics@rex@@QEAAXII@Z','EM2OriginalMarkRangeAsResolved'),
            ('cache.cpp.obj','?DestroyAllTextures@TextureCache@graphics@rex@@IEAAX_N@Z','EM2OriginalDestroyAllTextures')):
            matches=[]
            for ordinal in range(1,members.count(member)+1):
                directory=work/f'resolve-{bridge}-{ordinal}';directory.mkdir()
                run(args.ar,'xN',ordinal,staged,member,cwd=directory)
                candidate=directory/member
                if any(line.endswith(' '+symbol) for line in run(args.nm,'--defined-only',candidate).splitlines()):matches.append((ordinal,candidate))
            if len(matches)!=1:raise RuntimeError('Expected one native resolve observer target: '+bridge)
            ordinal,candidate=matches[0]
            run(args.objcopy,'--remove-section=.llvm_addrsig',candidate)
            run(args.objcopy,'--redefine-sym='+symbol+'='+bridge,candidate)
            run(args.ar,'dN',ordinal,staged,member);run(args.ar,'q',staged,candidate);run(args.ar,'s',staged)
            resolve_bridges.append({'member':member,'symbol':symbol,'bridge':bridge})
        ui_dir=work/'ui';ui_dir.mkdir()
        if run(args.ar,'t',ui_source).splitlines().count('d3d12_util.cpp.obj')!=1:raise RuntimeError('Expected one UI root-signature object')
        run(args.ar,'x',ui_source,'d3d12_util.cpp.obj',cwd=ui_dir)
        root_object=ui_dir/'d3d12_util.cpp.obj'
        if sum(line.endswith(' '+ROOT_SYMBOL) for line in run(args.nm,'--defined-only',root_object).splitlines())!=1:raise RuntimeError('Root signature ABI changed')
        run(args.objcopy,'--redefine-sym='+ROOT_SYMBOL+'='+ROOT_BRIDGE,root_object)
        compute_symbol='?CreateComputePipeline@util@d3d12@ui@rex@@YAPEAUID3D12PipelineState@@PEAUID3D12Device@@PEBX_KPEAUID3D12RootSignature@@@Z'
        if sum(line.endswith(' '+compute_symbol) for line in run(args.nm,'--defined-only',root_object).splitlines())!=1:raise RuntimeError('Compute pipeline helper ABI changed')
        run(args.objcopy,'--redefine-sym='+compute_symbol+'=EM2OriginalCreateComputePipeline',root_object)
        staged_ui=ui_dir/ui_output.name;shutil.copyfile(ui_source,staged_ui)
        run(args.ar,'r',staged_ui,root_object)
        ui_output.parent.mkdir(parents=True,exist_ok=True)
        # Rebuilt from the matching upstream source in the game target so HDR
        # can change presentation without editing the installed SDK. Keep all
        # other UI objects (including the root-signature bridge) intact.
        if run(args.ar,'t',staged_ui).splitlines().count('d3d12_presenter.cpp.obj')!=1:
            raise RuntimeError('Expected one source-replaced presenter object')
        run(args.ar,'d',staged_ui,'d3d12_presenter.cpp.obj')
        # Source-build the pinned upload pool to avoid double Release on Map
        # failure. The SDK archive hash above fixes the source/header ABI.
        upload_member='d3d12_upload_buffer_pool.cpp.obj'
        if run(args.ar,'t',staged_ui).splitlines().count(upload_member)!=1:
            raise RuntimeError('Expected one source-replaced upload-buffer pool object')
        run(args.ar,'d',staged_ui,upload_member)
        shutil.copyfile(staged_ui,ui_output)
        # Source-build the matching render-target cache as well. HDR requires
        # coordinated resource/RTV/SRV, transfer and resolve changes inside it;
        # replacing only format getters would leave invalid 8-bit sRGB views.
        if run(args.ar,'t',staged).splitlines().count('render_target_cache.cpp.obj')!=1:
            raise RuntimeError('Expected one source-replaced render-target cache object')
        run(args.ar,'d',staged,'render_target_cache.cpp.obj')
        shutil.copyfile(staged, output)
    manifest.write_text(json.dumps({'source': str(source), 'source_sha256': digest,
        'script_sha256': fingerprint, 'output_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
        'pipeline_retirement': pipeline_retirement_info,
        'shared_upload_completion_bridge': 'EM2OriginalSharedUploadCompletion',
        'renamed_symbol': SYMBOL, 'bridge': BRIDGE, 'factory': factory_info,'packet':packet_info,'constant_writer':write_info,'type3_constants':type3_info,'pso_creation':pso_info,'pipeline_scope':pipeline_scope_info,'material_translation':material_translate_info,'ui_source_sha256':ui_digest,'ui_output_sha256':hashlib.sha256(ui_output.read_bytes()).hexdigest(),'root_signature_bridge':ROOT_BRIDGE,'deferred_execute_bridge':'EM2OriginalDeferredExecute','deferred_writer_bridge':'EM2OriginalDeferredWriteCommand','resolve_observer_bridges':resolve_bridges,'hdr_texture_bridges':texture_bridges}, indent=2)+'\n')


if __name__ == '__main__':
    main()

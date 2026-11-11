#include "built_shaders.h"
#include "hdr_output_shader.h"
#include "hdr_guest_view_shader.h"
#include "hdr_scene_resolve_shader.h"
#include "scene_ambient_shader.h"
#include "scene_geometry_shader.h"
#include "scene_unproject_shader.h"
#include "scene_indirect_shader.h"
#include "scene_occlusion_shader.h"
#include "scene_raster_geometry_shader.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <rex/logging.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <immintrin.h>
#include <string>
bool em2::TestBuiltShaders(const std::filesystem::path& directory){
    struct Fp {unsigned value=_mm_getcsr();~Fp(){_mm_setcsr(value);}}fp;
    const auto library=LoadLibraryW(L"d3dcompiler_47.dll");if(!library)return false;
    struct Library {HMODULE handle;~Library(){FreeLibrary(handle);}}release{library};
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
    unsigned invalid=0,source_checks=0;size_t bytes=0;
    const auto begin=std::chrono::steady_clock::now();
    for(const auto& record:BuiltShaderRecords()){
        const auto* blob=GetBuiltShader(record.id);
        if(!blob || blob->bytes!=record.blob.bytes || blob->size!=record.blob.size || blob->size<32 || std::memcmp(blob->bytes,"DXBC",4))return false;
        std::string original;
        switch(record.id){
        case BuiltShader::HdrOutput:original=kHdrOutputShader;break;
        case BuiltShader::HdrGuestView:original=kHdrGuestViewShader;break;
        case BuiltShader::HdrResolve:case BuiltShader::HdrMSResolve:original=kHdrSceneResolveShader;break;
        case BuiltShader::SceneAmbient:original=kSceneAmbientShader;break;
        case BuiltShader::SceneGeometry:original=std::string(kSceneUnprojectShader)+kSceneGeometryShader;break;
        case BuiltShader::SceneIndirect:case BuiltShader::SceneIndirectLoopEmitters:case BuiltShader::SceneIndirectLoopAll:original=kSceneIndirectShader;break;
        case BuiltShader::SceneOcclusion:original=kSceneOcclusionShader;break;
        case BuiltShader::SceneRasterGeometry:original=std::string(kSceneUnprojectShader)+kSceneRasterGeometryShader;break;
        default:break;
        }
        if(!original.empty()){++source_checks;if(original!=record.source)return false;}
        const D3D_SHADER_MACRO macros[]={{record.macro_name,record.macro_value},{nullptr,nullptr}};
        Microsoft::WRL::ComPtr<ID3DBlob> actual,error;
        const auto hr=compile(record.source,std::strlen(record.source),record.name,record.macro_name?macros:nullptr,nullptr,record.entry,record.profile,record.flags,0,&actual,&error);
        if(FAILED(hr) || !actual || actual->GetBufferSize()!=blob->size || std::memcmp(actual->GetBufferPointer(),blob->bytes,blob->size)){
            ++invalid;REXLOG_ERROR("Built shader mismatch {}: {}",record.name,error?static_cast<const char*>(error->GetBufferPointer()):"bytecode differs");
        }
        bytes+=blob->size;
    }
    const auto compile_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    if(GetBuiltShader(BuiltShader::Count) || GetBuiltShader(static_cast<BuiltShader>(UINT_MAX)))return false;
    std::filesystem::create_directories(directory);std::ofstream report(directory/"built-shaders-results.json");
    report<<"{\"programs\":"<<BuiltShaderRecords().size()<<",\"bytes\":"<<bytes<<",\"source_checks\":"<<source_checks<<",\"bytecode_mismatches\":"<<invalid<<",\"reference_compile_ms\":"<<compile_ms<<",\"scope\":\"All packaged PC renderer HLSL variants recompiled with original profiles, flags and macros; exact bytecode comparison and generated-source agreement. Production retrieves immutable embedded bytecode without HLSL compilation. Driver pipeline creation remains.\"}\n";
    REXLOG_INFO("Built renderer shaders: {} programs, {} bytes, {} source checks, {} mismatches; reference compiler {} ms",BuiltShaderRecords().size(),bytes,source_checks,invalid,compile_ms);
    return !invalid && source_checks==11 && report.good();
}

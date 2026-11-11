#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
namespace em2 {
enum class BuiltShader : unsigned {
    AuxiliaryVS, AuxiliaryMSVS, AuxiliaryDepth, AuxiliaryMSDepth,
    AuxiliaryStencil, AuxiliaryMSStencil, HdrGuestView, HdrOutput,
    HdrUIVS, HdrUIPS, HdrSDRPaint, HdrResolve, HdrMSResolve,
    ScaledResolve, SceneAmbient, SceneGeometry, SceneIndirect,
    SceneIndirectLoopEmitters, SceneIndirectLoopAll,
    SceneOcclusion, SceneRasterGeometry, DirectColorResolve, Count
};
// Bytecode is immutable and lives with the executable. No compiler or disk
// access is involved in retrieving a built shader.
struct BuiltShaderBlob {
    const uint8_t* bytes;size_t size;
    const void* GetBufferPointer()const{return bytes;}
    size_t GetBufferSize()const{return size;}
};
struct BuiltShaderRecord {
    BuiltShader id;BuiltShaderBlob blob;
    const char *source,*name,*entry,*profile,*macro_name,*macro_value;
    uint32_t flags;
};
std::span<const BuiltShaderRecord> BuiltShaderRecords();
const BuiltShaderBlob* GetBuiltShader(BuiltShader);
// Diagnostic only: compare every packaged program with the source compiler.
bool TestBuiltShaders(const std::filesystem::path&);
}

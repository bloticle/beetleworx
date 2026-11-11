#include "built_shaders.h"
#include "built_shaders_data.h"
std::span<const em2::BuiltShaderRecord> em2::BuiltShaderRecords(){return kBuiltShaderRecords;}
const em2::BuiltShaderBlob* em2::GetBuiltShader(BuiltShader id){
    const auto index=static_cast<unsigned>(id);
    if(index>=std::size(kBuiltShaderRecords) || kBuiltShaderRecords[index].id!=id)return nullptr;
    return &kBuiltShaderRecords[index].blob;
}

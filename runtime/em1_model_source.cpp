#include "em1_model_source.h"
#include "em1_nif_textures.h"
#include "em1_baked_lighting.h"
#include "em1_jigsaw_havok.h"
#include <algorithm>
namespace em2::em1 {
bool ReadAdaptedModel(std::shared_ptr<RvzSource> source, std::string_view archive,
                      std::string_view resource, AdaptedModelResource& output,
                      std::string& error,const ModelLighting* lighting) {
  try {
    auto local = PakSource::Open(source, archive, error);
    if (!local) return false;
    std::string path(resource);
    for(auto& c:path){if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;}
    auto contains=[&](const PakSource& pak){return std::any_of(pak.Entries().begin(),pak.Entries().end(),[&](const auto& entry){return entry.path==path;});};
    PakSource* pak=local.get();std::unique_ptr<PakSource> shared;
    // The original global _Dynamic archive owns common actors/effects/cameras.
    // Local entries win. Never mask a local checksum/decode/read failure.
    if(!contains(*pak)){
      std::string name(archive);for(auto& c:name){if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;}
      if(name!="packfiles/_dynamic.pak"){
        shared=PakSource::Open(source,"packfiles/_dynamic.pak",error);
        if(!shared)return false;
        if(contains(*shared))pak=shared.get();
      }
    }
    std::vector<uint8_t> original;
    if (!pak->ReadOriginal(resource, original, error)) return false;
    uint32_t baked_meshes=0,jigsaw_aggregates=0;
    {
      nif::Nif model(original);bool changed=false;
      if(std::find(model.types.begin(),model.types.end(),"JPSJigsawNode")!=model.types.end()){
        std::vector<JigsawAggregateInfo> aggregates;
        if(!AdaptJigsawHavokBlocks(model,aggregates,error))return false;
        jigsaw_aggregates=uint32_t(aggregates.size());changed=true;
      }
      if(lighting){
      std::vector<uint8_t> cooked;
      if(!local->ReadOriginal(lighting->resource,cooked,error))return false;
      baked_meshes=ApplyBakedLighting(model,cooked,lighting->entity);
      changed=true;
      }
      if(changed)original=model.Write();
    }
    NifMeshResult mesh;
    if (!AdaptNifMeshes(original, mesh, error)) return false;
    if (mesh.unadapted_skin_meshes || mesh.unadapted_modifier_meshes) {
      error = "EM1 model requires an unimplemented skin/morph modifier adapter"; return false;
    }
    NifTextureResult texture;
    if (!AdaptNifTextures(mesh.bytes, texture, error)) return false;
    if (texture.remaining_wii_display_lists) {
      error = "Model still contains unsupported Wii display-list streams"; return false;
    }
    AdaptedModelResource prepared;
    for (auto& c : path) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 'a'-'A'; }
    prepared.origin_key = pak->OriginKey() + ":model-mesh1-texture1:" + std::to_string(path.size()) + ':' + path;
    if(lighting)prepared.origin_key+=":baked1:"+std::to_string(lighting->entity)+":"+std::string(lighting->resource);
    if(jigsaw_aggregates)prepared.origin_key+=":jigsaw2011-v1";
    if(mesh.prepared_indexed_skin_meshes)prepared.origin_key+=":indexed-skin-v1";
    if(mesh.prepared_indexed_static_meshes)prepared.origin_key+=":indexed-static-v1";
    if(mesh.prepared_indexed_morph_meshes)prepared.origin_key+=":relative-position-morph-v1";
    if(mesh.prepared_gx_morph_meshes)prepared.origin_key+=":gx-position-morph-v1";
    prepared.baked_meshes=baked_meshes;
    prepared.converted_jigsaw_aggregates=jigsaw_aggregates;
    prepared.bytes = std::move(texture.bytes);
    prepared.textures = texture.textures;
    prepared.mesh_origins = std::move(mesh.meshes);
    output = std::move(prepared); error.clear(); return true;
  } catch (const std::exception& exception) { error = exception.what(); return false; }
}
}

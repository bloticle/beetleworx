#include "em1_model_closure.h"
#include "em1_nif_container.h"
#include "em1_nif_textures.h"
#include "em1_nif_materials.h"
#include "em1_portable_pixels.h"
#include "em1_resource_catalog.h"
#include <map>
#include <algorithm>
#include <set>
#include <rex/cvar.h>
#include <rex/logging.h>
REXCVAR_DEFINE_BOOL(em1_particle_material_candidate,false,"Debug","Validate original steam names against native EM2 particle techniques");

namespace em2::em1 {
namespace {
std::string TexturePath(std::string_view original) {
  std::string path(original);
  for(auto& c:path) {
    if(c=='\\')c='/';
    if(c>='A'&&c<='Z')c+='a'-'A';
  }
  if(path.starts_with('/'))path.erase(0,1); // Authored game-root texture names.
  nif::Check(!path.empty() && path.size()<=1024 && !path.starts_with('/') &&
      path.find(':')==std::string::npos && path.find('\0')==std::string::npos,
      "Invalid original EM1 texture path");
  for(size_t at=0;at<path.size();) {
    const auto end=path.find('/',at);
    const auto part=path.substr(at,end==std::string::npos ? end : end-at);
    nif::Check(!part.empty() && part!="." && part!="..", "Unsafe EM1 texture path");
    if(end==std::string::npos)break;
    at=end+1;
  }
  nif::Check(path.ends_with(".nif"), "External EM1 texture is not an original texture NIF");
  path+="_wii";
  return path;
}
void Word(nif::Bytes& bytes,size_t at,uint32_t value,bool big) {
  nif::Check(at<=bytes.size() && bytes.size()-at>=4,"Invalid texture link offset");
  for(unsigned i=0;i<4;++i)bytes[at+i]=uint8_t(value>>(8*(big?3-i:i)));
}
size_t TextureBase(nif::View bytes,bool big) {
  const uint64_t base=12ull+4ull*nif::U32(bytes,4,big);
  nif::Check(base+24==bytes.size(),"Unsupported source texture layout");
  return size_t(base);
}
}
bool ReadModelClosure(std::shared_ptr<RvzSource> source,std::string_view archive,
                      std::string_view resource,ModelClosure& output,std::string& error,const ModelLighting* lighting,
                      std::span<const std::string> level_archives) {
  std::string current_texture;
  try {
    ModelClosure prepared;prepared.source=source;
    if(!ReadAdaptedModel(source,archive,resource,prepared.model,error,lighting))return false;
    auto pak=PakSource::Open(source,archive,error);
    if(!pak)return false;
    nif::Nif model(prepared.model.bytes);
    std::map<std::string,uint32_t> pixels;
    std::map<std::string,std::string> texture_identities;
    std::unique_ptr<PakSource> dynamic;
    std::vector<std::unique_ptr<PakSource>> siblings;
    bool siblings_open=false;
    std::unique_ptr<OriginalResourceCatalog> absence_catalog;
    size_t expanded=prepared.model.bytes.size();
    const size_t original_blocks=model.blocks.size();
    for(size_t i=0;i<original_blocks;++i) {
      if(model.types[model.blocks[i].type]!="NiSourceTexture")continue;
      const auto data=model.blocks[i].Data();
      const size_t base=TextureBase(data,model.big);
      nif::Check(data[base]<=1,"Invalid source texture external flag");
      if(!data[base])continue;
      const auto path=TexturePath(model.String(nif::U32(data,base+1,model.big)));
      current_texture=path;
      nif::Check(nif::U32(data,base+5,model.big)==UINT32_MAX,
          "External source texture unexpectedly retains a pixel link");
      // Copy before appending blocks: vector growth can invalidate Block references.
      nif::Bytes replacement(data.begin(),data.end());
      auto cached=pixels.find(path);
      uint32_t link;
      if(cached!=pixels.end())link=cached->second;
      else {
        nif::Check(pixels.size()+prepared.missing_texture_origins.size()<256,"Too many original texture dependencies");
        std::vector<uint8_t> original;
        auto contains=[&](const PakSource& archive){return std::any_of(archive.Entries().begin(),archive.Entries().end(),[&](const PakEntry& entry){return entry.path==path;});};
        PakSource* texture_pak=pak.get();
        // Original particle assets reference shared textures in EM1 _Dynamic.
        // Local definitions win; only absence permits fallback, never a decode,
        // checksum or I/O failure. Do not search unrelated scenes or EM2 content.
        if(!contains(*texture_pak)){
          if(!dynamic)dynamic=PakSource::Open(source,"packfiles/_dynamic.pak",error);
          if(!dynamic){error+=" [shared texture archive for: "+path+"]";return false;}
          texture_pak=dynamic.get();
        }
        bool loaded=false;
        if(!contains(*texture_pak) && !level_archives.empty()){
          // Source scene archives share dependencies. Scope is provided by the
          // complete descriptor, never discovered by searching unrelated PAKs.
          if(!siblings_open){
            nif::Check(level_archives.size()<=1024,"Too many authored dependency archives");
            std::set<std::string> visited;
            for(auto name:level_archives){
              for(auto& ch:name){if(ch=='\\')ch='/';if(ch>='A'&&ch<='Z')ch+='a'-'A';}
              if(!visited.insert(name).second)continue;
              auto sibling=PakSource::Open(source,name,error);
              if(!sibling){error+=" [authored dependency archive: "+name+"]";return false;}
              if(sibling->OriginKey()!=pak->OriginKey() && sibling->OriginKey()!=dynamic->OriginKey())
                siblings.push_back(std::move(sibling));
            }
            siblings_open=true;
          }
          PakSource* selected=nullptr;
          for(const auto& sibling:siblings){
            if(!contains(*sibling))continue;
            std::vector<uint8_t> candidate;
            if(!sibling->ReadOriginal(path,candidate,error)){error+=" [original scoped texture: "+path+"]";return false;}
            nif::Check(!selected || original==candidate,"Conflicting texture definitions in authored level archives");
            if(!selected){original=std::move(candidate);selected=sibling.get();}
          }
          if(selected){texture_pak=selected;loaded=true;}
        }
        if(!loaded && !contains(*texture_pak) && model.version==0x14030109){
          // Original legacy NiStream tolerates an absent external image. Only
          // retain that behavior when a COMPLETE source index proves that no
          // authored relative/root location can own this filename. Decode/I/O
          // errors and out-of-scope existing images must still fail normally.
          if(!absence_catalog){
            absence_catalog=std::make_unique<OriginalResourceCatalog>();
            if(!BuildOriginalResourceCatalog(source,*absence_catalog,error))return false;
          }
          const auto filename=path.substr(path.find_last_of('/')+1);
          bool found=false;
          for(const auto& [candidate,locations]:absence_catalog->resources){
            auto leaf=std::string_view(candidate).substr(candidate.find_last_of('/')+1);
            if(leaf==filename || (filename.ends_with("_wii") && leaf==std::string_view(filename).substr(0,filename.size()-4))){found=true;break;}
          }
          if(!found){
            const auto identity=prepared.model.origin_key+":original-missing-image:"+path;
            // Preserve external/null-pixel semantics. Namespace the filename
            // to prevent an unrelated EM2 texture cache entry satisfying it.
            Word(replacement,base+1,model.AddString(identity),model.big);
            model.blocks[i].replacement=std::move(replacement);
            prepared.missing_texture_origins.push_back(identity);
            REXLOG_WARN("[em1] preserving original missing legacy texture: model='{}' image='{}'; complete {}-archive source index, no replacement pixels",resource,path,absence_catalog->packfiles);
            continue;
          }
        }
        if(!loaded && !texture_pak->ReadOriginal(path,original,error)){error+=" [original texture: "+path+"]";return false;}
        NifTextureResult texture;
        if(!AdaptNifTextures(original,texture,error)){error+=" [original texture: "+path+"]";return false;}
        nif::Nif image(texture.bytes);
        // Validate freshly decoded portable pixels rather than source user-version.
        // We import only freshly emitted NiPixelData, whose explicit portable
        // RGBA layout has no user-version fields. We do NOT copy the texture's
        // original NiSourceTexture/object layout into the destination model.
        nif::Check(image.big==model.big &&
            texture.textures<=1 && !texture.remaining_wii_display_lists &&
            nif::U32(image.footer,0,image.big)==1,"Unsupported original texture container");
        const auto root=nif::U32(image.footer,4,image.big);
        nif::Check(root<image.blocks.size(),"Texture root is outside its container");
        auto pixel=root;
        if(image.types[image.blocks[root].type]=="NiSourceTexture"){
          const auto source_data=image.blocks[root].Data();
          const auto source_base=TextureBase(source_data,image.big);
          nif::Check(!source_data[source_base],"Texture wrapper still refers to an external image");
          pixel=nif::U32(source_data,source_base+5,image.big);
        }
        nif::Check(pixel<image.blocks.size() &&
            image.types[image.blocks[pixel].type]=="NiPixelData","Texture lacks decoded pixel data");
        const auto payload=image.blocks[pixel].Data();
        if(!ValidatePortablePixels(payload,image.big,error)){error+=" [original texture: "+path+"]";return false;}
        nif::Check(payload.size()<=nif::kMaximum-expanded,"Model texture closure exceeds size budget");
        expanded+=payload.size();
        link=model.AddBlock("NiPixelData",nif::Bytes(payload.begin(),payload.end()));
        pixels.emplace(path,link);
        const auto identity=texture_pak->OriginKey()+":"+path;
        texture_identities.emplace(path,identity);
        prepared.texture_origins.push_back(identity);
      }
      replacement[base]=0;
      Word(replacement,base+5,link,model.big);
      // Preserve authored names in metadata; use an origin-qualified texture
      // identity so native texture caches cannot alias an EM2 filename.
      Word(replacement,base+1,model.AddString(texture_identities.at(path)),model.big);
      replacement[replacement.size()-2]=0;
      replacement.back()=0;
      model.blocks[i].replacement=std::move(replacement);
    }
    current_texture.clear();
    const auto native_materials=AdaptPreviewDiffuseMaterials(model);
    const auto particle_materials=REXCVAR_GET(em1_particle_material_candidate)?AdaptPreviewParticleMaterials(model):0;
    prepared.model.bytes=model.Write();
    prepared.model.textures+=uint32_t(pixels.size());
    prepared.model.origin_key+=":texture-closure3";
    for(const auto& identity:prepared.missing_texture_origins)
      prepared.model.origin_key+=":original-missing-image1:"+std::to_string(identity.size())+":"+identity;
    for(const auto& identity:prepared.texture_origins)
      prepared.model.origin_key+=":"+std::to_string(identity.size())+":"+identity;
    prepared.model.origin_key+=":preview-diffuse2-pe-"+std::to_string(native_materials);
    if(REXCVAR_GET(em1_particle_material_candidate))prepared.model.origin_key+=":particle-material1-"+std::to_string(particle_materials);
    output=std::move(prepared);error.clear();return true;
  } catch(const std::exception& exception) {
    error=exception.what();if(!current_texture.empty())error+=" [original texture: "+current_texture+"]";
    return false;
  }
}
}

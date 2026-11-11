#include "em1_nif_meshes.h"
#include "em1_nif_container.h"
#include "em1_nif_skin.h"
#include "em1_nif_morph.h"
#include <set>
#include <map>
#include <bit>
#include <array>

namespace em2::em1 {
namespace {
using namespace nif;
struct Semantic { uint32_t name, index; };
struct Reference { uint32_t stream; uint8_t instance; std::vector<uint16_t> regions; std::vector<Semantic> semantics; };
struct Mesh {
  size_t primitive_at, references_at, tail_at;
  uint16_t submeshes;
  uint8_t instance, skin;
  uint32_t modifiers;
  std::vector<uint32_t> modifier_links;
  uint32_t skin_bytes=0;
  std::vector<Reference> references;
};
uint32_t Count(Cursor& c, uint32_t limit) {
  const auto n = c.Word(); Check(n <= limit, "NIF mesh table exceeds limit"); return n;
}
Mesh ReadMesh(const Nif& nif, View bytes) {
  Cursor c{bytes, 0, nif.big};
  c.Word(); c.Take(size_t(Count(c, 100000)) * 4); c.Word();  // ObjectNET
  c.Take(2 + 12 + 36 + 4); c.Take(size_t(Count(c, 100000)) * 4); c.Word(); // AVObject
  c.Take(size_t(Count(c, 1000)) * 8); c.Take(5); // RenderObject materials
  // Native 825881A0 reads these RenderObject extensions BEFORE NiMesh's
  // primitive. They are conditional records, not opaque words after it.
  // Overwriting their presence flag makes the native reader skip payload and
  // interpret a bound float as the stream count (observed in 825F7ED8).
  if(nif.user_version>=4) {
    const auto present=c.Word();Check(present<=1,"Unknown render-object extension flag");
    if(present)c.Take(8);
  }
  if(nif.user_version>=15)c.Word(); // extra link consumed by NiStream virtual+56
  // Native 825F8580: optional per-mesh user data, then primitive/submeshes.
  if(nif.user_version>=3) {
    const auto present=c.Word();Check(present<=1,"Unknown mesh extension flag");
    if(present)c.Word();
  }
  Mesh mesh{};
  mesh.primitive_at = c.at; c.Word();
  mesh.submeshes = c.Short(); mesh.instance = c.Take(1)[0]; c.Take(16);
  mesh.references_at = c.at;
  const auto refs = Count(c, 1000);
  for (uint32_t i = 0; i < refs; ++i) {
    Reference ref{}; ref.stream = c.Word(); ref.instance = c.Take(1)[0];
    Check(ref.stream < nif.blocks.size(), "Invalid mesh data-stream link");
    const auto regions = c.Short(); Check(regions <= 1000, "Too many submesh regions");
    for (unsigned j = 0; j < regions; ++j) ref.regions.push_back(c.Short());
    const auto semantics = Count(c, 1000);
    for (unsigned j = 0; j < semantics; ++j) {
      Semantic value{c.Word(), c.Word()}; nif.String(value.name); ref.semantics.push_back(value);
    }
    mesh.references.push_back(std::move(ref));
  }
  mesh.tail_at = c.at; mesh.modifiers = Count(c, 1000);
  for(uint32_t i=0;i<mesh.modifiers;++i){const auto link=c.Word();Check(link==UINT32_MAX || link<nif.blocks.size(),"Invalid mesh modifier link");mesh.modifier_links.push_back(link);}
  mesh.skin = nif.user_version >= 10 ? c.Take(1)[0] : 0;
  mesh.skin_bytes=uint32_t(bytes.size()-c.at);
  // Embedded skin/subclass bytes remain opaque unless this mesh is converted.
  if (!mesh.skin && !mesh.modifiers) Check(c.at == bytes.size(), "Unknown static mesh tail");
  return mesh;
}
struct Region { uint32_t start, count; };
struct Stream {
  View buffer; uint32_t clone, stride = 0; uint8_t streamable;
  std::vector<Region> regions;
  std::vector<uint32_t> formats, offsets, widths;
};
Stream ReadStream(const Nif& nif, uint32_t block) {
  const auto& object = nif.blocks[block];
  Check(nif.types[object.type].starts_with("NiDataStream\x01"), "Mesh link is not a data stream");
  Cursor c{object.Data(), 0, nif.big}; Stream result;
  const auto size = c.Word(); result.clone = c.Word();
  const auto regions = Count(c, 1000);
  for (unsigned i = 0; i < regions; ++i) result.regions.push_back({c.Word(), c.Word()});
  const auto components = Count(c, 1000);
  for (unsigned i = 0; i < components; ++i) {
    const auto format = c.Word(); const auto width = ((format >> 16) & 255) * ((format >> 8) & 255);
    Check(width && width <= 64 && result.stride <= 4096 - width, "Unsupported mesh component width");
    result.formats.push_back(format); result.offsets.push_back(result.stride);
    result.widths.push_back(width); result.stride += width;
  }
  result.buffer = c.Take(size); result.streamable = c.Take(1)[0];
  Check(c.at == c.data.size(), "Unknown data-stream tail"); return result;
}
Bytes MakeStream(bool big, uint32_t clone, uint32_t count, uint32_t format,
                 View buffer, uint8_t streamable) {
  Bytes out; Number(out, uint32_t(buffer.size()), big); Number(out, clone, big);
  Number(out, 1, big); Number(out, 0, big); Number(out, count, big);
  Number(out, 1, big); Number(out, format, big); Append(out, buffer);
  Number(out, streamable, big, 1); return out;
}
void WriteReference(Bytes& out, bool big, uint32_t block, uint32_t name, uint32_t index) {
  Number(out, block, big); Number(out, 0, big, 1); Number(out, 1, big, 2);
  Number(out, 0, big, 2); Number(out, 1, big); Number(out, name, big); Number(out, index, big);
}
struct Attribute {
  unsigned kind; uint32_t name, index, block, component; Region region; Stream stream;
};
void FixedUv(Bytes& output,View input,bool big) {
  Check(input.size()==4,"Fixed6.10 UV requires exactly two signed16 components");
  for(unsigned component=0;component<2;++component){
    const auto encoded=U16(input,component*2,big);
    const auto signed_value=std::bit_cast<int16_t>(encoded);
    // Ten fractional bits: every possible signed16 value is exactly
    // representable in float32. This is not half-float or normalized short.
    const float value=float(signed_value)*(1.f/1024.f);
    Number(output,std::bit_cast<uint32_t>(value),big);
  }
}
}
bool InspectNifMeshVariants(std::span<const uint8_t> original,std::vector<NifMeshVariant>& output,std::string& error){
  try {
    Nif nif(original);std::vector<NifMeshVariant> result;
    for(uint32_t block=0;block<nif.blocks.size();++block){
      if(nif.types[nif.blocks[block].type]!="NiMesh")continue;
      const auto mesh=ReadMesh(nif,nif.blocks[block].Data());NifMeshVariant variant;
      variant.block=block;variant.submeshes=mesh.submeshes;variant.instanced=mesh.instance;variant.skin=mesh.skin;
      variant.modifier_count=mesh.modifiers;variant.embedded_skin_bytes=mesh.skin_bytes;
      if(mesh.skin){WiiCookedSkin skin;if(!ReadWiiCookedSkin(nif.blocks[block].Data().last(mesh.skin_bytes),nif.big,nif.user_version,skin,error))return false;
        variant.cooked_skin_counts={uint32_t(skin.transforms.size()),uint32_t(skin.weights.size()),uint32_t(skin.vertex_weights.size()),uint32_t(skin.morph_indices.size()),skin.morph_width,uint32_t(skin.partitions.size())};}

      for(const auto link:mesh.modifier_links)variant.modifier_types.push_back(link==UINT32_MAX?"<null>":nif.types[nif.blocks[link].type]);
      for(const auto& reference:mesh.references){
        const auto stream=ReadStream(nif,reference.stream);
        for(const auto& semantic:reference.semantics)variant.semantics.emplace_back(nif.String(semantic.name));
        variant.formats.insert(variant.formats.end(),stream.formats.begin(),stream.formats.end());
      }
      result.push_back(std::move(variant));
    }
    output=std::move(result);error.clear();return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}

bool AdaptNifMeshes(std::span<const uint8_t> original, NifMeshResult& output,
                    std::string& error) {
  try {
    Nif nif(original); NifMeshResult prepared;
    size_t budget = original.size();
    auto charge = [&](size_t bytes) {
      Check(bytes <= kMaximum - budget, "Expanded Wii mesh working set exceeds limit");
      budget += bytes;
    };
    std::map<uint32_t, Mesh> meshes;
    std::set<uint32_t> display_blocks;
    for (uint32_t i = 0; i < nif.blocks.size(); ++i) {
      const auto& type = nif.types[nif.blocks[i].type];
      if (type.starts_with("NiDataStream\x01" "4\x01")) display_blocks.insert(i);
      if (type == "NiMesh") {
        auto mesh = ReadMesh(nif, nif.blocks[i].Data());
        prepared.unadapted_skin_meshes += mesh.skin != 0;
        prepared.unadapted_modifier_meshes += mesh.modifiers != 0;
        meshes.emplace(i, std::move(mesh));
      }
    }
    // Some EM1 actors already have ordinary indexed triangles, not GX draw
    // commands. Their cooked skin is shared with the Xbox format. Validate the
    // complete index/palette/weight association before preparing those streams;
    // retaining an opaque skin flag alone is not evidence of compatibility.
    for (auto& [block, mesh] : meshes) {
      bool display=false,indexed=false;
      for(const auto& ref:mesh.references)for(const auto& semantic:ref.semantics)
        {display|=nif.String(semantic.name)=="DISPLAYLIST";indexed|=nif.String(semantic.name)=="INDEX";}
      if(!mesh.skin && !indexed)continue;
      if(display)continue; // The GX skin/matrix-index adapter is still required.
      uint16_t morph_targets=0;
      if(mesh.modifiers){Check(!mesh.skin && mesh.modifiers==1,"Indexed mesh has unsupported modifier combination");morph_targets=ReadPositionMorphTargets(nif,mesh.modifier_links[0]);}
      Check(mesh.skin<=1 && mesh.submeshes==1 && !mesh.instance &&
        U32(nif.blocks[block].Data(),mesh.primitive_at,nif.big)==0,
        "Indexed preparation requires a single noninstanced triangle mesh");
      uint32_t vertex_count=0;
      for(const auto& ref:mesh.references)for(const auto& semantic:ref.semantics)if(nif.String(semantic.name)=="POSITION"){
        const auto stream=ReadStream(nif,ref.stream);
        Check(!vertex_count && ref.regions.size()==1 && ref.regions[0]<stream.regions.size(),"Indexed mesh lacks a unique position region");
        vertex_count=stream.regions[ref.regions[0]].count;
      }
      Check(vertex_count && vertex_count<=(1u<<18),"Indexed mesh vertex count exceeds bounds");
      WiiCookedSkin skin;
      if(mesh.skin){
        if(!ReadWiiCookedSkin(nif.blocks[block].Data().last(mesh.skin_bytes),nif.big,nif.user_version,skin,error))throw std::runtime_error(error);
        Check(skin.has_weights==1 && !skin.transforms.empty() && skin.transforms.size()%16==0 &&
          skin.vertex_weights.size()==vertex_count && skin.max_partition_index<10,"Indexed skin has unsupported palette dimensions");
      }
      for(const auto& weight:skin.weights)for(unsigned i=0;i<3;++i)
        if(weight.values[i]!=0)Check(weight.bones[i]>=0 && uint32_t(weight.bones[i])<skin.transforms.size()/16,
          "Indexed skin weight references absent inverse bind matrix");
      for(const auto index:skin.morph_indices)Check(index<skin.vertex_weights.size(),"Indexed morph references absent vertex");
      std::vector<Attribute> attributes;std::vector<uint32_t> indices;
      std::vector<Reference> morph_references;std::set<uint32_t> morph_positions;bool morph_weights=false;
      uint32_t index_block=UINT32_MAX,index_name=0;bool positions=false;
      for(const auto& ref:mesh.references){
        Check(!ref.instance && ref.regions.size()==1,"Indexed skin requires one explicit stream region");
        const auto stream=ReadStream(nif,ref.stream);
        Check(ref.regions[0]<stream.regions.size() && ref.semantics.size()==stream.formats.size(),
          "Indexed skin declaration/region mismatch");
        const auto region=stream.regions[ref.regions[0]];
        Check(stream.stride && uint64_t(region.start)+region.count<=stream.buffer.size()/stream.stride,
          "Indexed skin stream region exceeds buffer");
        if(!ref.semantics.empty()){
          const auto role=nif.String(ref.semantics[0].name);
          if(role=="MORPH_POSITION" || role=="MORPHWEIGHTS"){
            Check(morph_targets!=0,"Morph stream lacks a supported modifier");
            const auto& type=nif.types[nif.blocks[ref.stream].type];
            for(size_t component=0;component<ref.semantics.size();++component){const auto& semantic=ref.semantics[component];
              Check(nif.String(semantic.name)==role,"Mixed morph reference roles");
              if(role=="MORPH_POSITION")Check(semantic.index<morph_targets && morph_positions.insert(semantic.index).second &&
                stream.formats[component]==0x00030437 && region.count==vertex_count && type.starts_with("NiDataStream\x01" "1\x01"),"Invalid morph position stream");
              else{Check(ref.semantics.size()==1 && !morph_weights && semantic.index==0 && region.count==morph_targets && stream.formats[0]==0x00010435 &&
                type.starts_with("NiDataStream\x01" "3\x01"),"Invalid morph weight stream");morph_weights=true;}
            }
            for(size_t at=size_t(region.start)*stream.stride;at<size_t(region.start+region.count)*stream.stride;at+=4)
              Check(std::isfinite(std::bit_cast<float>(U32(stream.buffer,at,nif.big))),"Nonfinite morph stream value");
            morph_references.push_back(ref);continue;
          }
        }
        for(uint32_t component=0;component<ref.semantics.size();++component){
          const auto& semantic=ref.semantics[component];const auto name=nif.String(semantic.name);
          if(name=="INDEX"){
            Check(index_block==UINT32_MAX && component==0 && ref.semantics.size()==1 && (!mesh.skin || region.start==0) &&
              nif.types[nif.blocks[ref.stream].type].starts_with("NiDataStream\x01" "0\x01") &&
              (stream.formats[0]==0x00010105 || stream.formats[0]==0x00010215 || stream.formats[0]==0x00010425),"Unsupported ordinary indexed mesh indices");
            index_block=ref.stream;index_name=semantic.name;
            Check(region.count && region.count<=(3u<<20) && region.count%3==0,"Indexed skin triangle count exceeds supported bounds");
            for(uint32_t i=0;i<region.count;++i){const auto at=(size_t(region.start)+i)*stream.stride;
              const auto index=stream.stride==1?uint32_t(stream.buffer[at]):stream.stride==2?uint32_t(U16(stream.buffer,at,nif.big)):U32(stream.buffer,at,nif.big);
              Check(index<vertex_count,"Indexed triangle references absent vertex");indices.push_back(index);}
            if(stream.stride==1 || region.start){
              Bytes values;const auto width=stream.stride==4?4u:2u;charge(indices.size()*width+128);
              for(const auto index:indices)Number(values,index,nif.big,width);
              index_block=nif.AddBlock(nif.types[nif.blocks[ref.stream].type],MakeStream(nif.big,stream.clone,region.count,
                width==4?0x00010425:0x00010215,values,stream.streamable));
            }
            continue;
          }
          Check(nif.types[nif.blocks[ref.stream].type].starts_with("NiDataStream\x01" "1\x01") &&
            region.count==vertex_count,"Indexed attribute vertex count differs");
          unsigned kind=0;
          if(name=="POSITION"){Check(!positions && semantic.index==0 && stream.formats[component]==0x00030437,
              "Unsupported indexed skin position declaration");positions=true;}
          else if(name=="NORMAL"){kind=1;Check(semantic.index==0 && (stream.formats[component]==0x00030437 ||
              (!mesh.skin && stream.formats[component]==0x0004010c)),"Unsupported indexed normal declaration");}
          else if(name=="COLOR"){kind=2;Check(semantic.index<2,"Unsupported indexed skin color channel");}
          else if(name=="TEXCOORD"){kind=3;Check(semantic.index<8 && (stream.formats[component]==0x00020240 ||
              stream.formats[component]==0x00020436 || stream.formats[component]==0x00020232),"Unsupported indexed skin UV declaration");}
          else throw std::runtime_error("Unaudited indexed skin attribute");
          attributes.push_back({kind,semantic.name,semantic.index,ref.stream,component,region,stream});
        }
      }
      Check(positions && index_block!=UINT32_MAX,"Indexed mesh lacks complete triangle mapping");
      if(morph_targets)Check(morph_positions.size()==morph_targets && morph_weights,"Morph modifier lacks complete target/weight streams");
      if(mesh.skin)Check(skin.mapped_primitives.size()==indices.size() && !skin.partitions.empty(),"Indexed skin lacks complete palette mapping");
      uint32_t end=0;
      for(const auto& partition:skin.partitions){
        Check(partition.start>=0 && uint32_t(partition.start)==end && partition.end>partition.start &&
          uint32_t(partition.end)<=indices.size() && partition.start%3==0 && partition.end%3==0,
          "Indexed skin partitions do not cover consecutive complete triangles");
        for(uint32_t i=uint32_t(partition.start);i<uint32_t(partition.end);++i){
          const auto slot=skin.mapped_primitives[i];Check(slot<=skin.max_partition_index && slot<partition.weights.size() &&
            partition.weights[slot]==skin.vertex_weights[indices[i]],"Indexed skin palette disagrees with vertex weights");
        }
        end=uint32_t(partition.end);
      }
      if(mesh.skin)Check(end==indices.size(),"Indexed skin partitions leave triangles unmapped");
      std::set<std::pair<uint32_t,uint32_t>> semantics;
      for(const auto& attribute:attributes)Check(semantics.emplace(attribute.kind,attribute.index).second,"Duplicate indexed skin attribute");
      MeshVertexOrigins origins;origins.mesh_block=block;origins.topology.attributes=uint32_t(attributes.size());
      origins.topology.triangle_indices=indices;
      charge(indices.size()*4+vertex_count*attributes.size()*4+512);
      Bytes references;Number(references,uint32_t(attributes.size()+1+morph_references.size()),nif.big);
      WriteReference(references,nif.big,index_block,index_name,0);
      for(const auto& attribute:attributes){
        const auto& stream=attribute.stream;const auto format=stream.formats[attribute.component];
        const auto width=stream.widths[attribute.component];const bool fixed=attribute.kind==3 && format==0x00020240;
        Bytes values;charge(vertex_count*(fixed?8:width)+128);
        for(uint32_t v=0;v<vertex_count;++v){
          const auto source=stream.buffer.subspan((size_t(attribute.region.start)+v)*stream.stride+stream.offsets[attribute.component],width);
          if(fixed)FixedUv(values,source,nif.big);else Append(values,source);
        }
        const auto added=nif.AddBlock(nif.types[nif.blocks[attribute.block].type],MakeStream(nif.big,stream.clone,
          vertex_count,fixed?0x00020436:format,values,stream.streamable));
        WriteReference(references,nif.big,added,attribute.name,attribute.index);prepared.promoted_fixed_uv_streams+=fixed;
        origins.attributes.push_back({attribute.block,attribute.region.start,attribute.component,std::string(nif.String(attribute.name)),attribute.index});
      }
      for(uint32_t v=0;v<vertex_count;++v)for(size_t a=0;a<attributes.size();++a)origins.topology.attribute_indices.push_back(v);
      for(const auto& ref:morph_references){
        // Validated one-region references; retain original stream
        // IDs and complete payloads so native modifier binding remains intact.
        Number(references,ref.stream,nif.big);Number(references,ref.instance,nif.big,1);
        Number(references,1,nif.big,2);Number(references,ref.regions[0],nif.big,2);
        Number(references,uint32_t(ref.semantics.size()),nif.big);
        for(const auto& semantic:ref.semantics){Number(references,semantic.name,nif.big);Number(references,semantic.index,nif.big);}
      }
      const auto original_mesh=nif.blocks[block].original;Bytes replacement;
      Append(replacement,original_mesh.first(mesh.references_at));Append(replacement,references);
      Append(replacement,original_mesh.subspan(mesh.tail_at)); // Exact original bones, morphs and palette; no static fallback.
      nif.blocks[block].replacement=std::move(replacement);prepared.meshes.push_back(std::move(origins));
      if(mesh.skin){--prepared.unadapted_skin_meshes;++prepared.prepared_indexed_skin_meshes;}
      else ++prepared.prepared_indexed_static_meshes;
      if(morph_targets){--prepared.unadapted_modifier_meshes;++prepared.prepared_indexed_morph_meshes;}
    }
    if (display_blocks.empty()) {
      if(prepared.prepared_indexed_skin_meshes || prepared.prepared_indexed_static_meshes)prepared.bytes=nif.Write();else prepared.bytes.assign(original.begin(),original.end());
      output = std::move(prepared); error.clear(); return true;
    }
    std::set<uint32_t> converted_displays;
    for (auto& [block, mesh] : meshes) {
      bool has_display = false;
      for (const auto& ref : mesh.references) for (const auto& semantic : ref.semantics)
        has_display |= nif.String(semantic.name) == "DISPLAYLIST";
      if (!has_display) continue;
      Check(mesh.submeshes == 1 && !mesh.instance && !mesh.skin && mesh.modifiers<=1,
            "Wii mesh requires skin/morph/instance/submesh adapter");
      const auto morph_targets=mesh.modifiers?ReadPositionMorphTargets(nif,mesh.modifier_links[0]):0;
      std::vector<Reference> morph_references;std::set<uint32_t> morph_positions;bool morph_weights=false;
      uint32_t display_block = UINT32_MAX, display_clone = 0;
      View commands;
      std::vector<uint32_t> position_triangles;
      std::vector<Attribute> attributes;
      for (const auto& ref : mesh.references) {
        Check(!ref.instance && ref.regions.size() <= 1, "Unsupported Wii stream instance/region layout");
        auto stream = ReadStream(nif, ref.stream);
        const auto region_index = ref.regions.empty() ? 0 : ref.regions[0];
        Check(region_index < stream.regions.size(), "Missing Wii stream region");
        const auto region = stream.regions[region_index];
        if(!ref.semantics.empty()){
          const auto role=nif.String(ref.semantics[0].name);
          if(role=="MORPH_POSITION" || role=="MORPHWEIGHTS"){
            Check(morph_targets && ref.regions.size()==1 && ref.semantics.size()==stream.formats.size() && stream.stride &&
                  uint64_t(region.start)+region.count<=stream.buffer.size()/stream.stride,"Invalid GX morph declaration/region");
            const auto& type=nif.types[nif.blocks[ref.stream].type];
            for(size_t component=0;component<ref.semantics.size();++component){const auto& semantic=ref.semantics[component];
              Check(nif.String(semantic.name)==role,"Mixed GX morph reference roles");
              if(role=="MORPH_POSITION")Check(semantic.index<uint32_t(morph_targets) && morph_positions.insert(semantic.index).second &&
                  stream.formats[component]==0x00030437 && type.starts_with("NiDataStream\x01" "1\x01"),"Unsupported GX morph position stream");
              else{Check(ref.semantics.size()==1 && !morph_weights && semantic.index==0 && region.count==uint32_t(morph_targets) &&
                  stream.formats[0]==0x00010435 && type.starts_with("NiDataStream\x01" "3\x01"),"Unsupported GX morph weights");morph_weights=true;}
            }
            for(size_t at=size_t(region.start)*stream.stride;at<size_t(region.start+region.count)*stream.stride;at+=4)
              Check(std::isfinite(std::bit_cast<float>(U32(stream.buffer,at,nif.big))),"Nonfinite GX morph value");
            morph_references.push_back(ref);continue;
          }
        }
        for (uint32_t component = 0; component < ref.semantics.size(); ++component) {
          const auto& semantic = ref.semantics[component]; const auto name = nif.String(semantic.name);
          if (name == "DISPLAYLIST") {
            Check(display_block == UINT32_MAX && display_blocks.contains(ref.stream) &&
                  region.start <= stream.buffer.size() && region.count <= stream.buffer.size() - region.start,
                  "Invalid or duplicate Wii display-list stream");
            display_block = ref.stream; display_clone = stream.clone;
            commands = stream.buffer.subspan(region.start, region.count); continue;
          }
          if (name == "INDEX") {
            // The original CPU triangle list disambiguates GX byte streams
            // that accidentally parse at both widths. It is never replaced
            // by a guessed vertex-count threshold or a preferred width.
            if(U32(nif.blocks[block].Data(),mesh.primitive_at,nif.big)==0 &&
               ref.semantics.size()==1 && component==0 && stream.formats.size()==1 &&
               nif.types[nif.blocks[ref.stream].type].starts_with("NiDataStream\x01" "0\x01") &&
               (stream.formats[0]==0x00010105 || stream.formats[0]==0x00010215 || stream.formats[0]==0x00010425)){
              Check(position_triangles.empty() && region.count && region.count<=(3u<<20) && region.count%3==0 && stream.stride &&
                uint64_t(region.start)+region.count<=stream.buffer.size()/stream.stride,"Invalid GX source triangle-list witness");
              charge(size_t(region.count)*4);
              for(uint32_t i=0;i<region.count;++i){const auto at=(size_t(region.start)+i)*stream.stride;
                position_triangles.push_back(stream.stride==1?uint32_t(stream.buffer[at]):stream.stride==2?uint32_t(U16(stream.buffer,at,nif.big)):U32(stream.buffer,at,nif.big));}
            }
            continue;
          }
          unsigned kind;
          if (name == "POSITION") kind = 0;
          else if (name == "NORMAL") kind = 1;
          else if (name == "COLOR") kind = 2;
          else if (name == "TEXCOORD") kind = 3;
          else throw std::runtime_error("Unsupported static Wii vertex semantic");
          Check(component < stream.formats.size() && stream.stride &&
                uint64_t(region.start) + region.count <= stream.buffer.size() / stream.stride,
                "Invalid Wii vertex region");
          Check(nif.types[nif.blocks[ref.stream].type].starts_with("NiDataStream\x01" "1\x01"),
                "Unsupported Wii attribute stream usage");
          Check((kind < 2 && semantic.index == 0) || (kind == 2 && semantic.index < 2) ||
                (kind == 3 && semantic.index < 8), "Unsupported GX attribute semantic index");
          attributes.push_back({kind, semantic.name, semantic.index, ref.stream, component, region, stream});
        }
      }
      Check(display_block != UINT32_MAX && converted_displays.insert(display_block).second,
            "Shared Wii display list requires an additional ownership adapter");
      std::sort(attributes.begin(), attributes.end(), [](const auto& a, const auto& b) {
        return std::pair(a.kind,a.index) < std::pair(b.kind,b.index);
      });
      Check(!attributes.empty() && attributes.front().kind == 0, "Missing Wii position attribute");
      Check(!morph_targets || (morph_positions.size()==size_t(morph_targets) && morph_weights),"Incomplete GX morph target/weight bindings");
      std::vector<uint32_t> counts;
      for (size_t i = 0; i < attributes.size(); ++i) {
        if (i) Check(attributes[i].kind != attributes[i-1].kind || attributes[i].index != attributes[i-1].index,
                     "Duplicate GX vertex semantic");
        counts.push_back(attributes[i].region.count);
      }
      MeshVertexOrigins origins; origins.mesh_block = block;
      if (!DecodeGxMesh(commands, counts, origins.topology, error,position_triangles)) throw std::runtime_error(error);
      charge((origins.topology.attribute_indices.size() + origins.topology.triangle_indices.size()) * 4 + 512);
      const auto vertices = uint32_t(origins.topology.attribute_indices.size() / attributes.size());
      Check(vertices > 0, "Empty converted Wii mesh");
      const bool wide = vertices > 65536;
      charge(origins.topology.triangle_indices.size() * (wide ? 4 : 2) + 64);
      Bytes indices;
      for (const auto index : origins.topology.triangle_indices) Number(indices, index, nif.big, wide ? 4 : 2);
      auto new_index = MakeStream(nif.big, display_clone, uint32_t(origins.topology.triangle_indices.size()),
                                  wide ? 0x00010425 : 0x00010215, indices, 1);
      nif.blocks[display_block].type = nif.Type("NiDataStream\x01" "0\x01" "18");
      nif.blocks[display_block].replacement = std::move(new_index);
      Bytes references; Number(references, uint32_t(attributes.size()+1+(morph_targets?morph_targets+1:0)), nif.big);
      WriteReference(references, nif.big, display_block, nif.AddString("INDEX"), 0);
      for (size_t a = 0; a < attributes.size(); ++a) {
        const auto& attribute = attributes[a]; const auto& stream = attribute.stream;
        const auto width = stream.widths[attribute.component],format=stream.formats[attribute.component];
        const bool fixed_uv=attribute.kind==3 && format==0x00020240;
        if(attribute.kind==3)Check(fixed_uv || format==0x00020436 || format==0x00020232,
          "Unaudited Wii TEXCOORD format requires a native vertex-declaration adapter");
        const auto target_width=fixed_uv?8u:width,target_format=fixed_uv?0x00020436u:format;Bytes values;
        Check(uint64_t(vertices) * target_width <= kMaximum, "Converted vertex stream exceeds limit");
        charge(size_t(vertices) * target_width + 128);
        values.reserve(size_t(vertices) * target_width);
        for (uint32_t v = 0; v < vertices; ++v) {
          const auto source_index = origins.topology.attribute_indices[v*attributes.size()+a];
          const size_t offset = (size_t(attribute.region.start) + source_index) * stream.stride + stream.offsets[attribute.component];
          const auto source=stream.buffer.subspan(offset,width);
          if(fixed_uv)FixedUv(values,source,nif.big);else Append(values,source);
        }
        const auto type = nif.types[nif.blocks[attribute.block].type];
        const auto new_block = nif.AddBlock(type, MakeStream(nif.big, stream.clone, vertices,
            target_format, values, stream.streamable));
        prepared.promoted_fixed_uv_streams+=fixed_uv;
        WriteReference(references, nif.big, new_block, attribute.name, attribute.index);
        origins.attributes.push_back({attribute.block,attribute.region.start,attribute.component,
                                      std::string(nif.String(attribute.name)),attribute.index});
      }
      for(const auto& ref:morph_references){
        const auto stream=ReadStream(nif,ref.stream);const auto region=stream.regions[ref.regions[0]];
        if(nif.String(ref.semantics[0].name)=="MORPHWEIGHTS"){
          // Weights address targets, not vertices: retain their writable stream.
          Number(references,ref.stream,nif.big);Number(references,ref.instance,nif.big,1);
          Number(references,1,nif.big,2);Number(references,ref.regions[0],nif.big,2);Number(references,1,nif.big);
          Number(references,ref.semantics[0].name,nif.big);Number(references,ref.semantics[0].index,nif.big);continue;
        }
        Check(region.count==attributes.front().region.count,"GX morph target count differs from source positions");
        for(size_t component=0;component<ref.semantics.size();++component){
          Bytes values;charge(size_t(vertices)*12+128);values.reserve(size_t(vertices)*12);
          // GX splits vertices at UV/normal/colour seams. Expand every target
          // using the position index, preserving the exact deformation there.
          for(uint32_t v=0;v<vertices;++v){
            const auto position=origins.topology.attribute_indices[v*attributes.size()];
            Check(position<region.count,"GX morph source position is outside its target");
            const auto at=(size_t(region.start)+position)*stream.stride+stream.offsets[component];Append(values,stream.buffer.subspan(at,12));
          }
          const auto target=nif.AddBlock(nif.types[nif.blocks[ref.stream].type],MakeStream(nif.big,stream.clone,vertices,0x00030437,values,stream.streamable));
          WriteReference(references,nif.big,target,ref.semantics[component].name,ref.semantics[component].index);
        }
      }
      // Preserve original materials, transforms, modifier and skin tail.
      // Only primitive topology and stream references change.
      const auto source = nif.blocks[block].original; Bytes replacement;
      Append(replacement, source.first(mesh.primitive_at)); Number(replacement, 0, nif.big);
      Append(replacement, source.subspan(mesh.primitive_at+4, mesh.references_at-mesh.primitive_at-4));
      Append(replacement, references); Append(replacement, source.subspan(mesh.tail_at));
      nif.blocks[block].replacement = std::move(replacement);
      prepared.meshes.push_back(std::move(origins));
      if(morph_targets){--prepared.unadapted_modifier_meshes;++prepared.prepared_gx_morph_meshes;}
    }
    Check(converted_displays == display_blocks, "Unowned Wii display-list blocks require an additional adapter");
    prepared.bytes = nif.Write();
    output = std::move(prepared); error.clear(); return true;
  } catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool TestNifFixedPointUvPromotion(std::string& error) {
  error.clear();try {
    {
      // Both widths parse: six byte tuples consume half the payload and see
      // NOP padding; the real six ushort tuples contain one oriented triangle.
      const std::vector<uint8_t> commands{0x90,0,6,0,0,0,0,0,1,0,1,0,2,0,2,0,0,0,0,0,0,0,0,0,0,0,0};
      const std::array<uint32_t,2> counts{3,3};
      const std::array<uint32_t,3> faces{0,1,2},reversed{0,2,1},invalid{0,1,3};
      GxMeshTopology topology;std::string reason;
      Check(!DecodeGxMesh(commands,counts,topology,reason),"Ambiguous GX stream accepted without source geometry");
      Check(DecodeGxMesh(commands,counts,topology,reason,faces) && topology.source_index_bytes==2 &&
        topology.triangle_indices==std::vector<uint32_t>{0,1,2},"Original oriented geometry did not disambiguate GX width");
      const std::array<uint32_t,3> cyclic{1,2,0};
      const std::array<uint32_t,6> duplicate{0,1,2,0,1,2};
      Check(DecodeGxMesh(commands,counts,topology,reason,cyclic),"Cyclic rotation changed GX face identity");
      const auto saved=topology.triangle_indices;
      Check(!DecodeGxMesh(commands,counts,topology,reason,reversed) && topology.triangle_indices==saved,"GX winding mismatch accepted or failure mutated output");
      Check(!DecodeGxMesh(commands,counts,topology,reason,duplicate),"GX face multiplicity mismatch accepted");
      Check(!DecodeGxMesh(commands,counts,topology,reason,invalid),"Out-of-region source geometry accepted");
    }
    // Exhaustive value coverage, both stream byte orders; unlike /1023
    // normalization, fixed6.10 must preserve values beyond [0,1] and -32.
    for(bool big:{false,true})for(int value=-32768;value<=32767;++value){
      Bytes source,converted;Number(source,uint16_t(value),big,2);Number(source,uint16_t(-1),big,2);
      FixedUv(converted,source,big);
      Check(converted.size()==8 && std::bit_cast<float>(U32(converted,0,big))==float(double(value)/1024.) &&
          std::bit_cast<float>(U32(converted,4,big))==-1.f/1024.f,"Fixed6.10 exhaustive boundary/endian proof failed");
    }
    for(bool big:{false,true}) {
      // End-to-end fixture includes a nonzero source region, an interleaved
      // preceding component, TEXCOORD1 and signed/extreme/repeating UVs.
      auto stream=[&](std::span<const uint32_t> formats,View data){Bytes b;Number(b,uint32_t(data.size()),big);Number(b,0,big);
        Number(b,1,big);Number(b,1,big);Number(b,3,big);Number(b,uint32_t(formats.size()),big);
        for(auto f:formats)Number(b,f,big);Append(b,data);Number(b,1,big,1);return b;};
      Bytes positions;for(unsigned i=0;i<12;++i)Number(positions,std::bit_cast<uint32_t>(float(i)),big);
      const std::array<std::array<int16_t,2>,4> raw={{{9,9},{-32768,32767},{1023,1024},{-1,0}}};Bytes uvs;
      for(const auto& v:raw){Number(uvs,0xbeef,big,2);for(auto f:v)Number(uvs,std::bit_cast<uint16_t>(f),big,2);}
      const std::array<uint32_t,1> pf={0x00030437};const std::array<uint32_t,2> uf={0x00010215,0x00020240};
      auto pstream=stream(pf,positions),ustream=stream(uf,uvs);
      const std::array<uint8_t,9> commands={0x90,0,3,0,0,1,1,2,2};
      Bytes display;Number(display,uint32_t(commands.size()),big);Number(display,0,big);Number(display,1,big);Number(display,0,big);
      Number(display,uint32_t(commands.size()),big);Number(display,1,big);Number(display,0x00010105,big);Append(display,commands);Number(display,1,big,1);
      Bytes mesh;Number(mesh,UINT32_MAX,big);Number(mesh,0,big);Number(mesh,UINT32_MAX,big);Number(mesh,0,big,2);
      for(unsigned i=0;i<13;++i)Number(mesh,0,big);Number(mesh,0,big);Number(mesh,UINT32_MAX,big);
      Number(mesh,0,big);Number(mesh,0,big);Number(mesh,0,big,1);Number(mesh,0,big);Number(mesh,UINT32_MAX,big);Number(mesh,0,big);
      Number(mesh,0,big);Number(mesh,1,big,2);Number(mesh,0,big,1);for(unsigned i=0;i<4;++i)Number(mesh,0,big);
      Number(mesh,3,big);
      WriteReference(mesh,big,1,0,0);
      Number(mesh,2,big);Number(mesh,0,big,1);Number(mesh,1,big,2);Number(mesh,0,big,2);Number(mesh,2,big);
      Number(mesh,1,big);Number(mesh,0,big);Number(mesh,2,big);Number(mesh,1,big);
      WriteReference(mesh,big,3,3,0);Number(mesh,0,big);Number(mesh,0,big,1);
      auto fixture=[&](const Bytes& uv){Bytes file;constexpr std::string_view magic="Gamebryo File Format, Version 20.6.5.0\n";
        Append(file,View(reinterpret_cast<const uint8_t*>(magic.data()),magic.size()));Number(file,0x14060500,false);Number(file,big?0:1,false,1);Number(file,15,false);Number(file,4,false);
        const std::array<std::string_view,3> types={"NiMesh","NiDataStream\x01" "1\x01" "17","NiDataStream\x01" "4\x01" "17"};Number(file,3,big,2);
        for(auto s:types){Number(file,uint32_t(s.size()),big);Append(file,View(reinterpret_cast<const uint8_t*>(s.data()),s.size()));}
        for(auto i:{0,1,1,2})Number(file,i,big,2);for(auto size:{mesh.size(),pstream.size(),uv.size(),display.size()})Number(file,uint32_t(size),big);
        Number(file,4,big);Number(file,11,big);for(std::string_view s:{"POSITION","INDEX","TEXCOORD","DISPLAYLIST"}){Number(file,uint32_t(s.size()),big);Append(file,View(reinterpret_cast<const uint8_t*>(s.data()),s.size()));}Number(file,0,big);
        Append(file,mesh);Append(file,pstream);Append(file,uv);Append(file,display);Number(file,1,big);Number(file,0,big);return file;};
      auto source=fixture(ustream);NifMeshResult converted;std::string reason;
      Check(AdaptNifMeshes(source,converted,reason),reason.c_str());Check(converted.promoted_fixed_uv_streams==1,"Fixed UV conversion path was not used");
      Nif target(converted.bytes);auto parsed=ReadMesh(target,target.blocks[0].Data());bool found=false;
      for(const auto& ref:parsed.references)for(const auto& semantic:ref.semantics)if(target.String(semantic.name)=="TEXCOORD"){
        Check(semantic.index==1,"Secondary UV semantic index changed");const auto values=ReadStream(target,ref.stream);
        Check(values.formats==std::vector<uint32_t>{0x00020436} && values.stride==8 && values.buffer.size()==24,"Promoted UV declaration/stride differs");
        for(unsigned i=0;i<3;++i)for(unsigned k=0;k<2;++k)Check(std::bit_cast<float>(U32(values.buffer,i*8+k*4,big))==float(raw[i+1][k])/1024.f,"Region/interleaved UV source selection changed");found=true;
      }
      Check(found && target.blocks[2].Data().size()==ustream.size() && std::equal(ustream.begin(),ustream.end(),target.blocks[2].Data().begin()),"Original UV provenance bytes changed");
      // Morph targets follow POSITION indices when GX expands a UV seam.
      {
        Nif morph(source);const auto input=ReadMesh(morph,morph.blocks[0].Data());
        const auto morph_name=morph.AddString("MORPH_POSITION"),weights_name=morph.AddString("MORPHWEIGHTS");
        Bytes targets;for(unsigned v=0;v<4;++v)for(unsigned t=0;t<2;++t)for(unsigned k=0;k<3;++k)
          Number(targets,std::bit_cast<uint32_t>(float(t*1000+v*100+k)),big);
        const std::array<uint32_t,2> formats={0x00030437,0x00030437};
        const auto target_link=morph.AddBlock("NiDataStream\x01" "1\x01" "3",stream(formats,targets));
        Bytes weights;Number(weights,std::bit_cast<uint32_t>(.25f),big);Number(weights,std::bit_cast<uint32_t>(.75f),big);
        const auto weights_link=morph.AddBlock("NiDataStream\x01" "3\x01" "5",MakeStream(big,0,2,0x00010435,weights,1));
        Bytes modifier;Number(modifier,1,big);Number(modifier,0x8030,big,2);Number(modifier,1,big);Number(modifier,0x8040,big,2);
        Number(modifier,1,big,1);Number(modifier,2,big,2);Number(modifier,1,big);Number(modifier,0,big);Number(modifier,0,big);Number(modifier,0,big);
        const auto modifier_link=morph.AddBlock("NiMorphMeshModifier",modifier);
        const auto original=morph.blocks[0].Data();Bytes modified;
        Append(modified,original.first(input.references_at));Number(modified,5,big);
        Append(modified,original.subspan(input.references_at+4,input.tail_at-input.references_at-4));
        Number(modified,target_link,big);Number(modified,0,big,1);Number(modified,1,big,2);Number(modified,0,big,2);Number(modified,2,big);
        for(unsigned t=0;t<2;++t){Number(modified,morph_name,big);Number(modified,t,big);}
        WriteReference(modified,big,weights_link,weights_name,0);Number(modified,1,big);Number(modified,modifier_link,big);Number(modified,0,big,1);
        morph.blocks[0].replacement=std::move(modified);
        const std::array<uint8_t,15> seams={0x90,0,6,0,0,1,1,2,2,0,2,2,0,1,1};
        morph.blocks[3].replacement=MakeStream(big,0,uint32_t(seams.size()),0x00010105,seams,1);
        const auto encoded=morph.Write();NifMeshResult result;
        Check(AdaptNifMeshes(encoded,result,reason),reason.c_str());
        Check(result.prepared_gx_morph_meshes==1 && !result.unadapted_modifier_meshes,"GX morph modifier was not prepared");
        Nif rebuilt(result.bytes);const auto ready=ReadMesh(rebuilt,rebuilt.blocks[0].Data());
        Check(ready.modifiers==1 && rebuilt.blocks[modifier_link].Data().size()==modifier.size() &&
          std::equal(modifier.begin(),modifier.end(),rebuilt.blocks[modifier_link].Data().begin()),"Original morph modifier changed");
        const auto& topology=result.meshes.at(0).topology;const size_t count=topology.attribute_indices.size()/2;
        Check(count>3,"GX morph fixture did not produce split position vertices");
        unsigned checked_targets=0;bool checked_weights=false;
        for(const auto& ref:ready.references)for(const auto& semantic:ref.semantics){
          const auto role=rebuilt.String(semantic.name);const auto values=ReadStream(rebuilt,ref.stream);
          if(role=="MORPH_POSITION"){
            Check(values.regions.at(0).count==count && values.stride==12,"Expanded morph target vertex layout differs");
            for(size_t v=0;v<count;++v)for(unsigned k=0;k<3;++k)
              Check(std::bit_cast<float>(U32(values.buffer,v*12+k*4,big))==float(semantic.index*1000+(topology.attribute_indices[v*2]+1)*100+k),"Morph target used UV/normal index instead of source position");
            ++checked_targets;
          }else if(role=="MORPHWEIGHTS"){
            Check(ref.stream==weights_link && values.buffer.size()==weights.size() &&
              std::equal(weights.begin(),weights.end(),values.buffer.begin()),"Morph target weights were altered/expanded");checked_weights=true;
          }
        }
        Check(checked_targets==2 && checked_weights,"Expanded GX morph lost target/weight bindings");
      }
      auto unsupported=ustream;const size_t format_at=12+8+4+4;for(unsigned i=0;i<4;++i)unsupported[format_at+i]=uint8_t(0x00020241u>>(8*(big?3-i:i)));
      NifMeshResult rejected;Check(!AdaptNifMeshes(fixture(unsupported),rejected,reason),"Unsupported fixed UV declaration accepted");
    }
    return true;
  }catch(const std::exception& exception){error=exception.what();return false;}
}
}

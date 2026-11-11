#include "em1_scene_adapter.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <map>

namespace em2::em1 {
namespace {
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> data) : data_(data) {}
  uint32_t Word(size_t at) const {
    Require(at <= data_.size() && data_.size() - at >= 4, "Truncated scene word");
    return (uint32_t(data_[at]) << 24) | (uint32_t(data_[at + 1]) << 16) |
           (uint32_t(data_[at + 2]) << 8) | data_[at + 3];
  }
  std::string_view String(uint32_t at, size_t string_end) const {
    Require(at >= 4 && at < string_end && string_end - at > 2,
            "Scene string pointer outside string table");
    auto first = data_.begin() + at + 2;
    auto last = std::find(first, data_.begin() + string_end, uint8_t(0));
    Require(last != data_.begin() + string_end, "Unterminated scene string");
    return {reinterpret_cast<const char*>(&*first), size_t(last - first)};
  }
  void Advance(size_t& at, size_t bytes) const {
    Require(at <= data_.size() && bytes <= data_.size() - at, "Truncated scene payload");
    at += bytes;
  }
  static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }
 private:
  std::span<const uint8_t> data_;
};
void Word(std::vector<uint8_t>& output, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) output.push_back(uint8_t(value >> shift));
}
size_t PropertySize(std::string_view type) {
  if (type == "Boolean" || type == "Integer" || type == "Unsigned Integer" ||
      type == "Short" || type == "Unsigned Short" || type == "Float" ||
      type == "String" || type == "Entity Pointer") return 4;
  if (type == "Point2") return 8;
  if (type == "Point3" || type == "Color (RGB)") return 12;
  if (type == "Color (RGBA)") return 16;
  if (type == "Matrix3") return 36;
  throw std::runtime_error("Unsupported EM1 scene property type");
}
}  // namespace

bool AdaptScene(std::span<const uint8_t> source, SceneAdapterResult& result,
                std::string& error) {
  try {
    Reader::Require(source.size() >= 28 && source.size() <= 64 * 1024 * 1024,
                    "EM1 scene outside supported size range");
    Reader reader(source);
    const auto end = reader.Word(0);
    Reader::Require(end != 0x01000001 && end >= 4 && end <= source.size() - 24,
                    "Not a supported EM1 scene header");
    SceneAdapterResult converted;
    std::copy_n(source.begin() + end, 16, converted.source_uuid.begin());
    converted.entity_count = reader.Word(end + 16);
    const auto references = reader.Word(end + 20);
    Reader::Require(converted.entity_count <= source.size() / 20 &&
                    references <= source.size() / 4, "Invalid scene counts");
    std::vector<uint8_t> output,strings;
    std::map<std::string,uint32_t> added_strings;
    auto intern=[&](std::string_view text){
      const std::string key(text);if(auto found=added_strings.find(key);found!=added_strings.end())return found->second;
      const auto offset=size_t(end)+strings.size(),length=text.size()+1,block=(length+5)&~size_t(3);
      Reader::Require(offset<=UINT32_MAX && block<=255 && length<=255,"Compatibility string exceeds scene encoding");
      const auto begin=strings.size();strings.push_back(uint8_t(block));strings.push_back(uint8_t(length));
      strings.insert(strings.end(),text.begin(),text.end());strings.resize(begin+block,0);
      added_strings.emplace(key,uint32_t(offset));return uint32_t(offset);
    };
    output.reserve(source.size() + size_t(converted.entity_count) * 4);
    Word(output, 0x02000002); Word(output, 0);
    Word(output, converted.entity_count); Word(output, references);
    size_t position = size_t(end) + 24;
    for (uint32_t entity = 0; entity < converted.entity_count; ++entity) {
      const auto begin = position;
      reader.String(reader.Word(position), end);
      const auto components = reader.Word(position + 16);
      reader.Advance(position, 20);
      Reader::Require(components <= source.size() / 20 &&
                      components <= std::numeric_limits<uint32_t>::max() - converted.component_count,
                      "Invalid component count");
      converted.component_count += components;
      output.insert(output.end(),source.begin()+begin,source.begin()+begin+16);
      Word(output,0);Word(output,components);
      for (uint32_t component = 0; component < components; ++component) {
        const auto component_begin=position;
        const auto component_name=reader.String(reader.Word(position), end);
        reader.String(reader.Word(position + 4), end);
        const auto properties = reader.Word(position + 16);
        reader.Advance(position, 20);
        Reader::Require(properties <= source.size() / 16, "Invalid property count");
        bool has_instigator=false,has_output=false;size_t output_count=0;uint32_t string_type=0;
        for (uint32_t property = 0; property < properties; ++property) {
          const auto name=reader.String(reader.Word(position), end);
          const auto type = reader.String(reader.Word(position + 4), end);
          const auto type_offset=reader.Word(position+4);
          const auto mode = reader.Word(position + 8);
          const auto amount = reader.Word(position + 12);
          reader.Advance(position, 16);
          Reader::Require(mode < 6, "Unsupported scene property storage mode");
          const size_t count = mode % 2 ? amount : 1;
          if(component_name=="InputOutputComponent"){
            if(name=="Instigator")has_instigator=true;
            if(name=="Output"){
              Reader::Require(!has_output && type=="String","Original IO Output must be a unique String property");
              has_output=true;output_count=count;string_type=type_offset;
            }
          }
          const auto stride = PropertySize(type);
          Reader::Require(count <= (source.size() - position) / stride,
                          "Scene property exceeds remaining source");
          if (type == "String") {
            for (size_t i = 0; i < count; ++i) reader.String(reader.Word(position + i * 4), end);
          }
          reader.Advance(position, count * stride);
        }
        // EM1 subscribes each authored IO output on its own entity. EM2 adds
        // Instigator: missing/empty defaults instead enter the named-source
        // map, so they never create those local listeners. Supply its native
        // `_self` spelling only for original layers that own actual rows.
        // Empty/absent Output layers inherit their master's generated list;
        // explicit Instigator values, including empty arrays, remain authored.
        const bool add_instigator=component_name=="InputOutputComponent" && has_output && output_count && !has_instigator;
        Reader::Require(!add_instigator || properties<UINT32_MAX,"IO property count overflow");
        output.insert(output.end(),source.begin()+component_begin,source.begin()+component_begin+16);
        Word(output,properties+uint32_t(add_instigator));
        output.insert(output.end(),source.begin()+component_begin+20,source.begin()+position);
        if(add_instigator){
          Word(output,intern("Instigator"));Word(output,string_type);Word(output,1);Word(output,uint32_t(output_count));
          const auto self=intern("_self");for(size_t row=0;row<output_count;++row)Word(output,self);
        }
      }
    }
    Reader::Require(size_t(references) * 4 == source.size() - position,
                    "Trailing or truncated scene references");
    output.insert(output.end(), source.begin() + position, source.end());
    auto& bytes=converted.bytes;
    Reader::Require(strings.size()<=UINT32_MAX-end,"Compatibility string table overflow");
    Word(bytes,0x01000001);Word(bytes,end+uint32_t(strings.size()));
    bytes.insert(bytes.end(),source.begin()+4,source.begin()+end);
    bytes.insert(bytes.end(),strings.begin(),strings.end());bytes.insert(bytes.end(),output.begin(),output.end());
    Reader::Require(bytes.size()<=64*1024*1024,"Adapted scene exceeds native input size limit");
    result = std::move(converted);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}
bool TestSceneAdapterIOCompatibility(){
  try{
    // Verify original bytes and inheritance independently of native dispatch:
    // parent owns two rows; a child owns no rows and must not shadow its list.
    for(unsigned variant=0;variant<5;++variant){
      std::vector<uint8_t> table,body,original_properties;std::map<std::string,uint32_t> keys;
      auto text=[&](std::string_view value){
        const std::string key(value);if(auto found=keys.find(key);found!=keys.end())return found->second;
        const auto offset=uint32_t(4+table.size());
        const auto start=table.size(),length=value.size()+1,block=(length+5)&~size_t(3);
        table.push_back(uint8_t(block));table.push_back(uint8_t(length));table.insert(table.end(),value.begin(),value.end());table.resize(start+block,0);
        keys.emplace(key,offset);return offset;
      };
      const auto owner=text("Owner"),klass=text("InputOutputComponent"),version=text("1,0,0,0"),output=text("Output"),string=text("String"),
        onload=text("OnMapLoadOrStream"),onuse=text("OnUse"),flags=text("IoFlags"),short_type=text("Unsigned Short"),
        instigator=text("Instigator"),player=text("_player");
      const bool explicit_field=variant==1 || variant==2,scalar=variant==3,non_io=variant==4;
      const auto used_class=non_io?text("OtherComponent"):klass;
      const uint32_t count=scalar?1:2;
      Word(original_properties,output);Word(original_properties,string);Word(original_properties,scalar?0:1);Word(original_properties,count);
      Word(original_properties,onload);if(!scalar)Word(original_properties,onuse);
      Word(original_properties,flags);Word(original_properties,short_type);Word(original_properties,1);Word(original_properties,count);
      Word(original_properties,0x00010000);if(!scalar)Word(original_properties,0x00030000);
      if(explicit_field){
        Word(original_properties,instigator);Word(original_properties,string);Word(original_properties,1);Word(original_properties,variant==1?1:0);
        if(variant==1)Word(original_properties,player);
      }
      auto entity=[&](uint32_t id,uint32_t parent,uint32_t component,uint32_t master,bool empty){
        Word(body,owner);Word(body,id);Word(body,parent);Word(body,0);Word(body,1);
        Word(body,used_class);Word(body,version);Word(body,component);Word(body,master);Word(body,empty?1:2+uint32_t(explicit_field));
        if(empty){Word(body,output);Word(body,string);Word(body,1);Word(body,0);}
        else body.insert(body.end(),original_properties.begin(),original_properties.end());
      };
      entity(100,0,10,0,false);entity(200,100,20,10,true);Word(body,200);
      std::vector<uint8_t> source;const auto original_end=uint32_t(table.size()+4);
      Word(source,original_end);source.insert(source.end(),table.begin(),table.end());
      for(unsigned i=0;i<16;++i)source.push_back(uint8_t(i));Word(source,2);Word(source,1);source.insert(source.end(),body.begin(),body.end());
      SceneAdapterResult result;std::string error;if(!AdaptScene(source,result,error))return false;
      const Reader reader(result.bytes);const auto end=reader.Word(4);const auto boundary=size_t(end)+4;
      if(result.entity_count!=2 || result.component_count!=2 || reader.Word(0)!=0x01000001 || reader.Word(boundary)!=0x02000002 ||
        reader.Word(boundary+8)!=2 || reader.Word(boundary+12)!=1 ||
        !std::equal(table.begin(),table.end(),result.bytes.begin()+8))return false;
      for(unsigned i=0;i<16;++i)if(result.source_uuid[i]!=i)return false;
      const bool added=!explicit_field && !non_io;
      size_t cursor=boundary+16;
      for(unsigned index=0;index<2;++index){
        const auto id=index?200u:100u;
        if(reader.Word(cursor)!=owner || reader.Word(cursor+4)!=id || reader.Word(cursor+8)!=(index?100u:0u) ||
          reader.Word(cursor+12)!=0 || reader.Word(cursor+16)!=0 || reader.Word(cursor+20)!=1)return false;
        cursor+=24;
        if(reader.Word(cursor)!=used_class || reader.Word(cursor+4)!=version || reader.Word(cursor+8)!=(index?20u:10u) || reader.Word(cursor+12)!=(index?10u:0u))return false;
        if(index){
          if(reader.Word(cursor+16)!=1 || reader.Word(cursor+20)!=output || reader.Word(cursor+24)!=string || reader.Word(cursor+28)!=1 || reader.Word(cursor+32)!=0)return false;
          cursor+=36;continue;
        }
        if(reader.Word(cursor+16)!=2+uint32_t(explicit_field)+uint32_t(added))return false;cursor+=20;
        if(!std::equal(original_properties.begin(),original_properties.end(),result.bytes.begin()+cursor))return false;cursor+=original_properties.size();
        if(added){
          const auto name=reader.Word(cursor),self=reader.Word(cursor+16);
          auto modern_text=[&](uint32_t offset){return reader.String(offset+4,size_t(end)+4);};
          if(modern_text(name)!="Instigator" || modern_text(self)!="_self" || reader.Word(cursor+4)!=string || reader.Word(cursor+8)!=1 || reader.Word(cursor+12)!=count)return false;
          for(unsigned row=0;row<count;++row)if(reader.Word(cursor+16+row*4)!=self)return false;
          cursor+=16+count*4;
        }
      }
      if(reader.Word(cursor)!=200 || cursor+4!=result.bytes.size() || (!added && end!=original_end))return false;
    }
    return true;
  }catch(...){return false;}
}
}  // namespace em2::em1

#include "em1_nif_textures.h"
#include "em1_nif_container.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace em2::em1 {
namespace {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
using Color = std::array<uint8_t, 4>;
constexpr size_t kMaximum = 256 * 1024 * 1024;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
uint32_t U32(View bytes, size_t at, bool big) {
  Check(at <= bytes.size() && bytes.size() - at >= 4, "Truncated EM1 NIF word");
  if (big) return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at+1]) << 16 |
                  uint32_t(bytes[at+2]) << 8 | bytes[at+3];
  return uint32_t(bytes[at+3]) << 24 | uint32_t(bytes[at+2]) << 16 |
         uint32_t(bytes[at+1]) << 8 | bytes[at];
}
uint16_t U16(View bytes, size_t at, bool big) {
  Check(at <= bytes.size() && bytes.size() - at >= 2, "Truncated EM1 NIF short");
  return big ? uint16_t(bytes[at]) << 8 | bytes[at+1] : uint16_t(bytes[at+1]) << 8 | bytes[at];
}
void Append(Bytes& output, View bytes) {
  Check(bytes.size() <= kMaximum - output.size(), "Adapted EM1 NIF exceeds size limit");
  output.insert(output.end(), bytes.begin(), bytes.end());
}
void Number(Bytes& output, uint32_t value, bool big, unsigned width = 4) {
  Check(width <= kMaximum - output.size(), "Adapted EM1 NIF exceeds size limit");
  for (unsigned i = 0; i < width; ++i)
    output.push_back(uint8_t(value >> (8 * (big ? width - 1 - i : i))));
}
using Nif = nif::Nif;
enum class Mode { I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, C8, C4 };
Mode TextureMode(uint32_t format, uint8_t bits) {
  if (format == 11 && bits == 4) return Mode::I4;
  if (format == 11 && bits == 8) return Mode::I8;
  if (format == 12 && bits == 8) return Mode::IA4;
  if (format == 12 && bits == 16) return Mode::IA8;
  if (format == 0 && bits == 16) return Mode::RGB565;
  if (format == 1 && bits == 16) return Mode::RGB5A3;
  if (format == 1 && bits == 32) return Mode::RGBA8;
  if (format == 4 && bits == 0) return Mode::CMPR;
  if ((format == 2 || format == 3) && bits == 8) return Mode::C8;
  if ((format == 2 || format == 3) && bits == 4) return Mode::C4;
  throw std::runtime_error("Unsupported EM1 Wii texture format");
}
uint8_t Expand(unsigned value, unsigned bits) {
  if (bits == 3) return uint8_t((value << 5) | (value << 2) | (value >> 1));
  if (bits == 4) return uint8_t(value * 17);
  if (bits == 5) return uint8_t((value << 3) | (value >> 2));
  return uint8_t((value << 2) | (value >> 4));
}
Color Rgb565(unsigned word) {
  return {Expand(word >> 11, 5), Expand((word >> 5) & 63, 6), Expand(word & 31, 5), 255};
}
Bytes Decode(View data, uint32_t width, uint32_t height, Mode mode,
             std::span<const Color> palette) {
  Check(width && height && width <= 8192 && height <= 8192, "Invalid EM1 texture dimensions");
  const unsigned tw = mode == Mode::I4 || mode == Mode::I8 || mode == Mode::IA4 ||
                             mode == Mode::CMPR || mode == Mode::C8 || mode == Mode::C4 ? 8 : 4;
  const unsigned th = mode == Mode::I4 || mode == Mode::CMPR || mode == Mode::C4 ? 8 : 4;
  const unsigned tile_size = mode == Mode::RGBA8 ? 64 : 32;
  const unsigned columns = (width + tw - 1) / tw, rows = (height + th - 1) / th;
  Check(uint64_t(columns) * rows * tile_size <= data.size(), "Truncated EM1 Wii texture mip");
  Bytes output(size_t(width) * height * 4);
  for (unsigned ty = 0; ty < rows; ++ty) for (unsigned tx = 0; tx < columns; ++tx) {
    const auto tile = data.subspan((size_t(ty) * columns + tx) * tile_size, tile_size);
    std::array<std::array<Color, 4>, 4> cmpr{};
    if (mode == Mode::CMPR) {
      for (unsigned sub = 0; sub < 4; ++sub) {
        const auto first = U16(tile, sub * 8, true), second = U16(tile, sub * 8 + 2, true);
        auto& colors = cmpr[sub]; colors[0] = Rgb565(first); colors[1] = Rgb565(second);
        for (unsigned c = 0; c < 3; ++c) {
          const int a = colors[0][c], b = colors[1][c];
          // Wii CMPR is not BC1: use 5/8 interpolation, retain transparent RGB,
          // and truncate signed midpoint differences toward the first endpoint.
          colors[2][c] = uint8_t(first > second ? (5*a + 3*b) / 8 : a + (b-a) / 2);
          colors[3][c] = uint8_t(first > second ? (3*a + 5*b) / 8 : colors[2][c]);
        }
        colors[2][3] = 255; colors[3][3] = first > second ? 255 : 0;
      }
    }
    for (unsigned y = 0; y < std::min(th, height - ty * th); ++y)
      for (unsigned x = 0; x < std::min(tw, width - tx * tw); ++x) {
        const unsigned index = y * tw + x;
        Color color{};
        switch (mode) {
          case Mode::I4: { const auto v = Expand((tile[index/2] >> (index%2 ? 0 : 4)) & 15, 4); color = {v,v,v,v}; break; }
          case Mode::I8: { const auto v = tile[index]; color = {v,v,v,v}; break; }
          case Mode::IA4: { const auto v = Expand(tile[index] & 15, 4); color = {v,v,v,Expand(tile[index] >> 4,4)}; break; }
          case Mode::IA8: { const auto v = tile[index*2+1]; color = {v,v,v,tile[index*2]}; break; }
          case Mode::RGB565: color = Rgb565(U16(tile,index*2,true)); break;
          case Mode::RGB5A3: {
            const auto word = U16(tile,index*2,true);
            color = word & 0x8000 ? Color{Expand((word>>10)&31,5),Expand((word>>5)&31,5),Expand(word&31,5),255}
                : Color{Expand((word>>8)&15,4),Expand((word>>4)&15,4),Expand(word&15,4),Expand((word>>12)&7,3)};
            break;
          }
          case Mode::RGBA8: color = {tile[index*2+1],tile[32+index*2],tile[33+index*2],tile[index*2]}; break;
          case Mode::C4: {
            const auto value=(tile[index/2]>>(index%2?0:4))&15;
            Check(value<palette.size(),"Invalid EM1 four-bit texture palette index");color=palette[value];break;
          }
          case Mode::C8:
            Check(tile[index] < palette.size(), "Invalid EM1 texture palette index");
            color = palette[tile[index]]; break;
          case Mode::CMPR: {
            const unsigned sub = (y/4)*2+x/4;
            color = cmpr[sub][(tile[sub*8+4+y%4] >> (6-2*(x%4))) & 3]; break;
          }
        }
        std::copy(color.begin(), color.end(), output.begin() + ((size_t(ty*th+y)*width+tx*tw+x)*4));
      }
  }
  return output;
}
}  // namespace

bool TestNifContainerCompatibility(std::string& error) {
  try {
    for (const auto version : {0x14030109u, 0x14060500u}) for (const bool big : {false, true}) {
      const std::string header = version == 0x14030109
          ? "Gamebryo File Format, Version 20.3.1.9\n"
          : "Gamebryo File Format, Version 20.6.5.0\n";
      Bytes source(header.begin(), header.end());
      Number(source,version,false);source.push_back(big?0:1);
      Number(source,0,false);Number(source,2,false);
      Number(source,2,big,2);
      for (const std::string type : {"NiTriShape", "NiSourceTexture"}) {
        Number(source,uint32_t(type.size()),big);
        source.insert(source.end(),type.begin(),type.end());
      }
      Number(source,0,big,2);Number(source,1,big,2);
      Number(source,7,big);Number(source,36,big);
      const std::string filename="GridRoom2_ExtTexture.NIF";
      Number(source,1,big);Number(source,uint32_t(filename.size()),big);
      Number(source,uint32_t(filename.size()),big);
      source.insert(source.end(),filename.begin(),filename.end());Number(source,0,big);
      const Bytes opaque={0xEA,0x15,0x91,0x00,0xFF,0x73,0x08};
      Append(source,opaque);
      Number(source,UINT32_MAX,big);Number(source,0,big);Number(source,UINT32_MAX,big);
      source.push_back(1);Number(source,0,big);Number(source,UINT32_MAX,big);
      Number(source,0,big);Number(source,6,big);Number(source,2,big);
      source.push_back(0);source.push_back(1);source.push_back(0);
      Number(source,1,big);Number(source,0,big);
      Nif model(source);
      Check(model.version==version && model.big==big && model.Write()==source,
            "NIF container round trip changed original version or opaque bytes");
      NifTextureResult adapted;std::string reason;
      Check(AdaptNifTextures(source,adapted,reason),reason.c_str());
      Check(adapted.bytes==source && adapted.textures==0 && adapted.remaining_wii_display_lists==0,
            "Texture adapter changed a native legacy/static container");
      const auto extra=model.AddBlock("OriginalOpaqueExtension",opaque);
      const auto name=model.AddString("source-owned-texture-identity");
      const auto rewritten=model.Write();Nif reread(rewritten);
      Check(reread.version==version && reread.blocks.size()==3 &&
            reread.String(name)=="source-owned-texture-identity" &&
            reread.types[reread.blocks[extra].type]=="OriginalOpaqueExtension" &&
            std::equal(opaque.begin(),opaque.end(),reread.blocks[0].Data().begin()),
            "NIF table growth lost original version, blocks or added identity");
      auto reject=[&](Bytes invalid) {
        NifTextureResult untouched;untouched.bytes={0xAB};untouched.textures=99;
        Check(!AdaptNifTextures(invalid,untouched,reason) &&
              untouched.bytes==Bytes{0xAB} && untouched.textures==99,
              "Invalid NIF was accepted or partially published");
      };
      auto invalid=source;invalid[header.size()+3]=0x7F;reject(invalid);
      invalid=source;invalid.pop_back();reject(invalid);
      if(version==0x14030109){invalid=source;invalid[header.size()+5]=1;reject(invalid);}
    }
    error.clear();return true;
  } catch(const std::exception& exception){error=exception.what();return false;}
}

bool AdaptNifTextures(std::span<const uint8_t> original,
                       NifTextureResult& result, std::string& error) {
  try {
    Nif nif(original);
    NifTextureResult prepared;
    uint16_t pixel_type = 0;
    std::vector<bool> changed(nif.blocks.size(), false);
    size_t expanded = 0;
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
      auto& object = nif.blocks[i];
      const auto& type = nif.types[object.type];
      if (type.starts_with("NiDataStream\x01" "4\x01")) ++prepared.remaining_wii_display_lists;
      if (type != "NiPersistentSrcTextureRendererData") continue;
      const auto block = object.original;
      Check(block.size() >= 87, "Truncated EM1 texture metadata");
      const auto word = [&](size_t at) { return U32(block, at, nif.big); };
      const auto mode = TextureMode(word(0),block[4]);
      const auto mips = word(63);
      Check(word(14) == 2 && mips >= 1 && mips <= 16, "Unsupported EM1 Wii texture layout");
      const size_t end = 71 + size_t(mips) * 12;
      Check(word(end+8) == 1 && word(end+12) == 4 && block.size() - end - 16 == word(end),
            "Unsupported EM1 Wii texture face/platform/length");
      const auto payload = block.subspan(end+16);
      std::vector<Color> palette;
      if (mode == Mode::C8 || mode == Mode::C4) {
        const auto link = word(59);
        Check(link < nif.blocks.size() && nif.types[nif.blocks[link].type] == "NiPalette",
              "Missing EM1 texture palette");
        const auto pal = nif.blocks[link].original;
        const auto count = U32(pal,1,nif.big);
        Check(count >= 1 && count <= (mode==Mode::C4?16u:256u) && pal.size() == 5 + count * 4,
              "Invalid EM1 texture palette size");
        palette.resize(count);
        for (uint32_t c = 0; c < count; ++c) std::copy_n(pal.begin()+5+c*4,4,palette[c].begin());
      }
      Bytes pixels;
      std::vector<std::array<uint32_t,3>> levels;
      for (uint32_t mip = 0; mip < mips; ++mip) {
        const auto width = word(71+mip*12), height = word(75+mip*12), offset = word(79+mip*12);
        Check(offset < payload.size() && uint64_t(width) * height * 4 <= kMaximum - expanded,
              "Invalid or oversized EM1 texture mip");
        levels.push_back({width,height,uint32_t(pixels.size())});
        auto decoded = Decode(payload.subspan(offset),width,height,mode,palette);
        expanded += decoded.size();
        Append(pixels,decoded);
      }
      Bytes adapted;
      Number(adapted,1,nif.big); adapted.push_back(32);
      Number(adapted,UINT32_MAX,nif.big); Number(adapted,0,nif.big); adapted.push_back(1);
      Number(adapted,0,nif.big); adapted.push_back(block[18]);
      for (uint32_t channel = 0; channel < 4; ++channel) {
        Number(adapted,channel,nif.big); Number(adapted,0,nif.big);
        adapted.push_back(8); adapted.push_back(0);
      }
      Number(adapted,UINT32_MAX,nif.big); Number(adapted,mips,nif.big); Number(adapted,4,nif.big);
      for (const auto& level : levels) for (const auto value : level) Number(adapted,value,nif.big);
      Number(adapted,uint32_t(pixels.size()),nif.big); Number(adapted,1,nif.big);
      Append(adapted,pixels);
      if (!prepared.textures) {
        Check(nif.types.size() < UINT16_MAX, "EM1 NIF has no remaining block type slot");
        pixel_type = uint16_t(nif.types.size()); nif.types.push_back("NiPixelData");
      }
      object.type = pixel_type;
      object.replacement = std::move(adapted);
      changed[i] = true;
      ++prepared.textures;
    }
    for (auto& block : nif.blocks) {
      if (nif.types[block.type] != "NiSourceTexture") continue;
      const auto data = block.original;
      const uint64_t base = 12ull + 4ull * U32(data,4,nif.big);
      Check(base + 24 == data.size(), "Unsupported EM1 source texture layout");
      const auto link = U32(data,size_t(base)+5,nif.big);
      if (link < changed.size() && changed[link]) {
        block.replacement.assign(data.begin(), data.end());
        block.replacement[size_t(base)] = 0;
        block.replacement[block.replacement.size()-2] = 0;
        block.replacement.back() = 0;
      }
    }
    prepared.bytes = nif.Write();
    result = std::move(prepared);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}
}  // namespace em2::em1

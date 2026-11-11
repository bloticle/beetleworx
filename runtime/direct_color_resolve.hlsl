// Native color -> scaled guest texture resolve. Original Xenia destination,
// sample-selection, half-pixel, red/blue and endian equations stay unchanged.
// Only the EDRAM reads are replaced; eligibility must prove one complete RGBA8
// owner, matching 1x sample layout, bounds and no EDRAM wrapping.
#define SHADING_LANGUAGE_HLSL_XE 1
#define XE_RESOLVE_RESOLUTION_SCALED
#define XE_RESOLVE_COPY_EDRAM_BYTE_BUFFER_ALIGNMENT 16
#include "rex_render_target/thirdparty/direct_resolve/src/xenia/ui/shaders/xesl.xesli"
// The installed SDK predates upstream's larger scaled texture groups. Keep
// upstream sources intact, but use its installed 4x1 RGBA8 grouping: each
// 16-byte guest group contains column-major (phaseX,phaseY) host groups.
#define XeniaTextureGetResolutionScaledAddressing Em2CurrentUpstreamScaledAddressing
#include "rex_render_target/thirdparty/direct_resolve/src/xenia/gpu/shaders/texture_address.xesli"
#undef XeniaTextureGetResolutionScaledAddressing
XeniaTextureResolutionScaledAddressing XeniaTextureGetResolutionScaledAddressing(
    uint2 position,uint2 scale,uint bytes_per_block_log2) {
    if(bytes_per_block_log2!=2)
        return Em2CurrentUpstreamScaledAddressing(position,scale,bytes_per_block_log2);
    XeniaTextureResolutionScaledAddressing a;
    a.guest_group_origin=uint2((position.x/(4*scale.x))*4,position.y/scale.y);
    uint2 phase=uint2((position.x/4)%scale.x,position.y%scale.y);
    a.host_byte_offset_in_guest_group=(phase.x*scale.y+phase.y)*16+(position.x%4)*4;
    return a;
}
#include "rex_render_target/thirdparty/direct_resolve/src/xenia/gpu/shaders/resolve.xesli"
uint Em2InstalledScaledQuadStep(XeResolveInfo info,uint2 pixel_index) {
    uint phase=(pixel_index.x/4)%info.resolution_scale.x;
    uint step=phase+1==info.resolution_scale.x
        ? info.resolution_scale.x*32-phase*16 : 16;
    return step*info.resolution_scale.y;
}
Texture2D<float4> em2_native_source : register(t1);
cbuffer Em2NativeSource : register(b1) {
    uint em2_source_base, em2_source_pitch, em2_scale_x, em2_scale_y;
};
uint Em2NativePixel(uint2 p) {
    // Identical to the installed RGBA8 dump shader: mad 255,+.5 then ftou.
    uint4 bytes = uint4(mad(em2_native_source.Load(int3(p,0)),255.0,.5));
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}
uint4 Em2NativeEdramRead16(uint address) {
    uint width=80*em2_scale_x, height=16*em2_scale_y;
    uint word=address>>2, tile=word/(width*height);
    uint inside=word-tile*(width*height), relative=tile-em2_source_base;
    uint2 p=uint2((relative%em2_source_pitch)*width+inside%width,
                  (relative/em2_source_pitch)*height+inside/width);
    // Resolve threads issue aligned 4-pixel reads that never cross an 80*scale
    // row. The CPU preflight rejects unsupported layouts and resource bounds.
    return uint4(Em2NativePixel(p),Em2NativePixel(p+uint2(1,0)),
                 Em2NativePixel(p+uint2(2,0)),Em2NativePixel(p+uint2(3,0)));
}
#undef byte_buffer_align16_load16_xe
#define byte_buffer_align16_load16_xe(buffer,address) Em2NativeEdramRead16(address)
#define XeResolveLocalXAddressXor(x,bpp) Em2InstalledScaledQuadStep(resolve_info,pixel_index)
#include "rex_render_target/thirdparty/direct_resolve/src/xenia/gpu/shaders/resolve_fast_32bpp_1x2xmsaa.xesli"

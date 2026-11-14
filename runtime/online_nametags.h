#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
struct PPCContext;
namespace em2::net {
struct OnlineNameActor {uint64_t peer=0;uint32_t entity=0,rgba=0;std::string name;unsigned character=0;};
struct NameTagCamera {
    std::array<float,3> position{},right{},up{},forward{};
    float left=0,right_edge=0,top=0,bottom=0,near_plane=0,far_plane=0;
    bool orthographic=false;
};
bool ProjectNameTag(const NameTagCamera&,const std::array<float,3>&,float& x,float& y);
// Render-owner only, after presentation transforms are applied. Guest pointers
// never cross into the host UI thread. May retire cached clothing references
// at this native render boundary; does not advance actor/camera simulation.
void CaptureOnlineNameTags(PPCContext&,uint8_t*);
void ClearOnlineNameTags();
void DrawOnlineNameTags(float width,float height);
bool TestOnlineNameTags();
}

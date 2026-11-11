#include "online_notice.h"
#include <imgui.h>
#include <rex/logging.h>
#include <algorithm>
#include <cmath>
#include <immintrin.h>

namespace em2::net {
std::string OnlineFailureAdvice(std::string_view reason,bool during_play) {
    std::string text(reason);
    for(auto& ch:text)if(static_cast<unsigned char>(ch)<32 || ch==127)ch=' ';
    if(text.size()>240){size_t cut=240;while(cut && (static_cast<unsigned char>(text[cut])&0xC0)==0x80)--cut;text.resize(cut);text+="...";}
    auto contains=[&](std::string_view value){return text.find(value)!=std::string::npos;};
    std::string advice;
    if(contains("(10061)"))advice="The host is not accepting a connection. Ask them to select Host, then paste their new invitation.";
    else if(contains("(10060)") || contains("Connection/handshake timed out"))
        advice="The host could not be reached. On the same Wi-Fi, try Find LAN games. Over the Internet, check the host's router connection.";
    else if(contains("(10051)") || contains("(10065)"))
        advice="No route to the host. Check your connection and request a fresh invitation; a LAN invitation only works on the host's network.";
    else if(contains("(10048)"))advice="That host port is already in use. Choose port 0 (automatic), then select Host again.";
    else if(contains("(10013)"))advice="Windows denied this connection. Check Beetleworx's firewall permission and try again.";
    else if(contains("Different game builds") || contains("Different game content") || contains("different session") ||
            contains("Different session timing") || contains("Both copies selected"))return text;
    else advice=during_play?"Restart both copies from the shared save.":"Online setup stopped. Try Host or Join again.";
    return advice+(text.empty()?std::string{}:"\n"+text);
}
OnlineNotice OnlineNoticeModel::Update(const NativeSessionStatus& status,bool automatic,Time now) {
    using namespace std::chrono_literals;
    if(!initialized_ || status.state!=state_ || now<last_) {
        initialized_=true;state_=status.state;state_since_=now;wait_=InputWait::None;stalled_=false;recovered_until_={};
    }
    last_=now;
    switch(status.state) {
    case NativeSessionState::None:return {};
    case NativeSessionState::Failed:return {"Online play stopped",OnlineFailureAdvice(status.error,true),true};
    case NativeSessionState::WaitingForWorld:
        return {"Online connected",status.continue_requested?"Starting the shared save. Waiting for gameplay...":
            automatic?"Select your profile if prompted. The shared save starts automatically when ready.":"Waiting for gameplay. Choose Continue if you are still at the title menu."};
    case NativeSessionState::Joining:return {"Preparing online co-op","Preparing both characters for online play..."};
    case NativeSessionState::Attached:break;
    }
    if(status.wait!=InputWait::None) {
        if(wait_!=status.wait){wait_=status.wait;wait_since_=now;}
        if(now-wait_since_>=500ms) {
            stalled_=true;
            if(wait_==InputWait::LocalController)return {"Controller unavailable","Reconnect the controller selected for this session."};
            if(wait_==InputWait::Verification)return {"Synchronizing online play","Gameplay is paused while both copies catch up."};
            return {"Waiting for the other player","Gameplay will continue when both copies are ready."};
        }
    } else {
        if(stalled_)recovered_until_=now+2s;
        wait_=InputWait::None;stalled_=false;
        if(now<recovered_until_)return {"Online play resumed","Both players are ready."};
    }
    if(now-state_since_<3s)return {"Online controls connected","Both characters are ready."};
    return {};
}
void DrawOnlineNotice(const OnlineNotice& notice,float width,float height) {
    if(!notice || !std::isfinite(width) || !std::isfinite(height) || width<160 || height<120)return;
    const float box_width=std::min(620.f,width-32.f),wrap=box_width-20.f;
    const auto title=ImGui::CalcTextSize(notice.title.c_str(),nullptr,false,wrap);
    const auto detail=ImGui::CalcTextSize(notice.detail.c_str(),nullptr,false,wrap);
    const float box_height=std::min(title.y+detail.y+26.f,height-32.f);
    const ImVec2 top((width-box_width)*.5f,16),bottom(top.x+box_width,top.y+box_height);
    auto* draw=ImGui::GetForegroundDrawList();
    draw->PushClipRect(top,bottom,true);
    draw->AddRectFilled(top,bottom,IM_COL32(0,0,0,210),5.f);
    draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),ImVec2(top.x+10,top.y+10),
        notice.error?IM_COL32(255,155,145,255):IM_COL32(175,225,255,255),notice.title.c_str(),nullptr,wrap);
    draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),ImVec2(top.x+10,top.y+title.y+16),IM_COL32(245,245,245,255),notice.detail.c_str(),nullptr,wrap);
    draw->PopClipRect();
}
bool TestOnlineNotices() {
    struct RestoreMath{unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}}restore_math;
    using namespace std::chrono_literals;
    bool ok=true;unsigned checks=0;
    auto check=[&](bool value,const char* text){++checks;if(!value){ok=false;REXLOG_ERROR("Online notice test: {}",text);}};
    check(OnlineFailureAdvice("connect completion (10061)",false).find("select Host")!=std::string::npos,
        "connection refusal explains host/invitation recovery");
    check(OnlineFailureAdvice("Connection/handshake timed out",false).find("Find LAN games")!=std::string::npos,
        "actual handshake timeout distinguishes LAN and Internet recovery");
    check(OnlineFailureAdvice("connect (10065)",false).find("LAN invitation")!=std::string::npos,
        "no-route failure explains private-address invitations");
    check(OnlineFailureAdvice("bind/listen (10048)",false).find("port 0")!=std::string::npos,
        "port conflict explains automatic-port recovery");
    check(OnlineFailureAdvice("bind/listen (10013)",false).find("firewall")!=std::string::npos,
        "permission failure explains application permission");
    const std::string mismatch="Different game builds. Both players must use the same beetleworx.exe.";
    check(OnlineFailureAdvice(mismatch,false)==mismatch,"build mismatch retains its specific actionable repair");
    check(OnlineFailureAdvice("Unknown setup failure",false).find("Try Host or Join again")!=std::string::npos,
        "pre-world failure does not prescribe restarting gameplay");
    OnlineNoticeModel model;NativeSessionStatus status;const auto start=OnlineNoticeModel::Time{};
    check(!model.Update(status,true,start),"offline must stay silent");
    status.state=NativeSessionState::WaitingForWorld;
    check(model.Update(status,true,start).detail.find("automatically")!=std::string::npos,"automatic startup explains readiness");
    check(model.Update(status,false,start).detail.find("Continue")!=std::string::npos,"manual startup explains Continue");
    status.continue_requested=true;check(model.Update(status,true,start).detail.find("Starting")!=std::string::npos,"accepted request is visible");
    status.state=NativeSessionState::Joining;check(model.Update(status,true,start).title=="Preparing online co-op","join is distinct");
    status.state=NativeSessionState::Attached;check(bool(model.Update(status,true,start)),"new attachment has a toast");
    check(!model.Update(status,true,start+3s),"healthy connection toast expires");
    status.wait=InputWait::Peer;
    check(!model.Update(status,true,start+4s) && !model.Update(status,true,start+4499ms),"short waits do not flicker");
    check(model.Update(status,true,start+4500ms).title=="Waiting for the other player","sustained peer wait is shown");
    status.wait=InputWait::None;check(model.Update(status,true,start+5s).title=="Online play resumed","recovery has feedback");
    check(!model.Update(status,true,start+7s),"recovery toast expires");
    status.wait=InputWait::LocalController;model.Update(status,true,start+8s);
    check(model.Update(status,true,start+8500ms).title=="Controller unavailable","local input loss is not mislabeled peer loss");
    status.wait=InputWait::Verification;model.Update(status,true,start+9s);
    check(model.Update(status,true,start+9500ms).title=="Synchronizing online play","verification wait is distinct");
    status.state=NativeSessionState::Failed;status.error=std::string(300,'X')+"\n\t%ss";
    const auto failure=model.Update(status,true,start+10s);
    check(failure.error && failure.detail.size()<300 && failure.detail.find("Restart both copies")!=std::string::npos,"failure stays bounded with recovery guidance");
    status.error="Lost\nconnection\t%p";const auto plain=model.Update(status,true,start+11s);
    check(plain.detail.find("Lost connection %p")!=std::string::npos,"control characters are neutralized without format interpretation");
    status={};check(!model.Update(status,true,start+12s),"release clears failure");
    status.state=NativeSessionState::Attached;check(bool(model.Update(status,true,start)),"clock reset starts clean");
    auto* previous=ImGui::GetCurrentContext();auto* context=ImGui::CreateContext();
    struct RestoreContext{ImGuiContext* current,*previous;~RestoreContext(){ImGui::DestroyContext(current);ImGui::SetCurrentContext(previous);}}restore{context,previous};
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    unsigned char* pixels=nullptr;int font_width=0,font_height=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&font_width,&font_height);
    for(const auto size:{ImVec2(320,180),ImVec2(640,480),ImVec2(1280,720),ImVec2(3840,2160)}) {
        io.DisplaySize=size;io.DeltaTime=1.f/60;ImGui::NewFrame();DrawOnlineNotice(failure,size.x,size.y);ImGui::Render();
        const auto* data=ImGui::GetDrawData();check(data && data->TotalVtxCount>0,"failure creates actual foreground geometry");
        bool finite=true,clipped=true;
        for(int list=0;data && list<data->CmdListsCount;++list) {
            const auto* commands=data->CmdLists[list];
            for(const auto& vertex:commands->VtxBuffer)finite&=std::isfinite(vertex.pos.x)&&std::isfinite(vertex.pos.y);
            for(const auto& command:commands->CmdBuffer)clipped&=command.ClipRect.x>=0 && command.ClipRect.y>=0 && command.ClipRect.z<=size.x && command.ClipRect.w<=size.y;
        }
        check(finite && clipped && !io.WantCaptureKeyboard && !io.WantCaptureMouse,"banner stays clipped and noninteractive");
    }
    REXLOG_INFO("Online notices: {} deterministic state/timing/foreground-layout checks, no window or GPU",checks);return ok;
}
}

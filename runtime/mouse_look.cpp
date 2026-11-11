#include "mouse_look.h"
#include "controls.h"
#include "pc_settings.h"
#include "ppc_context.h"
#include "native_engine.h"
#include "players.h"
#include "online_runtime.h"
#include <rex/ui/window_win.h>
#include <rex/logging.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>
#include <immintrin.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_private_profile);
PPC_EXTERN_FUNC(sub_827C3B80);

namespace em2 {
namespace {
using Clock=std::chrono::steady_clock;
using Time=Clock::time_point;
using namespace std::chrono_literals;
struct Motion {
    uint32_t mode=0;bool available=false,captured=false;
    int64_t x=0,y=0;Time last{};
    void Clear(){x=y=0;}
    bool Fresh(Time now)const{return available && now>=last && now-last<150ms;}
    void Capture(bool value){if(captured!=value)Clear();captured=value;}
    void Add(int32_t dx,int32_t dy,bool relative,Time now) {
        if(!captured || !relative || !Fresh(now))return;
        x=std::clamp(x+dx,int64_t(-65536),int64_t(65536));
        y=std::clamp(y+dy,int64_t(-65536),int64_t(65536));
    }
    MouseLookAngles Take(uint32_t owner,bool allowed,Time now,float sx,float sy) {
        if(!allowed || !owner){mode=0;available=false;Clear();return {};}
        if(mode!=owner || !Fresh(now))Clear();
        mode=owner;available=true;last=now;
        if(!captured){Clear();return {};}
        // Counts represent displacement, not angular velocity: no dt factor.
        constexpr double radians_per_count=3.14159265358979323846/3600.;
        const MouseLookAngles result{float(-double(x)*radians_per_count*sx),float(double(y)*radians_per_count*sy)};
        Clear();return result;
    }
};
std::mutex motion_mutex;Motion motion;
struct DeviceHandoff {
    bool controller=false,center_pending=false,aiming=false;unsigned mouse_counts=0;
    std::array<int,7> pad{};
    bool Mouse(const MouseCursorOptions& options) {
        const bool switched=controller;controller=false;mouse_counts=0;
        if(switched && options.center_switch)center_pending=true;
        return switched;
    }
    void Pad(std::array<int,7> next) {
        // Quantization suppresses stick drift. A held stick is not a new
        // device transition on every poll, and neutral releases don't steal it.
        bool changed=(next[0]&~pad[0])!=0;
        for(unsigned i=1;i<next.size();++i)changed|=next[i]!=0 && next[i]!=pad[i];
        pad=next;if(changed){controller=true;mouse_counts=0;}
    }
    void Aim(bool value,const MouseCursorOptions& options) {
        if(value && !aiming && !controller && options.center_aim)center_pending=true;
        aiming=value;
    }
} handoff;
std::atomic<bool> center_desktop_pending{false};
void MouseActivityLocked(const MouseCursorOptions& options) {
    if(handoff.Mouse(options)){motion.Clear();if(options.center_switch)center_desktop_pending.store(true);}
}
std::optional<RAWINPUTDEVICE> MouseRegistration(bool& valid) {
    UINT count=0;valid=GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE))!=UINT(-1);
    if(!valid || !count)return {};
    std::vector<RAWINPUTDEVICE> devices(count);
    const auto result=GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE));
    valid=result!=UINT(-1);if(!valid)return {};
    for(UINT i=0;i<result;++i)if(devices[i].usUsagePage==1 && devices[i].usUsage==2)return devices[i];
    return {};
}
bool EqualRect(const RECT& a,const RECT& b){return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;}
}
void NotifyMouseKeyboardInput() {
    std::lock_guard lock(motion_mutex);MouseActivityLocked(GetMouseCursorOptions());
}
void NotifyControllerInput(unsigned user,uint16_t buttons,uint8_t left,uint8_t right,int16_t lx,int16_t ly,int16_t rx,int16_t ry) {
    if(user)return;
    auto axis=[](int value){return std::abs(value)<8192?0:value/2048;};
    std::lock_guard lock(motion_mutex);
    handoff.Pad({int(buttons),left>30?int(left)/16:0,right>30?int(right)/16:0,axis(lx),axis(ly),axis(rx),axis(ry)});
    if(handoff.controller)motion.Clear();
}
void NotifyMappedAimInput(unsigned user,bool aiming) {
    if(user)return;std::lock_guard lock(motion_mutex);handoff.Aim(aiming,GetMouseCursorOptions());
}
namespace {
void CenterNativeCursor(PPCContext& ctx,uint8_t* base,uint32_t manager,uint32_t entity) {
    struct Restore {PPCContext& ctx;PPCContext saved;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
        ~Restore(){ctx=saved;_mm_setcsr(csr);SetLastError(error);}}restore{ctx,ctx};
    // Solo uses both the HUD's primary cursor and the possessed actor's native
    // aim cursor. Co-op supplies an entity filter to preserve the other player.
    ctx.r3.u64=manager;ctx.r4.u64=entity;ctx.r5.u64=1;ctx.r6.u64=1;sub_827C3B80(ctx,base);
}
bool TestNativeCursorCenter() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {uint8_t* base;unsigned csr=_mm_getcsr();DWORD error=GetLastError();
        ~Restore(){if(base)VirtualFree(base,0,MEM_RELEASE);_mm_setcsr(csr);SetLastError(error);}}restore{base};
    if(!base || !VirtualAlloc(base,0x10000,MEM_COMMIT,PAGE_READWRITE) ||
       !VirtualAlloc(base+0x82000000,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    PPC_STORE_U32(0x82003EA0,std::bit_cast<uint32_t>(.5f));
    for(unsigned missing=0;missing<4;++missing)for(unsigned owner:{0u,11u,22u,33u}) {
        PPC_STORE_U32(0x10A0,(missing&1)?0:0x2000);PPC_STORE_U32(0x10A4,(missing&2)?0:0x3000);
        PPC_STORE_U32(0x2000,11);PPC_STORE_U32(0x3000,22);PPC_STORE_U32(0x4000,owner);
        for(uint32_t cursor:{0x2000u,0x3000u}){PPC_STORE_U32(cursor+4,std::bit_cast<uint32_t>(.2f));PPC_STORE_U32(cursor+8,std::bit_cast<uint32_t>(.9f));}
        PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.lr=0x12345678;
        const auto before=ctx;const auto csr=_mm_getcsr();SetLastError(0x1234);
        CenterNativeCursor(ctx,base,0x1000,owner?0x4000:0);
        if(std::memcmp(&ctx,&before,sizeof(ctx)) || _mm_getcsr()!=csr || GetLastError()!=0x1234)return false;
        for(unsigned i=0;i<2;++i) {
            const uint32_t cursor=i?0x3000:0x2000;const bool centered=(!owner || owner==(i?22u:11u)) && !(missing&(1u<<i));
            if(PPC_LOAD_U32(cursor+4)!=std::bit_cast<uint32_t>(centered?.5f:.2f) ||
               PPC_LOAD_U32(cursor+8)!=std::bit_cast<uint32_t>(centered?.5f:.9f))return false;
        }
    }
    REXLOG_INFO("Native cursor recenter passed 16 real retail manager dispatches: solo HUD/aim synchronization, Mickey/Oswald entity filtering, unrelated actor, missing cursors, both axes, other-player preservation and complete caller/FP/error restoration");
    return true;
}
}
void TickMouseCursor(PPCContext& ctx,uint8_t* base) {
    if(!MouseCameraEnabled() || ControlBindingsOpen() || net::SynchronizedSessionActive() || !NativeConsoleWorldReady())return;
    const auto manager=PPC_LOAD_U32(0x833DBAD8),players=PPC_LOAD_U32(0x833DAB60);if(!manager || !players)return;
    const auto profile=PPC_LOAD_U32(0x833DC87C);if(!profile)return;
    const auto count=PPC_LOAD_U32(profile+24);if(count!=1 && count!=2)return;
    const auto entity=players+(OswaldIsSelected()?340u:20u);if(!PPC_LOAD_U32(entity))return;
    {std::lock_guard lock(motion_mutex);if(!handoff.center_pending)return;handoff.center_pending=false;}
    CenterNativeCursor(ctx,base,manager,count==1?0:entity);
}
MouseLookAngles TakeMouseLook(uint32_t mode,bool allowed) {
    struct RestoreMath {unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}}restore_math;
    _mm_setcsr(0x1F80);
    auto scales=MouseLookStrength();const auto options=GetMouseCursorOptions();
    scales.x*=options.gain_x;scales.y*=options.gain_y;
    const bool permitted=allowed && MouseCameraEnabled() && !ControlBindingsOpen();
    if(REXCVAR_GET(em2_private_profile)) {
        static thread_local uint32_t last=UINT32_MAX;
        const uint32_t state=(bool(mode)?1u:0u)|(allowed?2u:0u)|(MouseCameraEnabled()?4u:0u)|(ControlBindingsOpen()?8u:0u);
        if(state!=last){last=state;REXLOG_INFO("Private mouse camera admission: mode {:08X} allowed {} enabled {} controls captured {}",mode,allowed,MouseCameraEnabled(),ControlBindingsOpen());}
    }
    std::lock_guard lock(motion_mutex);
    auto angles=motion.Take(mode,permitted,Clock::now(),scales.x,scales.y);
    return !options.always_mouse && handoff.controller?MouseLookAngles{}:angles;
}
bool MouseLookHasMotion(uint32_t mode) {
    std::lock_guard lock(motion_mutex);
    return motion.captured && motion.mode==mode && motion.Fresh(Clock::now()) && (motion.x || motion.y);
}
bool MouseLookHasPendingMotion(){
    std::lock_guard lock(motion_mutex);
    return motion.x || motion.y;
}
void ResetMouseLook() {
    std::lock_guard lock(motion_mutex);motion.available=false;motion.mode=0;motion.Clear();
}
struct MouseLookHost::Impl {
    rex::ui::Window* window=nullptr;HWND hwnd=nullptr;bool attached=false,captured=false,closing=false,modal=false,prior_clip_valid=false;
    static constexpr UINT refresh_message=WM_APP+0x4D2;
    HANDLE timer=nullptr;
    std::optional<RAWINPUTDEVICE> previous;
    RECT prior_clip{},our_clip{};bool clipped=false;
    rex::ui::Window::CursorVisibility prior_cursor=rex::ui::Window::CursorVisibility::kVisible;
    explicit Impl(rex::ui::Window* value):window(value) {
        hwnd=value?static_cast<rex::ui::Win32Window*>(value)->hwnd():nullptr;
        if(!hwnd || GetWindowThreadProcessId(hwnd,nullptr)!=GetCurrentThreadId()){
            REXLOG_WARN("Mouse input could not attach: window available {}, owning thread {}, current thread {}",bool(hwnd),hwnd?GetWindowThreadProcessId(hwnd,nullptr):0,GetCurrentThreadId());return;
        }
        attached=SetWindowSubclass(hwnd,Procedure,reinterpret_cast<UINT_PTR>(this),reinterpret_cast<DWORD_PTR>(this))!=FALSE;
        // ReX's window loop does not service generated WM_TIMER messages.
        // A timer-queue callback posts an ordinary UI message instead; capture
        // and window APIs still run exclusively on the owning UI thread.
        if(attached && !CreateTimerQueueTimer(&timer,nullptr,[](PVOID data,BOOLEAN){
            const auto* self=static_cast<Impl*>(data);PostMessageW(self->hwnd,refresh_message,0,0);
        },this,16,16,WT_EXECUTEDEFAULT))timer=nullptr;
        if(attached && !timer) {
            RemoveWindowSubclass(hwnd,Procedure,reinterpret_cast<UINT_PTR>(this));attached=false;
        }
        REXLOG_INFO("Mouse input window hook attached {}, automatic capture service {}",attached,bool(timer));
    }
    void StopTimer(){if(timer){DeleteTimerQueueTimer(nullptr,timer,INVALID_HANDLE_VALUE);timer=nullptr;}}
    ~Impl(){StopTimer();Release();if(attached)RemoveWindowSubclass(hwnd,Procedure,reinterpret_cast<UINT_PTR>(this));}
    void Release() {
        {std::lock_guard lock(motion_mutex);motion.Capture(false);motion.Clear();}
        if(!captured)return;
        captured=false;
        bool valid=false;const auto current=MouseRegistration(valid);
        // Restore only registration still owned by this feature.
        if(valid && current && current->hwndTarget==hwnd && current->dwFlags==0) {
            RAWINPUTDEVICE restore=previous.value_or(RAWINPUTDEVICE{1,2,RIDEV_REMOVE,nullptr});
            RegisterRawInputDevices(&restore,1,sizeof(restore));
        }
        previous.reset();
        RECT clip{};if(clipped && GetClipCursor(&clip) && EqualRect(clip,our_clip))ClipCursor(&prior_clip);
        clipped=false;
        window->ReleaseMouse();
        if(window->GetCursorVisibility()==rex::ui::Window::CursorVisibility::kHidden)window->SetCursorVisibility(prior_cursor);
    }
    void Refresh() {
        const auto options=GetMouseCursorOptions();
        bool wanted=!closing && !modal && !IsIconic(hwnd) && MouseCameraEnabled() && window->HasFocus() && GetForegroundWindow()==hwnd && !ControlBindingsOpen();
        {std::lock_guard lock(motion_mutex);wanted=wanted && motion.Fresh(Clock::now());}
        if(REXCVAR_GET(em2_private_profile)) {
            static uint32_t previous=UINT32_MAX;
            const uint32_t status=(wanted?1u:0u)|(window->HasFocus()?2u:0u)|(GetForegroundWindow()==hwnd?4u:0u)|(ControlBindingsOpen()?8u:0u)|(modal?16u:0u);
            if(status!=previous){previous=status;REXLOG_INFO("Private mouse host state: wanted {} window focus {} foreground {} UI capture {} modal {}",wanted,window->HasFocus(),GetForegroundWindow()==hwnd,ControlBindingsOpen(),modal);}
        }
        if(!wanted){Release();return;}
        if(!captured) {
            bool valid=false;previous=MouseRegistration(valid);if(!valid){REXLOG_WARN("Mouse registration query failed: {}",GetLastError());return;}
            RAWINPUTDEVICE device{1,2,0,hwnd};
            if(!RegisterRawInputDevices(&device,1,sizeof(device))){REXLOG_WARN("Mouse registration failed: {}",GetLastError());return;}
            prior_cursor=window->GetCursorVisibility();prior_clip_valid=GetClipCursor(&prior_clip)!=FALSE;
            window->CaptureMouse();window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
            captured=true;
            {std::lock_guard lock(motion_mutex);motion.Capture(true);
                if(options.center_capture && !handoff.controller){handoff.center_pending=true;center_desktop_pending.store(true);}}
            if(REXCVAR_GET(em2_private_profile))REXLOG_INFO("Private mouse camera captured raw input");
        }
        const bool hide=options.hide;
        window->SetCursorVisibility(hide?rex::ui::Window::CursorVisibility::kHidden:prior_cursor);
        if(!options.confine && clipped){RECT current{};if(GetClipCursor(&current) && EqualRect(current,our_clip))ClipCursor(&prior_clip);clipped=false;}
        RECT rect{};if(GetClientRect(hwnd,&rect)) {
            POINT first{rect.left,rect.top},last{rect.right,rect.bottom};
            if(ClientToScreen(hwnd,&first) && ClientToScreen(hwnd,&last) && last.x>first.x && last.y>first.y) {
                const RECT next{first.x,first.y,last.x,last.y};
                if(options.confine && prior_clip_valid && ClipCursor(&next)){our_clip=next;clipped=true;}
                if(center_desktop_pending.exchange(false))SetCursorPos(first.x+(last.x-first.x)/2,first.y+(last.y-first.y)/2);
            }
        }
    }
    static LRESULT CALLBACK Procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR data) {
        auto* self=reinterpret_cast<Impl*>(data);
        if(message==WM_ENTERSIZEMOVE || message==WM_ENTERMENULOOP){self->modal=true;self->Release();}
        if(message==WM_EXITSIZEMOVE || message==WM_EXITMENULOOP)self->modal=false;
        if(message==WM_CLOSE || message==WM_DESTROY){self->closing=true;self->Release();}
        if(message==refresh_message){self->Refresh();return 0;}
        if(message==WM_KILLFOCUS || message==WM_CANCELMODE || (message==WM_ACTIVATEAPP && !wparam))self->Release();
        if(message==WM_INPUT && self->captured && self->window->HasFocus() && GetForegroundWindow()==hwnd && GET_RAWINPUT_CODE_WPARAM(wparam)==RIM_INPUT) {
            RAWINPUT input{};UINT bytes=sizeof(input);
            const auto read=GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),RID_INPUT,&input,&bytes,sizeof(RAWINPUTHEADER));
            if(read!=UINT(-1) && read>=offsetof(RAWINPUT,data)+sizeof(RAWMOUSE) && input.header.dwType==RIM_TYPEMOUSE) {
                std::lock_guard lock(motion_mutex);
                const auto dx=input.data.mouse.lLastX,dy=input.data.mouse.lLastY;
                const bool relative=!(input.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE);
                if(relative && (dx || dy)) {
                    const auto options=GetMouseCursorOptions();
                    handoff.mouse_counts=std::min(65536u,handoff.mouse_counts+unsigned(std::min<int64_t>(65536,std::abs(int64_t(dx))+std::abs(int64_t(dy)))));
                    if(!handoff.controller || options.always_mouse || handoff.mouse_counts>=options.takeover_counts)MouseActivityLocked(options);
                    if(!handoff.controller || options.always_mouse)motion.Add(dx,dy,true,Clock::now());
                }
            }
        }
        if(message==WM_NCDESTROY){self->StopTimer();self->Release();RemoveWindowSubclass(hwnd,Procedure,id);self->attached=false;}
        // Preserve normal SDK input and foreground raw-input cleanup.
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }
};
MouseLookHost::MouseLookHost(rex::ui::Window* window):impl_(std::make_unique<Impl>(window)){}
MouseLookHost::~MouseLookHost()=default;
bool TestMouseLook() {
    struct RestoreMath {unsigned csr=_mm_getcsr();~RestoreMath(){_mm_setcsr(csr);}}restore_math;
    bool ok=TestNativeCursorCenter();unsigned checks=0;
    auto check=[&](bool passed,const char* message){++checks;if(!passed){ok=false;REXLOG_ERROR("Mouse look test: {}",message);}};
    {
        const auto saved=[] {std::lock_guard lock(motion_mutex);return motion;}();
        struct RestoreMotion {Motion saved;~RestoreMotion(){std::lock_guard lock(motion_mutex);motion=saved;}}restore_motion{saved};
        {std::lock_guard lock(motion_mutex);motion.Clear();}
        check(!MouseLookHasPendingMotion(),"neutral global mouse state has no pending counts");
        {std::lock_guard lock(motion_mutex);motion.mode=0;motion.x=1;}
        check(MouseLookHasPendingMotion(),"first-mode pending motion is observable before camera ownership exists");
        {std::lock_guard lock(motion_mutex);motion.mode=0x1234;motion.x=0;motion.y=-2;}
        check(MouseLookHasPendingMotion(),"pending motion on a different camera mode blocks background replay");
        {std::lock_guard lock(motion_mutex);check(motion.mode==0x1234 && motion.x==0 && motion.y==-2,"pending inspection preserves all accumulated counts and ownership");}
    }
    {
        MouseCursorOptions options;DeviceHandoff device;
        device.Pad({0,0,0,0,0,0,0});check(!device.controller,"neutral pad does not claim ownership");
        device.Pad({1,0,0,0,0,0,0});check(device.controller,"new controller button claims ownership");
        check(device.Mouse(options) && device.center_pending,"controller-to-mouse transition requests native cursor recenter");
        device.center_pending=false;device.Pad({1,0,0,0,0,0,0});check(!device.controller,"held controller input does not steal mouse ownership");
        device.Pad({0,0,0,0,0,0,0});check(!device.controller,"controller release does not steal mouse ownership");
        device.Pad({0,0,0,0,0,12,0});check(device.controller,"changed deliberate stick input takes ownership");
        options.center_switch=false;check(device.Mouse(options) && !device.center_pending,"keep-position option prevents handoff recenter");
        device.Aim(true,options);check(device.center_pending,"mouse paint/aim entry requests recenter");
        device.center_pending=false;device.Aim(true,options);check(!device.center_pending,"held aim does not repeatedly reset cursor");
        options.center_aim=false;device.Aim(false,options);device.Aim(true,options);check(!device.center_pending,"aim recenter is optional");
    }
    const Time start{};Motion model;
    model.Add(100,200,true,start);check(model.x==0 && model.y==0,"inactive input ignored");
    model.Take(1,true,start,1,1);model.Capture(true);
    model.Add(100,-50,false,start);check(model.x==0,"absolute devices are not mistaken for relative counts");
    model.Add(100,-50,true,start);const auto angle=model.Take(1,true,start+10ms,1,1);
    check(std::abs(angle.yaw+3.14159265358979323846/36)<1e-7 && std::abs(angle.pitch+3.14159265358979323846/72)<1e-7,"relative counts become native radians with matching stick directions");
    check(model.Take(1,true,start+20ms,1,1).yaw==0,"counts consumed once without release tail");
    model.Add(20,30,true,start+20ms);check(model.Take(2,true,start+30ms,1,1).yaw==0,"new camera discards old motion");
    model.Add(20,30,true,start+30ms);check(model.Take(2,false,start+40ms,1,1).yaw==0 && !model.available,"pause/scripted/online rejection clears motion");
    model.Take(2,true,start+50ms,1,1);model.Add(20,30,true,start+50ms);check(model.Take(2,true,start+250ms,1,1).yaw==0,"loading stalls discard stale motion");
    model.Add(20,30,true,start+250ms);model.Capture(false);model.Capture(true);check(model.Take(2,true,start+260ms,1,1).yaw==0,"capture transitions clear pending counts");
    for(unsigned ticks:{30u,60u,120u,240u}) {
        Motion test;test.Take(1,true,start,1,1);test.Capture(true);double yaw=0,pitch=0;
        for(unsigned tick=0;tick<ticks;++tick) {
            const auto now=start+std::chrono::microseconds(1000000ull*tick/ticks);
            test.Add(720/ticks,1440/ticks,true,now);const auto step=test.Take(1,true,now,.5f,-1.f);
            yaw+=step.yaw;pitch+=step.pitch;
        }
        check(std::abs(yaw+3.14159265358979323846/10)<1e-6 && std::abs(pitch+2*3.14159265358979323846/5)<1e-6,"same physical counts give the same total angle across tick rates, strength and inversion");
    }
    if(ok)REXLOG_INFO("Mouse look model passed {} checks: raw relative counts, ownership, capture/focus transitions, pause/online gating, stale input, gain/inversion and no dt multiplier; no OS window/input created",checks);
    return ok;
}
}

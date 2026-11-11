#include "ppc_context.h"
#include "controls.h"
#include "mouse_look.h"
#include "simulation.h"
#include "players.h"
#include "online.h"
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>
#include <immintrin.h>
#include <string>
#include <windows.h>

PPC_EXTERN_FUNC(__imp__sub_831BB758);
PPC_EXTERN_FUNC(sub_831BB758);
PPC_EXTERN_FUNC(__imp__sub_830ACF88);
PPC_EXTERN_FUNC(sub_830ACF88);
namespace {
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
using Clock=std::chrono::steady_clock;
using Profile=std::array<int,em2::kBindingCount>;
constexpr std::array<float,8> look_strength={.5f,.6f,.75f,.85f,1.f,1.25f,1.5f,2.f};
constexpr std::array<const char*,8> look_names={"camera_x","camera_y","invert_x","invert_y","deadzone_x","deadzone_y","curve_x","curve_y"};
constexpr std::array<int,8> look_defaults={4,4,0,0,0,0,0,0};
constexpr std::array<int,8> look_sizes={8,8,2,2,8,8,3,3};
constexpr std::array<float,8> look_deadzones={-1.f,0.f,.05f,.1f,.15f,.2f,.25f,.3f};
// Native 830ACF88 gates raw axes, then 830ABA88 removes/renormalizes the
// configured deadzone. Observe its live value without changing guest state.
// Native initializer 830AB3A8 loads this exact value from 821DE688.
constexpr float native_default_deadzone=8689.f/32768.f;
std::atomic<uint32_t> native_deadzone_bits{std::bit_cast<uint32_t>(native_default_deadzone)};
using LookProfile=std::array<int,8>;
constexpr Profile keyboard_defaults={VK_SPACE,VK_BACK,'Q','E','C','R',VK_RBUTTON,VK_LBUTTON,
    'F','G',VK_ESCAPE,VK_TAB,VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,'A','D','W','S','J','L','I','K',VK_F6,VK_SHIFT,'V',VK_CONTROL};
std::array<std::atomic<unsigned>,2> mod_buttons{};
constexpr std::array<uint16_t,16> masks={0x1000,0x2000,0x4000,0x8000,0x100,0x200,0,0,0x40,0x80,0x10,0x20,1,2,4,8};
std::array<Profile,4> profiles;
std::array<LookProfile,4> look_profiles;
constexpr size_t keyboard_sources=em2::kWheelRight+1;
std::array<bool,keyboard_sources> keys{},blocked_keys{};
std::array<std::array<bool,keyboard_sources>,2> pending_keys{};
std::array<std::array<uint64_t,keyboard_sources>,2> latched_epoch{};
std::array<std::array<bool,keyboard_sources>,2> latched_valid{};
std::array<int64_t,2> wheel_remainder{};
void ResetWheelInputs() {
    wheel_remainder={};
    for(unsigned user=0;user<2;++user)for(unsigned code=em2::kWheelUp;code<=em2::kWheelRight;++code) {
        pending_keys[user][code]=false;latched_valid[user][code]=false;
    }
}
std::array<X_INPUT_GAMEPAD,2> raw_pads{};
std::array<uint32_t,2> packets{};
std::array<bool,2> wait_for_pad_release{};
std::array<X_INPUT_GAMEPAD,2> prior_output{};
std::mutex controls_mutex;
thread_local bool sampling_local_controls=false;
bool online_join_controls=false,online_join_start=false;
uint64_t controls_generation=0;
uint64_t view_poll_epoch=0;
struct SynchronizedControls {
    bool active=false,has_frame=false,exhausted=false;
    uint64_t next=0;
    em2::net::Frame frame{};
    struct Sample {bool valid=false,exhausted=false;uint64_t tick=0,next=0;em2::net::Pad pad{};};
    std::array<Sample,2> samples{};
} synchronized_controls;

// Caller holds controls_mutex. Both the fallback driver and native wrapper
// use this conversion so physical-device availability cannot change a frame.
bool ReadSynchronizedControls(unsigned user,X_INPUT_STATE* out) {
    if(sampling_local_controls || !synchronized_controls.active || user>=2) return false;
    if(!out) return true;
    const auto& pad=synchronized_controls.frame.players[user];
    X_INPUT_GAMEPAD mapped{};
    mapped.buttons=pad.buttons;mapped.left_trigger=pad.left_trigger;mapped.right_trigger=pad.right_trigger;
    mapped.thumb_lx=pad.left_x;mapped.thumb_ly=pad.left_y;mapped.thumb_rx=pad.right_x;mapped.thumb_ry=pad.right_y;
    if(std::memcmp(&mapped,&prior_output[user],sizeof(mapped))) {++packets[user];prior_output[user]=mapped;}
    *out={};out->packet_number=packets[user];out->gamepad=mapped;
    return true;
}
bool ReadOnlineJoinControls(unsigned user,X_INPUT_STATE* out) {
    if(!online_join_controls || synchronized_controls.active || sampling_local_controls || user>=2)return false;
    X_INPUT_GAMEPAD mapped{};
    if(user==1 && online_join_start)mapped.buttons=0x10;
    if(std::memcmp(&mapped,&prior_output[user],sizeof(mapped))){++packets[user];prior_output[user]=mapped;}
    if(out){*out={};out->packet_number=packets[user];out->gamepad=mapped;}
    return true;
}
std::filesystem::path controls_file;
int capture=-1;
int start_capture=-1;
Clock::time_point start_capture_time{};
bool capture_armed=false,menu_open=false;
bool external_capture=false;
bool swap_control_down=false;
std::array<bool,2> chord_shoulders_blocked{};
Clock::time_point capture_started{},suppressed_until{};

void Defaults(unsigned profile) {
    look_profiles[profile]=look_defaults;
    if(profile==0) profiles[profile]=keyboard_defaults;
    else if(profile==2) profiles[profile].fill(0);
    else for(int i=0;i<em2::kBindingCount;++i) profiles[profile][i]=i<em2::kPadSourceCount?256+i:0;
}
void ModernDefaults(unsigned profile) {
    Defaults(profile);
    if(profile==0)profiles[profile]={VK_SPACE,'E','Q','P',VK_MBUTTON,'F',VK_RBUTTON,VK_LBUTTON,
        0,'G',VK_ESCAPE,VK_MENU,'1','2','3','4','A','D','W','S',VK_LEFT,VK_RIGHT,VK_UP,VK_DOWN,VK_TAB,VK_SHIFT,VK_CONTROL,'R'};
    else if(profile&1)profiles[profile]={256,261,258,267,260,270,262,263,0,265,266,0,268,269,0,271,272,273,274,275,276,277,278,279,280,264,257,259};
}
bool ui_context=false;
bool ValidLook(unsigned index,int value) {
    return index<look_sizes.size() && value>=0 && value<look_sizes[index];
}
bool ValidCode(unsigned profile,int code) {
    return code==0 || ((profile&1)?code>=256 && code<256+em2::kPadSourceCount:
        (code>0 && code<256) || (code>=em2::kWheelUp && code<=em2::kWheelRight));
}
void Save() {
    if(controls_file.empty()) return;
    auto temporary=controls_file; temporary+=".tmp";
    std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);
    stream << "# EM2 in-game control bindings. Profiles: P1 keyboard, P1 pad, P2 keyboard, P2 pad.\n";
    for(unsigned p=0;p<4;++p) for(int i=0;i<em2::kBindingCount;++i)
        stream << p << '.' << i << '=' << profiles[p][i] << '\n';
    for(unsigned p=0;p<4;++p) for(unsigned i=0;i<look_names.size();++i)
        stream << p << '.' << look_names[i] << '=' << look_profiles[p][i] << '\n';
    stream.close();
    if(!stream || !MoveFileExW(temporary.c_str(),controls_file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        REXLOG_ERROR("Unable to save control bindings: {}",controls_file.string());
}
void Assign(unsigned profile,int control,int source) {
    const auto previous=profiles[profile][control];
    if(source) for(int i=0;i<em2::kBindingCount;++i)
        if(i!=control && profiles[profile][i]==source) profiles[profile][i]=previous;
    profiles[profile][control]=source;
    Save();
}
bool KeyboardConnected(unsigned user) {
    if(online_join_controls && !sampling_local_controls && user<2)return true;
    return user<2 && (user==0 || std::any_of(profiles[user*2].begin(),profiles[user*2].end(),[](int key){return key!=0;}));
}
float PadSource(const X_INPUT_GAMEPAD& pad,int source) {
    if(source<0 || source>=em2::kPadSourceCount) return 0;
    if(source==24) return (uint16_t(pad.buttons)&0x300)==0x300?1.f:0.f;
    if(source<16 && masks[source]) return (uint16_t(pad.buttons)&masks[source])?1.f:0.f;
    if(source==6) return pad.left_trigger/255.f;
    if(source==7) return pad.right_trigger/255.f;
    const int axis=source==16||source==17?int16_t(pad.thumb_lx):
        source==18||source==19?int16_t(pad.thumb_ly):source==20||source==21?int16_t(pad.thumb_rx):int16_t(pad.thumb_ry);
    const bool negative=source==16||source==19||source==20||source==23;
    return negative?std::clamp(-axis/32768.f,0.f,1.f):std::clamp(axis/32767.f,0.f,1.f);
}
void ShapeLook(std::array<float,em2::kBindingCount>& values,const LookProfile& look) {
    // Shape each input profile before combining it with the other device.
    // This keeps keyboard settings from changing a physical controller and
    // retains the game's own camera/aim response and context handling.
    const float deadzone=std::bit_cast<float>(native_deadzone_bits.load(std::memory_order_relaxed));
    auto scale=[&](float value,float strength,bool negative,int zone,int curve) {
        if(strength==1 && !zone && !curve)return value; // Preserve defaults bit-for-bit.
        if(!std::isfinite(deadzone) || deadzone<0 || deadzone>=1)return std::min(value*strength,1.f);
        if(zone || curve) {
            const float cutoff=zone?look_deadzones[zone]:deadzone;
            if(value<=cutoff || (!zone && std::lround(value*(negative?32768.f:32767.f))<=int(deadzone*32768.f)))return 0.f;
            float useful=std::clamp((value-cutoff)/(1-cutoff),0.f,1.f);
            if(curve==1)useful*=useful;
            else if(curve==2)useful=std::sqrt(useful);
            useful=std::min(useful*strength,1.f);
            return useful>0?deadzone+(1-deadzone)*useful:0.f;
        }
        // Keep the native onset, scale its useful range, then encode back to
        // the same native input domain. Scaling raw input instead raises the
        // physical deadzone at low sensitivity and lowers it at high values.
        if(std::lround(value*(negative?32768.f:32767.f))<=int(deadzone*32768.f))return value;
        return std::min(deadzone+(value-deadzone)*strength,1.f);
    };
    for(unsigned axis=0;axis<2;++axis) {
        const unsigned negative=axis?23:20,positive=axis?22:21;
        if(look[axis+2]) std::swap(values[negative],values[positive]);
        const auto strength=look_strength[look[axis]];
        values[negative]=scale(values[negative],strength,true,look[axis+4],look[axis+6]);
        values[positive]=scale(values[positive],strength,false,look[axis+4],look[axis+6]);
    }
}
int16_t QuantizeAxis(float positive,float negative) {
    const float value=std::clamp(positive-negative,-1.f,1.f);
    return int16_t(std::lround(value*(value<0?32768.f:32767.f)));
}
bool HasShoulderChord(unsigned user) {
    const auto& profile=profiles[user*2+1];
    return std::find(profile.begin(),profile.end(),280)!=profile.end();
}
bool TestLookStrengthNativeResponse() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    struct Restore {uint8_t* base;uint32_t deadzone=native_deadzone_bits.load();unsigned csr=_mm_getcsr();
        ~Restore(){native_deadzone_bits.store(deadzone);_mm_setcsr(csr);if(base)VirtualFree(base,0,MEM_RELEASE);}}restore{base};
    if(!base)return false;
    for(uint32_t address:{0u,0x82000000u,0x820A0000u,0x833B0000u})
        if(!VirtualAlloc(base+address,0x10000,MEM_COMMIT,PAGE_READWRITE))return false;
    auto put_float=[&](uint32_t address,float value){PPC_STORE_U32(address,std::bit_cast<uint32_t>(value));};
    put_float(0x82003EA0,.5f);put_float(0x82003F48,1.f);put_float(0x820A82B8,32768.f);
    constexpr uint32_t device=0x2000,history=0x3000,state=0x4000;
    PPC_STORE_U32(device+60,history);PPC_STORE_U32(device+68,0);
    auto setup=[&](int16_t x,int16_t y) {
        for(unsigned axis=0;axis<4;++axis) {
            const auto record=history+axis*32;
            for(unsigned offset=0;offset<32;offset+=4)PPC_STORE_U32(record+offset,0);
            PPC_STORE_U32(record+16,uint32_t(-32768));PPC_STORE_U32(record+20,32767);
            PPC_STORE_U32(record+24,uint32_t(-32768));PPC_STORE_U32(record+28,32767);
        }
        PPC_STORE_U16(state+8,12345);PPC_STORE_U16(state+10,uint16_t(-23456));
        PPC_STORE_U16(state+12,uint16_t(x));PPC_STORE_U16(state+14,uint16_t(y));
        PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u64=0xF000;ctx.r3.u64=device;ctx.r4.u64=state;ctx.lr=0x12345678;return ctx;
    };
    auto read=[&]{return std::array<int32_t,4>{int32_t(PPC_LOAD_U32(history+4)),int32_t(PPC_LOAD_U32(history+36)),
        int32_t(PPC_LOAD_U32(history+68)),int32_t(PPC_LOAD_U32(history+100))};};
    bool ok=true;unsigned comparisons=0,tuning_cases=0;double max_error=0,tuning_error=0;
    auto check=[&](bool passed,const char* message){if(!passed && ok)REXLOG_ERROR("Native look strength test: {}",message);ok&=passed;};
    for(float deadzone:{0.f,.1f,.2f,native_default_deadzone,.3f,.5f}) {
        put_float(0x833B65FC,deadzone);
        auto neutral=setup(0,0);sub_830ACF88(neutral,base);const auto center=read();
        check(native_deadzone_bits.load()==std::bit_cast<uint32_t>(deadzone),"native axis entry must observe the actual configured deadzone");
        std::vector<int> inputs={-32768,-32767,-1,0,1,32766,32767};
        const int edge=int(deadzone*32768);
        for(int offset:{-1,0,1})for(int sign:{-1,1})inputs.push_back(std::clamp(sign*(edge+offset),-32768,32767));
        for(int value=-32768;value<=32767;value+=257)inputs.push_back(value);
        for(int value:inputs) {
            auto original=setup(int16_t(value),int16_t(value));__imp__sub_830ACF88(original,base);const auto baseline=read();
            for(unsigned strength=0;strength<look_strength.size();++strength) {
                X_INPUT_GAMEPAD raw{};raw.thumb_rx=raw.thumb_ry=int16_t(value);
                std::array<float,em2::kBindingCount> mapped{};
                for(unsigned i=20;i<24;++i)mapped[i]=PadSource(raw,int(i));
                ShapeLook(mapped,LookProfile{int(strength),int(strength),0,0});
                auto ctx=setup(QuantizeAxis(mapped[21],mapped[20]),QuantizeAxis(mapped[22],mapped[23]));
                sub_830ACF88(ctx,base);const auto actual=read();
                check(actual[0]==baseline[0] && actual[1]==baseline[1],"look sensitivity must preserve both native movement channels");
                for(unsigned axis=2;axis<4;++axis) {
                    const auto expected=std::clamp(double(center[axis])+double(baseline[axis]-center[axis])*look_strength[strength],-32768.,32767.);
                    const auto error=std::abs(double(actual[axis])-expected);max_error=std::max(max_error,error);
                    if(error>5 && ok)REXLOG_ERROR("Native look response mismatch: deadzone {}, raw {}, strength {}, axis {}, actual {}, expected {}",deadzone,value,look_strength[strength],axis,actual[axis],expected);
                    check(error<=5.,"published native look must scale useful input, not its deadzone");
                    if(std::abs(value)<=edge)check(actual[axis]==center[axis],"sensitivity cannot enable raw input inside the native deadzone");
                    if(strength==4)check(actual[axis]==baseline[axis],"100 percent must preserve exact native axis output");
                }
                ++comparisons;
            }
            for(unsigned zone=0;zone<look_deadzones.size();++zone)for(unsigned curve=0;curve<3;++curve) {
                if(!zone && !curve)continue;
                for(unsigned strength=0;strength<look_strength.size();++strength) {
                    X_INPUT_GAMEPAD raw{};raw.thumb_rx=raw.thumb_ry=int16_t(value);
                    std::array<float,em2::kBindingCount> mapped{};for(unsigned i=20;i<24;++i)mapped[i]=PadSource(raw,int(i));
                    ShapeLook(mapped,LookProfile{int(strength),int(strength),0,0,int(zone),int(zone),int(curve),int(curve)});
                    auto ctx=setup(QuantizeAxis(mapped[21],mapped[20]),QuantizeAxis(mapped[22],mapped[23]));
                    sub_830ACF88(ctx,base);const auto actual=read();
                    const double cutoff=zone?look_deadzones[zone]:deadzone;
                    double useful=std::clamp((std::abs(double(value)/(value<0?32768.:32767.))-cutoff)/(1-cutoff),0.,1.);
                    if(!zone && std::abs(value)<=edge)useful=0;
                    if(curve==1)useful=std::pow(useful,2);else if(curve==2)useful=std::pow(useful,.5);
                    useful=std::min(useful*look_strength[strength],1.);
                    check(actual[0]==baseline[0] && actual[1]==baseline[1],"stick tuning must preserve both movement channels");
                    for(unsigned axis=2;axis<4;++axis) {
                        const auto sign=(value<0?-1.:1.)*(axis==2?1.:-1.);
                        const auto expected=std::clamp(double(center[axis])+sign*useful*32767.5,-32768.,32768.);
                        const auto error=std::abs(actual[axis]-expected);tuning_error=std::max(tuning_error,error);
                        if(error>5 && ok)REXLOG_ERROR("Native stick tuning mismatch: native zone {}, raw {}, selected zone {}, curve {}, strength {}, axis {}, actual {}, expected {}",deadzone,value,zone,curve,strength,axis,actual[axis],expected);
                        check(error<=5,"custom deadzone/curve must reach the requested native response");
                        if(useful==0)check(actual[axis]==center[axis],"selected deadzone and zero input must remain quiet");
                    }
                    ++tuning_cases;
                }
            }
        }
        // The new observer may change only host metadata, never native state.
        auto ctx=setup(12345,-23456),native=ctx;
        std::array<uint8_t,0x10000> before{},expected{};std::memcpy(before.data(),base,before.size());
        const auto csr=_mm_getcsr();__imp__sub_830ACF88(native,base);const auto native_csr=_mm_getcsr();std::memcpy(expected.data(),base,expected.size());
        std::memcpy(base,before.data(),before.size());_mm_setcsr(csr);sub_830ACF88(ctx,base);
        check(!std::memcmp(&ctx,&native,sizeof(ctx)) && !std::memcmp(base,expected.data(),expected.size()) && _mm_getcsr()==native_csr,
            "deadzone observation must preserve full native context, memory and FP state");
    }
    if(ok)REXLOG_INFO("Native look strength passed {} real axis-publisher comparisons: all eight strengths, six native deadzones including the authored default, both look axes, threshold boundaries, movement/neutral preservation and observer context parity; maximum integer output error {}",comparisons,max_error);
    if(ok)REXLOG_INFO("Native stick tuning passed {} real axis-publisher comparisons: all eight deadzone selections, three curves, eight strengths, six native deadzones, signed boundaries and movement preservation; maximum integer output error {}",tuning_cases,tuning_error);
    return ok;
}
X_INPUT_GAMEPAD ActionPad(unsigned user,const X_INPUT_GAMEPAD& raw) {
    auto result=raw;
    if(chord_shoulders_blocked[user] || (HasShoulderChord(user) && PadSource(raw,24)>.5f))
        result.buttons=uint16_t(result.buttons)&~0x300u;
    return result;
}
em2::LiveLookInput MapLiveLook(unsigned user,const X_INPUT_GAMEPAD& raw) {
    std::array<float,em2::kBindingCount> keyboard{},pad{};
    const auto action_pad=ActionPad(user,raw);
    for(unsigned i=20;i<24;++i) {
        const auto key=profiles[user*2][i];
        keyboard[i]=key && keys[key] && !blocked_keys[key]?1.f:0.f;
        const auto source=profiles[user*2+1][i]-256;
        pad[i]=PadSource(source==24?raw:action_pad,source);
    }
    ShapeLook(keyboard,look_profiles[user*2]);
    ShapeLook(pad,look_profiles[user*2+1]);
    for(unsigned i=20;i<24;++i) keyboard[i]=std::max(keyboard[i],pad[i]);
    return {QuantizeAxis(keyboard[21],keyboard[20]),QuantizeAxis(keyboard[22],keyboard[23])};
}
X_INPUT_GAMEPAD Remap(unsigned user,const X_INPUT_GAMEPAD& raw,std::optional<uint64_t> sample_tick={},bool dispatch_actions=true) {
    if(ui_context || menu_open){
        mod_buttons[user].store(0,std::memory_order_relaxed);auto out=raw;
        if(user==0){auto down=[&](unsigned key){return keys[key] && !blocked_keys[key];};
            uint16_t buttons=uint16_t(out.buttons);
            if(down(VK_RETURN) || down(VK_SPACE))buttons|=0x1000;
            if(down(VK_ESCAPE) || down(VK_BACK))buttons|=0x2000;
            if(down(VK_UP) || down('W'))buttons|=1;if(down(VK_DOWN) || down('S'))buttons|=2;
            if(down(VK_LEFT) || down('A'))buttons|=4;if(down(VK_RIGHT) || down('D'))buttons|=8;
            out.buttons=buttons;
        }
        return out;
    }
    // A chord owns both shoulders through either release order. This state is
    // advanced only by gameplay mapping; render-only look reads stay passive.
    if(!(uint16_t(raw.buttons)&0x300))chord_shoulders_blocked[user]=false;
    else if(HasShoulderChord(user) && PadSource(raw,24)>.5f)chord_shoulders_blocked[user]=true;
    const auto epoch=sample_tick.value_or(em2::simulation_steps.load(std::memory_order_relaxed));
    auto key_down=[&](int key) {
        if(!key || blocked_keys[key]) return false;
        if(pending_keys[user][key]) {latched_epoch[user][key]=epoch;latched_valid[user][key]=true;pending_keys[user][key]=false;}
        // Preserve a press until one simulation tick consumes it. A wall-clock
        // minimum pulse would keep movement/look held after release and can
        // leak a tap into extra ticks when running above the original rate.
        return keys[key] || (latched_valid[user][key] && latched_epoch[user][key]==epoch);
    };
    std::array<float,em2::kBindingCount> values{};
    std::array<float,em2::kBindingCount> pad_values{};
    const auto action_pad=ActionPad(user,raw);
    for(int i=0;i<em2::kBindingCount;++i) {
        const int key=profiles[user*2][i];
        if(key_down(key)) values[i]=1;
        const auto source=profiles[user*2+1][i]-256;
        pad_values[i]=PadSource(source==24?raw:action_pad,source);
    }
    ShapeLook(values,look_profiles[user*2]);
    ShapeLook(pad_values,look_profiles[user*2+1]);
    for(int i=0;i<em2::kBindingCount;++i) values[i]=std::max(values[i],pad_values[i]);
    const auto move_x=QuantizeAxis(values[17],values[16]),move_y=QuantizeAxis(values[18],values[19]);
    const auto deadzone=std::bit_cast<float>(native_deadzone_bits.load(std::memory_order_relaxed));
    const int cutoff=int((std::isfinite(deadzone) && deadzone>=0 && deadzone<1?deadzone:native_default_deadzone)*32768.f);
    const bool moving=std::abs(int(move_x))>cutoff || std::abs(int(move_y))>cutoff;
    mod_buttons[user].store((values[25]>.5f?1u:0u)|(values[26]>.5f?2u:0u)|(values[27]>.5f?4u:0u)|(pad_values[25]>.5f?8u:0u)|(moving?16u:0u),std::memory_order_relaxed);
    if(user==0) {
        const bool pressed=values[24]>.5f;
        if(pressed && !swap_control_down && !menu_open && capture<0 && dispatch_actions) em2::RequestCharacterSwap();
        swap_control_down=pressed;
    }
    // Fixed menu recovery keys also work when the chosen bindings are unusable.
    if(user==0 && key_down(VK_RETURN)) values[0]=1;
    X_INPUT_GAMEPAD out{};
    uint16_t buttons=0;
    for(unsigned i=0;i<16;++i) if(values[i]>=.5f) buttons|=masks[i];
    out.buttons=buttons;
    out.left_trigger=uint8_t(std::lround(values[6]*255));
    out.right_trigger=uint8_t(std::lround(values[7]*255));
    out.thumb_lx=QuantizeAxis(values[17],values[16]); out.thumb_ly=QuantizeAxis(values[18],values[19]);
    out.thumb_rx=QuantizeAxis(values[21],values[20]); out.thumb_ry=QuantizeAxis(values[22],values[23]);
    return out;
}
bool SuppressInput(unsigned user,const X_INPUT_GAMEPAD& raw) {
    if(external_capture && capture<0)return true;
    if(wait_for_pad_release[user]) {
        bool neutral=true;
        for(int i=0;i<em2::kPadSourceCount;++i) if(PadSource(raw,i)>.25f) neutral=false;
        if(!neutral) return true;
        wait_for_pad_release[user]=false;
    }
    if(capture>=0) {
        const auto profile=unsigned(capture/32);
        const auto now=Clock::now();
        if(now-capture_started>std::chrono::seconds(10)) {
            capture=-1; suppressed_until=now+std::chrono::milliseconds(150);
        } else if((profile&1) && profile/2==user) {
            float largest=0; int selected=-1;
            for(int i=0;i<em2::kPadSourceCount;++i) if(PadSource(raw,i)>largest) {largest=PadSource(raw,i);selected=i;}
            if(largest<.25f) capture_armed=true;
            if(PadSource(raw,24)>.9f) {largest=1.f;selected=24;}
            if((uint16_t(raw.buttons)&0x30)==0x30) {
                capture=-1; suppressed_until=now+std::chrono::milliseconds(250);
                wait_for_pad_release[user]=true;
            } else if(capture_armed && now-capture_started>std::chrono::milliseconds(250) && largest>.65f) {
                if(uint16_t(raw.buttons)&0x10){
                    if(start_capture!=capture){start_capture=capture;start_capture_time=now;}
                    if(now-start_capture_time>=std::chrono::milliseconds(600)){capture=-1;suppressed_until=now+std::chrono::milliseconds(250);wait_for_pad_release[user]=true;}
                    return true;
                }
                Assign(profile,capture%32,selected+256);
                capture=-1; suppressed_until=now+std::chrono::milliseconds(250);
                wait_for_pad_release[user]=true;
            } else if(start_capture==capture && !(uint16_t(raw.buttons)&0x10)){
                Assign(profile,capture%32,266);capture=-1;suppressed_until=now+std::chrono::milliseconds(250);wait_for_pad_release[user]=true;
            }
        }
        return true;
    }
    return Clock::now()<suppressed_until;
}

class KeyboardFallback final:public InputDriver {
public:
    explicit KeyboardFallback(rex::ui::Window* window):InputDriver(window,0) {}
    X_STATUS Setup() override {return X_STATUS_SUCCESS;}
    X_RESULT GetCapabilities(uint32_t user,uint32_t,X_INPUT_CAPABILITIES* out) override {
        std::lock_guard lock(controls_mutex);
        if(!(synchronized_controls.active && user<2) && !KeyboardConnected(user)) return X_ERROR_DEVICE_NOT_CONNECTED;
        if(!out) return X_ERROR_SUCCESS;
        *out={}; out->type=1; out->sub_type=1; out->gamepad.buttons=0xF3FF;
        out->gamepad.left_trigger=out->gamepad.right_trigger=255;
        out->gamepad.thumb_lx=out->gamepad.thumb_ly=out->gamepad.thumb_rx=out->gamepad.thumb_ry=32767;
        return X_ERROR_SUCCESS;
    }
    X_RESULT GetState(uint32_t user,X_INPUT_STATE* out) override {
        std::lock_guard lock(controls_mutex);
        if(ReadSynchronizedControls(user,out)) return X_ERROR_SUCCESS;
        if(ReadOnlineJoinControls(user,out)) return X_ERROR_SUCCESS;
        if(!KeyboardConnected(user)) return X_ERROR_DEVICE_NOT_CONNECTED;
        if(!out) return X_ERROR_SUCCESS;
        *out={}; return X_ERROR_SUCCESS; // The game wrapper combines keyboard and physical pad exactly once.
    }
    X_RESULT SetState(uint32_t user,X_INPUT_VIBRATION*) override {
        std::lock_guard lock(controls_mutex);
        return ((synchronized_controls.active && user<2) || KeyboardConnected(user))?X_ERROR_SUCCESS:X_ERROR_DEVICE_NOT_CONNECTED;
    }
    X_RESULT GetKeystroke(uint32_t user,uint32_t,X_INPUT_KEYSTROKE*) override {
        std::lock_guard lock(controls_mutex);
        return ((synchronized_controls.active && user<2) || KeyboardConnected(user))?X_ERROR_EMPTY:X_ERROR_DEVICE_NOT_CONNECTED;
    }
};
}

namespace em2 {
void SetControlUIContext(bool active){std::lock_guard lock(controls_mutex);if(ui_context==active)return;ui_context=active;++view_poll_epoch;
    for(auto& buttons:mod_buttons)buttons.store(0,std::memory_order_relaxed);
    for(auto& pending:pending_keys)pending.fill(false);for(auto& valid:latched_valid)valid.fill(false);
    if(!active){wait_for_pad_release.fill(true);for(unsigned key=0;key<keys.size();++key)if(keys[key])blocked_keys[key]=true;}
}
GameplayModInput ReadGameplayModInput(unsigned user){
    if(user>=2 || ControlBindingsOpen())return {};
    const auto buttons=mod_buttons[user].load(std::memory_order_relaxed);return {bool(buttons&1),bool(buttons&2),bool(buttons&4),bool(buttons&8),bool(buttons&16)};
}
MouseLookFactors MouseLookStrength() {
    std::lock_guard lock(controls_mutex);
    return {look_strength[look_profiles[0][0]]*(look_profiles[0][2]?-1.f:1.f),
            look_strength[look_profiles[0][1]]*(look_profiles[0][3]?-1.f:1.f)};
}
bool ControlBindingsOpen() {std::lock_guard lock(controls_mutex);return external_capture || ui_context || menu_open || capture>=0;}
void SetOnlineJoinControls(bool enabled,bool press_start) {
    std::lock_guard lock(controls_mutex);
    if(online_join_controls!=enabled) {++view_poll_epoch;ResetWheelInputs();}
    online_join_controls=enabled;online_join_start=enabled && press_start;
}
bool BeginSynchronizedControls(uint64_t first_tick) {
    std::lock_guard lock(controls_mutex);
    if(synchronized_controls.active || online_join_controls || controls_generation==UINT64_MAX) return false;
    ++controls_generation;
    ++view_poll_epoch;
    ResetWheelInputs();
    synchronized_controls={};synchronized_controls.active=true;synchronized_controls.next=first_tick;
    for(auto& sample:synchronized_controls.samples) sample.next=first_tick;
    for(auto& pending:pending_keys) pending.fill(false);
    for(auto& valid:latched_valid) valid.fill(false);
    return true;
}
bool PublishSynchronizedControls(const net::Frame& frame) {
    std::lock_guard lock(controls_mutex);
    auto& state=synchronized_controls;
    if(!state.active) return false;
    if(state.has_frame && frame.tick==state.frame.tick) return frame==state.frame;
    if(state.exhausted || frame.tick!=state.next) return false;
    state.frame=frame;state.has_frame=true;
    if(state.next==UINT64_MAX) state.exhausted=true;else ++state.next;
    return true;
}
void EndSynchronizedControls() {
    std::lock_guard lock(controls_mutex);
    ++view_poll_epoch;
    ResetWheelInputs();
    synchronized_controls={};
    for(auto& pending:pending_keys) pending.fill(false);
    for(auto& valid:latched_valid) valid.fill(false);
}
bool SampleLiveLook(rex::input::InputSystem& input,unsigned user,LiveLookInput& out) {
    out={};uint64_t epoch;
    auto allowed=[&] {
        return user<2 && !synchronized_controls.active && !online_join_controls &&
            !external_capture && !ui_context && !menu_open && capture<0 && !wait_for_pad_release[user] && Clock::now()>=suppressed_until;
    };
    {
        std::lock_guard lock(controls_mutex);
        if(!allowed()) return false;
        epoch=view_poll_epoch;
    }
    // The fallback driver locks controls_mutex. Poll without it and recheck
    // ownership afterwards; a menu, focus reset or online transition during
    // a device call must not leak the old physical state into the camera.
    X_INPUT_STATE state{};
    const auto result=input.GetState(user,&state);
    std::lock_guard lock(controls_mutex);
    if(result!=X_ERROR_SUCCESS || epoch!=view_poll_epoch || !allowed()) return false;
    out=MapLiveLook(user,state.gamepad);
    return true;
}
bool SampleLocalControls(rex::input::InputSystem& input,unsigned user,uint64_t tick,net::Pad& out) {
    out={};uint64_t generation;
    auto ready=[&]() {
        if(user>=2 || !synchronized_controls.active) return false;
        const auto& sample=synchronized_controls.samples[user];
        return (sample.valid && sample.tick==tick) || (!sample.exhausted && sample.next==tick);
    };
    {
        std::lock_guard lock(controls_mutex);
        if(!ready()) return false;
        const auto& sample=synchronized_controls.samples[user];
        if(sample.valid && sample.tick==tick) {out=sample.pad;return true;}
        generation=controls_generation;
    }
    X_INPUT_STATE physical{};
    rex::X_RESULT status;
    {
        // InputSystem may call our fallback driver. Do not hold its mutex or
        // let that driver substitute the last agreed network frame here.
        struct Scope {bool prior=sampling_local_controls;Scope(){sampling_local_controls=true;}~Scope(){sampling_local_controls=prior;}} scope;
        status=input.GetState(user,&physical);
    }
    std::lock_guard lock(controls_mutex);
    if(generation!=controls_generation || !ready()) return false;
    auto& sample=synchronized_controls.samples[user];
    if(sample.valid && sample.tick==tick) {out=sample.pad;return true;}
    if(status!=X_ERROR_SUCCESS) return false;
    raw_pads[user]=physical.gamepad;
    const auto suppressed=SuppressInput(user,physical.gamepad);if(suppressed)mod_buttons[user].store(0,std::memory_order_relaxed);
    const auto pad=suppressed?X_INPUT_GAMEPAD{}:Remap(user,physical.gamepad,tick);
    sample.pad={uint16_t(pad.buttons),pad.left_trigger,pad.right_trigger,int16_t(pad.thumb_lx),int16_t(pad.thumb_ly),int16_t(pad.thumb_rx),int16_t(pad.thumb_ry)};
    sample.valid=true;sample.tick=tick;
    if(tick==UINT64_MAX) sample.exhausted=true;else sample.next=tick+1;
    out=sample.pad;return true;
}
void LoadControls(const std::filesystem::path& directory) {
    std::lock_guard lock(controls_mutex);
    ResetWheelInputs();
    ++view_poll_epoch;
    for(unsigned p=0;p<4;++p) ModernDefaults(p);
    std::filesystem::create_directories(directory);
    controls_file=directory/"controls.ini";
    std::array<std::array<bool,3>,4> explicit_mod_bindings{};
    std::ifstream stream(controls_file);
    std::string line;
    while(std::getline(stream,line)) {
        int profile=-1,index=-1,code=-1; char tail=0;
        char name[32]{};
        if(std::sscanf(line.c_str(),"%d.%31[a-z_]=%d %c",&profile,name,&code,&tail)==3 && profile>=0 && profile<4) {
            for(unsigned i=0;i<look_names.size();++i)
                if(!std::strcmp(name,look_names[i]) && ValidLook(i,code)) look_profiles[profile][i]=code;
            continue;
        }
        if(std::sscanf(line.c_str(),"%d.%d=%d %c",&profile,&index,&code,&tail)!=3) continue;
        if(profile>=0 && profile<4 && index>=0 && index<kBindingCount && ValidCode(profile,code)) {profiles[profile][index]=code;if(index>=25)explicit_mod_bindings[profile][index-25]=true;}
    }
    if(stream.is_open())stream.close();
    // Adding moves must not steal a key from an existing user's 25-action
    // profile. Explicit new bindings still use the normal conflict-swap UI.
    for(unsigned p=0;p<4;++p)for(unsigned i=25;i<kBindingCount;++i)
        if(!explicit_mod_bindings[p][i-25] && profiles[p][i] && std::find(profiles[p].begin(),profiles[p].begin()+25,profiles[p][i])!=profiles[p].begin()+25)profiles[p][i]=0;
    // Upgrade only untouched old presets. A user's customized profile stays
    // intact and can opt into the new layout from the native controls page.
    bool migrated=false;
    if(profiles[0]==keyboard_defaults || (std::equal(profiles[0].begin(),profiles[0].begin()+25,keyboard_defaults.begin()) && std::none_of(explicit_mod_bindings[0].begin(),explicit_mod_bindings[0].end(),[](bool explicit_binding){return explicit_binding;}))){ModernDefaults(0);migrated=true;}
    for(unsigned p:{1u,3u}){bool original=true;for(int i=0;i<kBindingCount;++i)original&=profiles[p][i]==(i<kPadSourceCount?256+i:0);if(original){ModernDefaults(p);migrated=true;}}
    if(migrated)Save();
    capture=-1; keys.fill(false); blocked_keys.fill(false);swap_control_down=false;chord_shoulders_blocked.fill(false);
    for(auto& buttons:mod_buttons)buttons.store(0,std::memory_order_relaxed);
    for(auto& pending:pending_keys) pending.fill(false);
    for(auto& epoch:latched_epoch) epoch.fill(UINT64_MAX);
    for(auto& valid:latched_valid) valid.fill(false);
}
void ControlKey(unsigned key,bool down) {
    if(key>=256) return;
    std::lock_guard lock(controls_mutex);
    const bool pressed=down && !keys[key];
    keys[key]=down;
    if(!down) {
        blocked_keys[key]=false;
        if(capture>=0 && !(capture/32&1) && std::none_of(keys.begin(),keys.end(),[](bool value){return value;})) capture_armed=true;
    }
    if(!pressed) return;
    NotifyMouseKeyboardInput();
    if(external_capture && capture<0)return;
    static bool reported=false;
    if(!reported) {REXLOG_INFO("PC keyboard event listener is receiving input");reported=true;}
    if(capture<0) for(unsigned user=0;user<2;++user) {
        const auto& bindings=profiles[user*2];
        if((user==0 && key==VK_RETURN) || std::find(bindings.begin(),bindings.end(),int(key))!=bindings.end()) pending_keys[user][key]=true;
    }
    if(capture>=0) {
        if(key==VK_ESCAPE) capture=-1;
        else if(!(capture/32&1) && capture_armed && Clock::now()-capture_started>std::chrono::milliseconds(250)) {
            Assign(capture/32,capture%32,int(key)); capture=-1;
        } else return;
        blocked_keys[key]=true; suppressed_until=Clock::now()+std::chrono::milliseconds(150);
    } else if(menu_open && key==VK_F8) {
        for(unsigned p=0;p<4;++p) ModernDefaults(p);
        Save();
    }
}
void ControlWheel(int horizontal,int vertical) {
    std::lock_guard lock(controls_mutex);
    const auto now=Clock::now();
    if(capture>=0) {
        if(now-capture_started>std::chrono::seconds(10)) {
            capture=-1;suppressed_until=now+std::chrono::milliseconds(150);ResetWheelInputs();return;
        }
        if((capture/32&1) || !capture_armed || now-capture_started<=std::chrono::milliseconds(250)) {
            wheel_remainder={};return;
        }
    }else if(external_capture || menu_open || online_join_controls || now<suppressed_until) {ResetWheelInputs();return;}
    if(capture<0 && (horizontal || vertical))NotifyMouseKeyboardInput();
    const std::array<int,2> delta={horizontal,vertical};
    for(unsigned axis=0;axis<2;++axis) {
        wheel_remainder[axis]+=delta[axis];
        const auto detents=wheel_remainder[axis]/120;wheel_remainder[axis]%=120;
        if(!detents)continue;
        const int code=axis?(detents>0?kWheelUp:kWheelDown):(detents>0?kWheelRight:kWheelLeft);
        if(capture>=0) {
            Assign(capture/32,capture%32,code);capture=-1;
            suppressed_until=now+std::chrono::milliseconds(150);ResetWheelInputs();return;
        }
        // Repeated packets in one simulation tick intentionally coalesce, as
        // keyboard taps do. Polling again in that tick does not consume a pulse.
        for(unsigned user=0;user<2;++user)
            if(std::find(profiles[user*2].begin(),profiles[user*2].end(),code)!=profiles[user*2].end())pending_keys[user][code]=true;
    }
}
void ClearControlKeys() {
    std::lock_guard lock(controls_mutex);
    ++view_poll_epoch;
    keys.fill(false); blocked_keys.fill(false); capture=-1;swap_control_down=false;
    for(auto& buttons:mod_buttons)buttons.store(0,std::memory_order_relaxed);
    for(auto& pending:pending_keys) pending.fill(false);
    for(auto& epoch:latched_epoch) epoch.fill(UINT64_MAX);
    for(auto& valid:latched_valid) valid.fill(false);
    ResetWheelInputs();
}
void SetExternalControlCapture(bool captured){
    std::lock_guard lock(controls_mutex);if(external_capture==captured)return;
    external_capture=captured;++view_poll_epoch;
    for(auto& buttons:mod_buttons)buttons.store(0,std::memory_order_relaxed);
    if(!captured)for(unsigned code=0;code<keys.size();++code)if(keys[code])blocked_keys[code]=true;
    for(auto& pending:pending_keys)pending.fill(false);
    for(auto& valid:latched_valid)valid.fill(false);
    ResetWheelInputs();wait_for_pad_release.fill(true);
    if(!captured)capture=-1;
}
bool ControlOption(int id,int command,int& result) {
    std::lock_guard lock(controls_mutex);
    if(id==2200) {result=capture+1;return true;}
    if(id==2201) {++view_poll_epoch;ResetWheelInputs();menu_open=command!=0; if(!menu_open) capture=-1;result=0;return true;}
    if(id==2210){if(command<0 || command>=4)return false;ModernDefaults(unsigned(command));++view_poll_epoch;Save();result=command;return true;}
    if(id>=2300 && id<2332) {
        const auto profile=unsigned(id-2300)/8,index=unsigned(id-2300)%8;
        if(command==-3) {look_profiles[profile][index]=look_defaults[index];Save();}
        else if(command!=-1) {
            if(!ValidLook(index,command)) return false;
            look_profiles[profile][index]=command;Save();
        }
        result=look_profiles[profile][index];return true;
    }
    const int slot=id-2000,profile=slot/32,index=slot%32;
    if(slot<0 || profile>=4 || index>=kBindingCount) return false;
    if(command>=0 && !ValidCode(profile,command))return false;
    result=profiles[profile][index];
    if(command==-1) return true;
    if(command==-2) {
        ResetWheelInputs();
        ++view_poll_epoch;
        capture=slot;start_capture=-1; capture_started=Clock::now();
        for(auto& pending:pending_keys) pending.fill(false);
        for(auto& epoch:latched_epoch) epoch.fill(UINT64_MAX);
        capture_armed=!(profile&1) && std::none_of(keys.begin(),keys.end(),[](bool value){return value;});
    } else if(command==-3) Assign(profile,index,0);
    else if(command==-4) {ModernDefaults(profile);Save();}
    else if(command==-5) capture=-1;
    else if(command>=0 && ValidCode(profile,command)) Assign(profile,index,command);
    result=profiles[profile][index];
    return true;
}
std::unique_ptr<rex::input::InputDriver> CreateKeyboardFallback(rex::ui::Window* window) {
    return std::make_unique<KeyboardFallback>(window);
}
static bool TestLiveLook() {
    bool ok=true;
    auto check=[&](bool condition,const char* message) {
        if(!condition){REXLOG_ERROR("Live camera input test: {}",message);ok=false;}
    };
    struct Restore {
        decltype(profiles) bindings=profiles;
        decltype(look_profiles) look=look_profiles;
        decltype(chord_shoulders_blocked) chord=chord_shoulders_blocked;
        ~Restore() {
            SetOnlineJoinControls(false);EndSynchronizedControls();ClearControlKeys();
            profiles=bindings;look_profiles=look;chord_shoulders_blocked=chord;menu_open=false;wait_for_pad_release.fill(false);suppressed_until={};
        }
    } restore;
    for(unsigned p=0;p<4;++p)Defaults(p);
    suppressed_until={};menu_open=false;wait_for_pad_release.fill(false);
    class Device final:public rex::input::InputDriver {
    public:
        std::array<X_INPUT_GAMEPAD,2> pads{};
        bool connected=true;unsigned reads=0;std::function<void()> on_poll;
        Device():InputDriver(nullptr,0){}
        rex::X_STATUS Setup() override{return X_STATUS_SUCCESS;}
        rex::X_RESULT GetCapabilities(uint32_t,uint32_t,X_INPUT_CAPABILITIES*) override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetState(uint32_t user,X_INPUT_STATE* state) override {
            ++reads;if(on_poll)on_poll();
            if(!connected || user>=2)return X_ERROR_DEVICE_NOT_CONNECTED;
            if(state){*state={};state->gamepad=pads[user];}return X_ERROR_SUCCESS;
        }
        rex::X_RESULT SetState(uint32_t,X_INPUT_VIBRATION*) override{return X_ERROR_DEVICE_NOT_CONNECTED;}
        rex::X_RESULT GetKeystroke(uint32_t,uint32_t,X_INPUT_KEYSTROKE*) override{return X_ERROR_EMPTY;}
    };
    rex::input::InputSystem input(nullptr);
    auto driver=std::make_unique<Device>();auto* device=driver.get();input.AddDriver(std::move(driver));
    LiveLookInput look{};
    ControlKey('L',true);ControlKey(VK_SPACE,true);ControlKey(VK_SPACE,false);ControlKey(VK_F6,true);
    const auto pending=pending_keys;const auto latched=latched_epoch;const auto valid=latched_valid;
    const auto raw=raw_pads;const auto prior=prior_output;const auto packet=packets;const bool swap=swap_control_down;
    const auto chord=chord_shoulders_blocked;
    for(unsigned frame=0;frame<240;++frame)
        check(SampleLiveLook(input,0,look) && look==LiveLookInput{32767,0},"current look is readable between simulation ticks");
    check(pending==pending_keys && latched==latched_epoch && valid==latched_valid && packet==packets && swap==swap_control_down && chord==chord_shoulders_blocked &&
        !std::memcmp(raw.data(),raw_pads.data(),sizeof(raw)) && !std::memcmp(prior.data(),prior_output.data(),sizeof(prior)),
        "view reads leave pending taps, tick latches, swap latch, packet numbers and cached pads unchanged");
    ControlKey('L',false);ControlKey(VK_F6,false);
    check(SampleLiveLook(input,0,look) && look==LiveLookInput{},"released look stops within the same simulation tick");
    menu_open=false;
    const auto mapped=Remap(0,{}, {},false);
    menu_open=false;
    check((uint16_t(mapped.buttons)&0x1000) && int16_t(mapped.thumb_rx)==32767,
        "view sampling did not consume pending gameplay or look taps");
    ClearControlKeys();
    // Compare the view-only mapping with the real gameplay mapper for every
    // rebindable pad source, both slots, gains/inversion and signed extrema.
    // No elapsed wall time or synthetic camera pose is involved in this check.
    menu_open=false; // Compare gameplay mapping without dispatching actions.
    for(unsigned user=0;user<2;++user) for(unsigned source=0;source<kBindingCount;++source) {
        for(unsigned i=20;i<24;++i)profiles[user*2+1][i]=256+(source+i-20)%kBindingCount;
        for(unsigned variant=0;variant<8;++variant) {
            for(unsigned p=0;p<2;++p)look_profiles[user*2+p]={int(variant),int(7-variant),int(variant&1),int((variant>>1)&1),int(variant),int(7-variant),int(variant%3),int((variant+1)%3)};
            auto& pad=device->pads[user];pad.buttons=variant&1?0xF3FF:0xF0FF;
            pad.left_trigger=123;pad.right_trigger=241;
            pad.thumb_lx=-32768;pad.thumb_ly=32767;pad.thumb_rx=variant&1?-12345:32767;pad.thumb_ry=variant&2?-32768:718;
            keys['L']=variant&1;keys['K']=variant&2;
            const auto expected=Remap(user,pad,{},false);
            menu_open=false;
            check(SampleLiveLook(input,user,look) && look==LiveLookInput{int16_t(expected.thumb_rx),int16_t(expected.thumb_ry)},
                "live mapping matches gameplay rebinding, chord suppression and per-device response");
            menu_open=false;
            for(auto& pending:pending_keys)pending.fill(false);
            for(auto& latches:latched_valid)latches.fill(false);
        }
    }
    menu_open=false;ClearControlKeys();for(unsigned p=0;p<4;++p)Defaults(p);
    device->pads[0]={};device->pads[0].thumb_rx=32767;
    auto rejected=[&](const char* message) {
        look={123,456};check(!SampleLiveLook(input,0,look) && look==LiveLookInput{},message);
    };
    int ignored;
    ControlOption(2201,1,ignored);rejected("open controls menu suppresses live look");ControlOption(2201,0,ignored);
    ControlOption(2000,-2,ignored);rejected("binding capture suppresses live look");ControlOption(2000,-5,ignored);
    wait_for_pad_release[0]=true;rejected("binding release gate is respected without consuming it");
    check(wait_for_pad_release[0],"view reads cannot clear capture's release gate");wait_for_pad_release[0]=false;
    suppressed_until=Clock::now()+std::chrono::seconds(2);rejected("post-binding suppression is respected");suppressed_until={};
    SetOnlineJoinControls(true);rejected("native online preparation retains control ownership");SetOnlineJoinControls(false);
    check(BeginSynchronizedControls(42),"begin canonical ownership test");
    net::Frame canonical{42,{{{},{}}}};canonical.players[0].right_x=-30000;
    check(PublishSynchronizedControls(canonical),"publish canonical ownership fixture");
    rejected("live devices cannot replace an agreed online frame");
    X_INPUT_STATE agreed{};check(ReadSynchronizedControls(0,&agreed) && int16_t(agreed.gamepad.thumb_rx)==-30000,
        "canonical online axes remain unchanged");EndSynchronizedControls();
    device->on_poll=[] {ClearControlKeys();};rejected("focus reset during a device poll invalidates its result");
    device->on_poll=[] {int unused;ControlOption(2201,1,unused);ControlOption(2201,0,unused);};
    rejected("transient menu ownership during a device poll invalidates its result");
    device->on_poll=[] {BeginSynchronizedControls(42);EndSynchronizedControls();};
    rejected("online start/stop during a device poll invalidates its result");
    device->on_poll={};device->connected=false;rejected("disconnected device returns neutral failure");
    const auto reads=device->reads;
    check(!SampleLiveLook(input,2,look) && !SampleLiveLook(input,UINT32_MAX,look) && device->reads==reads,
        "invalid player slots never reach input drivers");
    rex::input::InputSystem keyboard(nullptr);keyboard.AddDriver(CreateKeyboardFallback(nullptr));
    ControlKey('J',true);
    check(SampleLiveLook(keyboard,0,look) && look==LiveLookInput{-32768,0},"actual keyboard fallback is polled without recursive locking");
    ControlKey('J',false);
    check(SampleLiveLook(keyboard,0,look) && look==LiveLookInput{},"keyboard fallback release is immediate");
    if(ok)REXLOG_INFO("Live camera input tests passed: fresh look, mapping parity, action isolation and ownership changes");
    return ok;
}
static bool TestWheelBindings(const std::filesystem::path& directory) {
    bool ok=true;auto check=[&](bool value,const char* text){if(!value){REXLOG_ERROR("Wheel binding test: {}",text);ok=false;}};
    const auto saved_profiles=profiles;const auto saved_look=look_profiles;
    struct Restore {
        decltype(profiles) bindings;decltype(look_profiles) look;
        ~Restore(){EndSynchronizedControls();SetOnlineJoinControls(false);ClearControlKeys();profiles=bindings;look_profiles=look;menu_open=false;suppressed_until={};}
    } restore{saved_profiles,saved_look};
    EndSynchronizedControls();SetOnlineJoinControls(false);ClearControlKeys();
    for(unsigned p=0;p<4;++p)Defaults(p);menu_open=false;suppressed_until={};wait_for_pad_release.fill(false);
    int result=0;
    for(int i=0;i<4;++i)check(ControlOption(2000+i,kWheelUp+i,result) && result==kWheelUp+i,"assign every wheel direction");
    LoadControls(directory);
    for(int i=0;i<4;++i)check(ControlOption(2000+i,-1,result) && result==kWheelUp+i,"wheel direction survives config reload");
    auto sample=[&](unsigned user,uint64_t tick){std::lock_guard lock(controls_mutex);return Remap(user,{},tick);};
    ControlWheel(0,30);ControlWheel(0,30);ControlWheel(0,30);
    check(uint16_t(sample(0,10).buttons)==0,"partial detents do not press a button");
    ControlWheel(0,30);
    check(uint16_t(sample(0,10).buttons)==0x1000 && uint16_t(sample(0,10).buttons)==0x1000 && uint16_t(sample(0,11).buttons)==0,
          "four quarter-detents press once for a whole tick and release on the next");
    for(unsigned direction=0;direction<4;++direction) {
        ClearControlKeys();const int x=direction==2?-120:direction==3?120:0,y=direction==0?120:direction==1?-120:0;
        ControlWheel(x,y);
        check(uint16_t(sample(0,20+direction*2).buttons)==uint16_t(0x1000u<<direction) && uint16_t(sample(0,21+direction*2).buttons)==0,"all wheel signs map to the assigned buttons without a held state");
    }
    ClearControlKeys();ControlWheel(0,60);ControlWheel(0,-60);
    check(uint16_t(sample(0,40).buttons)==0,"opposite fractional movement cancels");
    ControlWheel(0,INT32_MAX);check(uint16_t(sample(0,41).buttons)==0x1000,"large positive wheel deltas stay bounded");
    ClearControlKeys();ControlWheel(0,INT32_MIN);check(uint16_t(sample(0,42).buttons)==0x2000,"large negative wheel deltas stay bounded");
    ClearControlKeys();ControlWheel(0,120);ClearControlKeys();
    check(uint16_t(sample(0,43).buttons)==0,"focus reset removes a pending pulse");
    ControlWheel(0,60);ClearControlKeys();ControlWheel(0,60);
    check(uint16_t(sample(0,44).buttons)==0,"focus reset removes a fractional detent");
    ClearControlKeys();ControlOption(2201,1,result);ControlWheel(0,120);ControlOption(2201,0,result);
    check(uint16_t(sample(0,45).buttons)==0,"binding menu scrolling cannot leak into gameplay");
    ControlOption(2201,1,result);ControlOption(2000,-2,result);
    {std::lock_guard lock(controls_mutex);capture_started=Clock::now()-std::chrono::milliseconds(300);}
    ControlWheel(-120,0);ControlOption(2000,-1,result);check(result==kWheelLeft,"keyboard capture accepts horizontal wheel input");
    ControlOption(2200,-1,result);check(result==0,"wheel capture completes");
    ControlOption(2201,0,result);suppressed_until={};check(uint16_t(sample(0,46).buttons)==0,"capture consumes its wheel gesture");
    ControlKey(VK_RETURN,true);ControlOption(2000,-2,result);
    {std::lock_guard lock(controls_mutex);capture_started=Clock::now()-std::chrono::milliseconds(300);}
    ControlWheel(0,120);ControlOption(2200,-1,result);check(result!=0,"capture waits for the opening key release");
    ControlKey(VK_RETURN,false);ControlWheel(0,120);ControlOption(2000,-1,result);check(result==kWheelUp,"wheel can bind after the opening key is released");
    ClearControlKeys();suppressed_until={};ControlOption(2032,-2,result);
    {std::lock_guard lock(controls_mutex);capture_started=Clock::now()-std::chrono::milliseconds(300);}
    ControlWheel(0,120);ControlOption(2200,-1,result);check(result==33,"mouse wheel cannot bind a controller profile");
    ControlOption(2032,-5,result);ClearControlKeys();ControlOption(2000,-2,result);
    {std::lock_guard lock(controls_mutex);capture_started=Clock::now()-std::chrono::seconds(11);}
    ControlWheel(0,120);ControlOption(2200,-1,result);check(result==0,"wheel capture follows the timeout");
    ClearControlKeys();suppressed_until={};
    check(!ControlOption(2000,511,result) && !ControlOption(2000,516,result) && !ControlOption(2032,kWheelUp,result),"reserved and wrong-device source codes are rejected");
    ControlOption(2000,kWheelUp,result);ControlOption(2002,kWheelLeft,result);ControlOption(2002,kWheelUp,result);
    ControlOption(2000,-1,result);check(result==kWheelLeft,"wheel conflicts swap existing assignments");
    ControlOption(2064,kWheelUp,result);ClearControlKeys();ControlWheel(0,120);
    check(uint16_t(sample(1,50).buttons)==0x1000,"player two can bind a wheel direction independently");
    ClearControlKeys();ControlWheel(0,60);check(BeginSynchronizedControls(100),"online ownership begins for wheel reset test");
    EndSynchronizedControls();ControlWheel(0,60);check(uint16_t(sample(1,51).buttons)==0,"online ownership changes clear fractional wheel input");
    ClearControlKeys();
    rex::input::InputSystem local_input(nullptr);local_input.AddDriver(CreateKeyboardFallback(nullptr));
    check(BeginSynchronizedControls(300),"canonical wheel sampling begins");ControlWheel(0,120);
    net::Pad wheel_pad{},again{};
    check(SampleLocalControls(local_input,1,300,wheel_pad) && wheel_pad.buttons==0x1000 &&
          SampleLocalControls(local_input,1,300,again) && again==wheel_pad,"outgoing wheel pulse is sampled exactly once per network tick");
    net::Frame agreed{};agreed.tick=300;agreed.players[1]=wheel_pad;
    check(PublishSynchronizedControls(agreed),"ordinary pad frame can carry the wheel action");
    X_INPUT_STATE canonical{};
    check(local_input.GetState(1,&canonical)==X_ERROR_SUCCESS && uint16_t(canonical.gamepad.buttons)==0x1000,
          "canonical controller exposes the wheel-bound action without a new wire format");
    check(SampleLocalControls(local_input,1,301,again) && again.buttons==0,"wheel release is sampled without echoing the agreed frame");
    agreed.tick=301;agreed.players[1]=again;
    check(PublishSynchronizedControls(agreed) && local_input.GetState(1,&canonical)==X_ERROR_SUCCESS && uint16_t(canonical.gamepad.buttons)==0,
          "agreed wheel release reaches the controller");
    EndSynchronizedControls();
    ClearControlKeys();ControlOption(2000,-4,result);ControlOption(2000,-1,result);check(result==VK_SPACE,"profile reset restores the original keyboard defaults");
    if(ok)REXLOG_INFO("Wheel binding tests passed: all directions, high-resolution detents, tick-consistent pulses/release, capture/timeout, focus/menu/online clearing, independent players, persistence, conflicts and bounds (synthetic events; no gameplay input)");
    return ok;
}
static bool TestChordRebinding() {
    struct Restore {
        decltype(profiles) bindings=profiles;decltype(look_profiles) look=look_profiles;bool menu=menu_open,swap=swap_control_down;
        ~Restore(){ClearControlKeys();std::lock_guard lock(controls_mutex);menu_open=true;
            chord_shoulders_blocked.fill(false);profiles=bindings;look_profiles=look;menu_open=menu;swap_control_down=swap;Save();}
    }restore;
    std::lock_guard lock(controls_mutex);
    keys.fill(false);blocked_keys.fill(false);for(auto& p:pending_keys)p.fill(false);for(auto& p:latched_valid)p.fill(false);
    menu_open=false; // Exercise gameplay mapping; dispatch is disabled explicitly below.
    auto require=[](bool value,unsigned user,unsigned action,const char* reason){
        if(!value)REXLOG_ERROR("Rebound chord test: user {}, action {}: {}",user,action,reason);return value;};
    auto RemapChord=[](unsigned user,const X_INPUT_GAMEPAD& pad){return Remap(user,pad,{},false);};
    unsigned combinations=0;
    for(unsigned user=0;user<2;++user)for(unsigned destination=0;destination<kBindingCount;++destination) {
        for(unsigned p=0;p<4;++p)Defaults(p);
        RemapChord(0,{});RemapChord(1,{});swap_control_down=false;
        Assign(user*2+1,destination,280);
        X_INPUT_GAMEPAD raw{};raw.buttons=0x300;
        const auto output=RemapChord(user,raw);
        for(unsigned action=0;action<24;++action)
            if(!require(PadSource(output,action)==float(action==destination),user,destination,
                "chord must fire only its destination, including when conflict-swapped out of character switching"))return false;
        if(user==0 && !require(swap_control_down==(destination==24),user,destination,"only a chord still bound to character switching may latch it"))return false;
        const auto look=MapLiveLook(user,raw);
        if(!require(look.x==int16_t(output.thumb_rx) && look.y==int16_t(output.thumb_ry),user,destination,
            "view mapping must agree for chords assigned to camera directions"))return false;
        for(uint16_t remaining:{0x100u,0x200u}) {
            raw.buttons=0x300;RemapChord(user,raw);raw.buttons=remaining;
            const auto release=RemapChord(user,raw);
            if(!require(release.buttons==0 && !release.left_trigger && !release.right_trigger && !release.thumb_lx &&
                !release.thumb_ly && !release.thumb_rx && !release.thumb_ry,user,destination,
                "releasing either half of a chord must not dispatch a spare shoulder action"))return false;
            raw.buttons=0;RemapChord(user,raw);raw.buttons=remaining;
            const auto fresh=RemapChord(user,raw);
            const int source=remaining==0x100?260:261;
            const auto& binding=profiles[user*2+1];const auto target=std::find(binding.begin(),binding.end(),source)-binding.begin();
            for(unsigned action=0;action<24;++action)if(!require(PadSource(fresh,action)==float(action==target),user,destination,
                "a fresh shoulder press after full release must work with its conflict-swapped binding"))return false;
            raw.buttons=0;RemapChord(user,raw);
        }
        ++combinations;
    }
    for(unsigned p=0;p<4;++p)Defaults(p);
    RemapChord(0,{});RemapChord(1,{});
    X_INPUT_GAMEPAD raw{};raw.buttons=0x300;RemapChord(0,raw);raw.buttons=0x100;
    if(!require(uint16_t(RemapChord(1,raw).buttons)==0x100,1,4,"one player's consumed chord must not suppress the other player's shoulders"))return false;
    RemapChord(0,{});RemapChord(1,{});profiles[1][24]=0;raw.buttons=0x300;
    if(!require(uint16_t(RemapChord(0,raw).buttons)==0x300,0,24,"unassigned chords must leave individual shoulder actions available"))return false;
    REXLOG_INFO("Rebound chord mapping/release passed {} user/destination combinations, camera agreement, both release orders, user isolation and unbound fallback",combinations);
    return true;
}
bool TestControls(const std::filesystem::path& directory) {
    LoadControls(directory);
    bool ok=TestChordRebinding() && TestLiveLook();
    auto check=[&](bool condition,const char* message){if(!condition){REXLOG_ERROR("Control test: {}",message);ok=false;}};
    {
        int selected=0;ControlOption(2210,0,selected);ControlOption(2210,1,selected);
        check(profiles[0][7]==VK_LBUTTON && profiles[0][6]==VK_RBUTTON && profiles[0][25]==VK_SHIFT && profiles[0][26]==VK_CONTROL && profiles[0][27]=='R',"modern keyboard preset keeps paint/thinner and movement actions deliberate");
        {std::lock_guard lock(controls_mutex);X_INPUT_GAMEPAD pad{};pad.buttons=0x2000;const auto mapped=Remap(0,pad);check(!(uint16_t(mapped.buttons)&0x2000) && (mod_buttons[0].load()&2),"modern controller B dashes without sending the original co-op action");}
        SetControlUIContext(true);
        {std::lock_guard lock(controls_mutex);X_INPUT_GAMEPAD pad{};pad.buttons=0x3000;check(uint16_t(Remap(0,pad).buttons)==0x3000 && !mod_buttons[0].load(),"native menu A/B remain physical and never activate gameplay modifications");}
        SetControlUIContext(false);ClearControlKeys();
        {std::lock_guard lock(controls_mutex);X_INPUT_GAMEPAD neutral{};SuppressInput(0,neutral);SuppressInput(1,neutral);}
    }
    {
        struct Reset {~Reset(){SetOnlineJoinControls(false);EndSynchronizedControls();ClearControlKeys();}} reset;
        auto driver=CreateKeyboardFallback(nullptr);
        check(driver->GetCapabilities(1,0,nullptr)==X_ERROR_DEVICE_NOT_CONNECTED,"unassigned second keyboard starts disconnected");
        SetOnlineJoinControls(true,true);
        X_INPUT_STATE joining{},parked{};
        check(driver->GetCapabilities(1,0,nullptr)==X_ERROR_SUCCESS && driver->GetState(1,&joining)==X_ERROR_SUCCESS &&
            uint16_t(joining.gamepad.buttons)==0x10 && driver->GetState(0,&parked)==X_ERROR_SUCCESS &&
            uint16_t(parked.gamepad.buttons)==0 && !BeginSynchronizedControls(0),
            "native online preparation exposes P2 Start, parks P0 and cannot overlap canonical gameplay");
        sampling_local_controls=true;
        check(driver->GetState(1,&joining)==X_ERROR_DEVICE_NOT_CONNECTED,"preparation's virtual controller cannot become an outgoing physical input source");
        sampling_local_controls=false;
        SetOnlineJoinControls(true,false);
        check(driver->GetState(1,&joining)==X_ERROR_SUCCESS && uint16_t(joining.gamepad.buttons)==0,
            "cutscene deferral releases Start while retaining the preparation controller");
        SetOnlineJoinControls(false);
        check(driver->GetCapabilities(1,0,nullptr)==X_ERROR_DEVICE_NOT_CONNECTED,"cancelling preparation removes the temporary controller without changing bindings");
        constexpr uint64_t tick=0x100000001ull;
        check(BeginSynchronizedControls(tick) && !BeginSynchronizedControls(tick),"online controller activation cannot silently reset a session");
        X_INPUT_CAPABILITIES caps{};X_INPUT_STATE neutral{};
        check(driver->GetCapabilities(1,0,&caps)==X_ERROR_SUCCESS && caps.type==1 &&
            driver->GetState(1,&neutral)==X_ERROR_SUCCESS && uint16_t(neutral.gamepad.buttons)==0,
            "remote controller exists without physical pad and starts neutral");
        check(driver->GetCapabilities(2,0,nullptr)==X_ERROR_DEVICE_NOT_CONNECTED,"online bridge exposes exactly two player slots");
        net::Frame agreed{tick,{{{0x1000,123,255,-32768,32767,-12345,718},{0x2000,91,7,321,-654,32767,-32768}}}};
        auto wrong=agreed;wrong.tick++;
        check(!PublishSynchronizedControls(wrong) && PublishSynchronizedControls(agreed) && PublishSynchronizedControls(agreed),
            "online bridge requires next tick and accepts only identical retries");
        wrong=agreed;wrong.players[1].buttons^=0x1000;
        check(!PublishSynchronizedControls(wrong),"published input cannot be changed within a tick");
        rex::input::InputSystem local_input(nullptr);
        local_input.AddDriver(CreateKeyboardFallback(nullptr));
        net::Pad sampled{};
        check(SampleLocalControls(local_input,0,tick,sampled) && sampled==net::Pad{},
            "outgoing keyboard sample does not echo the agreed frame");
        check(!SampleLocalControls(local_input,1,tick,sampled) && sampled==net::Pad{},
            "virtual remote presence cannot become a local input source");
        ControlKey(VK_SPACE,true);ControlKey(VK_SPACE,false);
        check(SampleLocalControls(local_input,0,tick,sampled) && sampled==net::Pad{},
            "new events cannot change an already sampled tick");
        check(SampleLocalControls(local_input,0,tick+1,sampled) && sampled.buttons==0x1000,
            "quick local tap is captured on the next outgoing tick");
        check(SampleLocalControls(local_input,0,tick+2,sampled) && sampled.buttons==0,
            "network tap releases on the next sample even while simulation is waiting");
        check(!SampleLocalControls(local_input,0,tick+4,sampled) && !SampleLocalControls(local_input,0,tick,sampled) &&
            SampleLocalControls(local_input,0,tick+3,sampled),"sampling rejects skipped/stale ticks without consuming the next tick");
        class PhysicalPad final:public rex::input::InputDriver {
        public:
            X_INPUT_GAMEPAD pad{};unsigned reads=0;bool restart=false;uint64_t restart_tick=0;
            PhysicalPad():InputDriver(nullptr,0){}
            rex::X_STATUS Setup() override{return X_STATUS_SUCCESS;}
            rex::X_RESULT GetCapabilities(uint32_t,uint32_t,X_INPUT_CAPABILITIES*) override{return X_ERROR_DEVICE_NOT_CONNECTED;}
            rex::X_RESULT GetState(uint32_t user,X_INPUT_STATE* state) override {
                ++reads;if(user) return X_ERROR_DEVICE_NOT_CONNECTED;
                if(restart) {restart=false;EndSynchronizedControls();BeginSynchronizedControls(restart_tick);}
                if(state) {*state={};state->gamepad=pad;}return X_ERROR_SUCCESS;
            }
            rex::X_RESULT SetState(uint32_t,X_INPUT_VIBRATION*) override{return X_ERROR_DEVICE_NOT_CONNECTED;}
            rex::X_RESULT GetKeystroke(uint32_t,uint32_t,X_INPUT_KEYSTROKE*) override{return X_ERROR_EMPTY;}
        };
        rex::input::InputSystem physical_input(nullptr);
        auto physical=std::make_unique<PhysicalPad>();auto* physical_source=physical.get();
        const auto& p=agreed.players[0];
        physical_source->pad.buttons=p.buttons;physical_source->pad.left_trigger=p.left_trigger;physical_source->pad.right_trigger=p.right_trigger;
        physical_source->pad.thumb_lx=p.left_x;physical_source->pad.thumb_ly=p.left_y;physical_source->pad.thumb_rx=p.right_x;physical_source->pad.thumb_ry=p.right_y;
        physical_input.AddDriver(std::move(physical));
        check(SampleLocalControls(physical_input,0,tick+4,sampled) && sampled==p,
            "outgoing physical sample preserves trigger and signed analog magnitudes");
        const auto reads=physical_source->reads;physical_source->pad={};
        check(SampleLocalControls(physical_input,0,tick+4,sampled) && sampled==p && physical_source->reads==reads,
            "repeated sampling does not repoll a changing physical device");
        // Exercise the actual guest wrapper with no kernel or physical input
        // system available: success must come from the agreed frame alone.
        alignas(16) std::array<uint8_t,4096> guest{};auto* base=guest.data();
        constexpr uint32_t output=256;
        ControlKey('W',true);ControlKey(VK_SPACE,true);
        int ignored=0;ControlOption(2032+20,256+21,ignored);
        for(unsigned user=0;user<2;++user) {
            PPCContext context{};context.r3.u64=user;context.r4.u64=output;context.lr=0x12345678;context.r9.u64=0xABCDEF;
            auto expected=context;expected.r3.u64=0;
            sub_831BB758(context,base);
            const auto first=*reinterpret_cast<X_INPUT_STATE*>(guest.data()+output);
            const auto& p=agreed.players[user];const auto& pad=first.gamepad;
            check(!std::memcmp(&context,&expected,sizeof(context)),"online native wrapper preserves unrelated guest context");
            check(uint16_t(pad.buttons)==p.buttons && pad.left_trigger==p.left_trigger && pad.right_trigger==p.right_trigger &&
                int16_t(pad.thumb_lx)==p.left_x && int16_t(pad.thumb_ly)==p.left_y && int16_t(pad.thumb_rx)==p.right_x && int16_t(pad.thumb_ry)==p.right_y,
                "canonical online input bypasses local remapping and preserves signed analog extrema");
            context.r3.u64=user;sub_831BB758(context,base);
            check(!std::memcmp(&first,guest.data()+output,sizeof(first)),"multiple native polls return identical input and packet number");
        }
        ControlOption(2032,-4,ignored);
        agreed.tick++;agreed.players[1].buttons=0;
        check(PublishSynchronizedControls(agreed),"next agreed frame replaces the held frame");
        EndSynchronizedControls();
        check(!PublishSynchronizedControls(agreed) && driver->GetCapabilities(1,0,nullptr)==X_ERROR_DEVICE_NOT_CONNECTED,
            "disconnect removes virtual controller presence and rejects stale frames");
        check(BeginSynchronizedControls(UINT64_MAX),"new session resets input timeline");
        agreed.tick=UINT64_MAX;
        check(PublishSynchronizedControls(agreed),"last representable input tick accepted");
        agreed.tick=0;check(!PublishSynchronizedControls(agreed),"online controller timeline cannot wrap");
        ClearControlKeys();
        check(SampleLocalControls(local_input,0,UINT64_MAX,sampled) && sampled==net::Pad{},
            "maximum sample tick does not alias an uninitialized key latch");
        check(!SampleLocalControls(local_input,0,0,sampled),"outgoing sample timeline cannot wrap");
        EndSynchronizedControls();check(BeginSynchronizedControls(UINT64_MAX),"restart maximum-tick sampler");
        ControlKey(VK_SPACE,true);ControlKey(VK_SPACE,false);
        check(SampleLocalControls(local_input,0,UINT64_MAX,sampled) && sampled.buttons==0x1000 &&
            SampleLocalControls(local_input,0,UINT64_MAX,sampled) && sampled.buttons==0x1000,
            "quick tap remains valid and cached at the maximum tick");
        EndSynchronizedControls();check(BeginSynchronizedControls(tick),"restart local sample generation");
        physical_source->restart=true;physical_source->restart_tick=tick;
        check(!SampleLocalControls(physical_input,0,tick,sampled) && sampled==net::Pad{} &&
            SampleLocalControls(physical_input,0,tick,sampled) && sampled==net::Pad{},
            "session replacement during device polling discards the stale result and clears old latches");
    }
    {
        std::lock_guard lock(controls_mutex);
        X_INPUT_GAMEPAD raw{};raw.buttons=0x1101;raw.left_trigger=123;raw.right_trigger=241;
        raw.thumb_lx=-12345;raw.thumb_ly=32767;raw.thumb_rx=-32768;raw.thumb_ry=718;
        const auto mapped=Remap(0,raw);
        check(std::memcmp(&mapped,&raw,sizeof(raw))==0,"identity preserves buttons, analog axes and trigger magnitudes");
    }
    ControlKey('W',true);ControlKey(VK_SPACE,true);
    {std::lock_guard lock(controls_mutex);const auto out=Remap(0,{});check(int16_t(out.thumb_ly)==32767 && (uint16_t(out.buttons)&0x1000),"keyboard movement and jump");}
    ClearControlKeys();
    ControlKey(VK_SPACE,true);ControlKey(VK_SPACE,false);
    {
        std::lock_guard lock(controls_mutex);
        const auto first=Remap(0,{}),second=Remap(0,{});
        check((uint16_t(first.buttons)&0x1000) && (uint16_t(second.buttons)&0x1000),"quick tap survives multiple polls in the same simulation tick");
        simulation_steps.fetch_add(1,std::memory_order_relaxed);
        check(!(uint16_t(Remap(0,{}).buttons)&0x1000),"latched tap releases on the next simulation tick");
        simulation_steps.fetch_sub(1,std::memory_order_relaxed);
    }
    ClearControlKeys();
    // No wall-clock sleep: a short tap must release on the next native tick,
    // including axes, while repeated reads within a tick see the same press.
    ControlKey(VK_RIGHT,true);ControlKey(VK_RIGHT,false);
    ControlKey('W',true);ControlKey('W',false);
    {
        std::lock_guard lock(controls_mutex);
        const auto first=Remap(0,{}),again=Remap(0,{});
        check(int16_t(first.thumb_rx)==32767 && int16_t(first.thumb_ly)==32767 &&
              int16_t(again.thumb_rx)==32767 && int16_t(again.thumb_ly)==32767,
              "quick look/movement taps survive reads within one tick");
        simulation_steps.fetch_add(1,std::memory_order_relaxed);
        const auto released=Remap(0,{});
        check(int16_t(released.thumb_rx)==0 && int16_t(released.thumb_ly)==0,
              "released look/movement have no artificial minimum hold");
        simulation_steps.fetch_sub(1,std::memory_order_relaxed);
    }
    ClearControlKeys();
    int result;
    check(ControlOption(2002,VK_SPACE,result) && result==VK_SPACE,"rebind assignment");
    ControlOption(2000,-1,result);check(result=='Q',"binding conflicts swap instead of duplicating");
    LoadControls(directory);
    ControlOption(2002,-1,result);check(result==VK_SPACE,"bindings persist across reload");
    check(!ControlOption(1999,0,result) && !ControlOption(2031,0,result),"invalid binding IDs rejected");
    ControlOption(2000,-4,result);
    ControlOption(2002,-1,result);check(result=='Q',"profile reset");
    ControlOption(2000,'V',result);
    ControlOption(2201,1,result);ControlKey(VK_F8,true);ControlKey(VK_F8,false);
    ControlOption(2000,-1,result);check(result==VK_SPACE,"menu recovery hotkey restores defaults");
    ControlOption(2201,0,result);ClearControlKeys();
    SetExternalControlCapture(true);ControlKey('W',true);ControlKey('W',false);
    {
        std::lock_guard lock(controls_mutex);X_INPUT_GAMEPAD raw{};raw.buttons=0x1000;
        check(SuppressInput(0,raw) && !pending_keys[0]['W'],"native settings consumes gameplay buttons and keyboard edges");
    }
    ControlOption(2000,-2,result);
    {std::lock_guard lock(controls_mutex);capture_started=Clock::now()-std::chrono::milliseconds(300);}
    ControlKey(VK_F7,true);ControlKey(VK_F7,false);
    ControlOption(2000,-1,result);check(result==VK_F7,"native settings still permits deliberate keyboard binding capture");
    SetExternalControlCapture(false);
    {
        std::lock_guard lock(controls_mutex);suppressed_until={};X_INPUT_GAMEPAD raw{};raw.buttons=0x1000;
        check(SuppressInput(0,raw),"closing native settings keeps a held controller button suppressed");
        raw.buttons=0;check(!SuppressInput(0,raw),"releasing the controller resumes gameplay after native settings");
    }
    ControlOption(2000,-4,result);ClearControlKeys();
    ControlOption(2032,-2,result);
    {
        std::lock_guard lock(controls_mutex);
        X_INPUT_GAMEPAD raw{};raw.buttons=0x1000;
        check(SuppressInput(0,raw) && capture>=0,"opening button cannot bind itself");
        raw.buttons=0;SuppressInput(0,raw);
        capture_started=Clock::now()-std::chrono::milliseconds(300);
        raw.buttons=0x2000;
        check(SuppressInput(0,raw) && capture<0 && profiles[1][0]==257,"controller capture completes after neutral state");
        suppressed_until={};
        check(SuppressInput(0,raw),"captured pad input stays suppressed until released");
        raw.buttons=0;check(!SuppressInput(0,raw),"controller release ends capture suppression");
        raw.buttons=0x2000;
        check(uint16_t(Remap(0,raw).buttons)==0x1000,"controller rebind maps physical B to game A");
    }
    ControlOption(2032,-4,result);
    ControlOption(2024,-1,result);check(result==VK_TAB,"character switch uses the modern Tab keyboard default");
    ControlOption(2056,-1,result);check(result==280,"character switch has a controller chord default");
    {
        std::lock_guard lock(controls_mutex);
        X_INPUT_GAMEPAD chord{};chord.buttons=0x300;
        const auto was_menu_open=menu_open;menu_open=true;
        check(PadSource(chord,24)==1 && uint16_t(Remap(0,chord).buttons)==0x300,
            "native menu navigation preserves physical shoulders independently of gameplay bindings");
        Remap(0,{});
        menu_open=was_menu_open;
    }
    ControlOption(2024,VK_F7,result);
    LoadControls(directory);
    ControlOption(2024,-1,result);check(result==VK_F7,"character switch binding persists");
    ControlOption(2000,-4,result);
    check(ControlOption(2300,-1,result) && result==4 && ControlOption(2302,-1,result) && result==0,
        "camera defaults retain original input strength and orientation");
    check(!ControlOption(2304,8,result) && !ControlOption(2306,3,result) && !ControlOption(2332,0,result) &&
        !ControlOption(2300,8,result) && !ControlOption(2302,2,result) && !ControlOption(2300,-2,result),
        "camera option IDs and choices are bounded without entering binding capture");
    {
        // Exercise the same GFx argument bridge used by the in-game menu,
        // including the new ID range rather than only the host-side helper.
        std::array<uint8_t,8192> bytes{};auto* base=bytes.data();
        PPCContext ctx{};ctx.fpscr.InitHost();ctx.r1.u32=0x1000;ctx.r3.u32=0x100;
        PPC_STORE_U32(0x100,0x200);PPC_STORE_U32(0x104,2);PPC_STORE_U32(0x108,0x300);
        PPC_STORE_U32(0x204,3);PPC_STORE_U32(0x214,3);
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(2300.0));
        PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(-1.0));
        extern void TestPCOptionCallback(PPCContext&,uint8_t*);
        TestPCOptionCallback(ctx,base);
        check(PPC_LOAD_U32(0x304)==3 && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==4.0 &&
            ctx.r1.u32==0x1000,"camera options are reachable through the actual in-game script callback");
        for(unsigned profile:{1u,3u})for(unsigned setting=4;setting<8;++setting) {
            const auto id=2300+profile*8+setting;const int chosen=look_sizes[setting]-1;
            for(int command:{-1,chosen,-3}) {
                PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(double(id)));
                PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(double(command)));
                TestPCOptionCallback(ctx,base);
                check(ControlOption(id,-1,result) && result==(command==chosen?chosen:0) &&
                    std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==result && ctx.r1.u32==0x1000,
                    "both controller tuning pages must query/change/reset through the native GFx callback");
            }
        }
    }
    ControlOption(2300,0,result);ControlOption(2303,1,result);
    const auto mouse_factors=MouseLookStrength();
    check(mouse_factors.x==.5f && mouse_factors.y==-1.f,"raw mouse camera must use the keyboard/mouse profile strengths and inversion");
    ControlKey(VK_RIGHT,true);ControlKey(VK_UP,true);
    {
        std::lock_guard lock(controls_mutex);
        const auto out=Remap(0,{});
        check(int16_t(out.thumb_rx)==20728 && int16_t(out.thumb_ry)==-32768,
            "keyboard camera strength and vertical inversion affect only look axes");
    }
    ClearControlKeys();
    {
        std::lock_guard lock(controls_mutex);
        X_INPUT_GAMEPAD raw{};raw.thumb_rx=-32768;raw.thumb_ry=12345;raw.thumb_lx=718;raw.buttons=0x1000;
        const auto out=Remap(0,raw);
        check(!std::memcmp(&raw,&out,sizeof(raw)),
            "keyboard camera settings leave controller input unchanged");
    }
    ControlOption(2310,1,result);ControlOption(2309,0,result);
    {
        std::lock_guard lock(controls_mutex);
        X_INPUT_GAMEPAD raw{};raw.thumb_rx=-32768;raw.thumb_ry=32767;raw.thumb_lx=-12345;raw.buttons=0x1000;
        const auto out=Remap(0,raw);
        check(int16_t(out.thumb_rx)==32767 && int16_t(out.thumb_ry)==20728 &&
            out.thumb_lx==raw.thumb_lx && out.buttons==raw.buttons,
            "controller inversion handles negative extreme without overflow and preserves movement/buttons");
    }
    std::ofstream(controls_file,std::ios::app) << "0.camera_x=999\n1.invert_x=-1\n0.camera_y=0junk\n";
    LoadControls(directory);
    check(ControlOption(2300,-1,result) && result==0 && ControlOption(2303,-1,result) && result==1 &&
        ControlOption(2309,-1,result) && result==0 && ControlOption(2310,-1,result) && result==1 &&
        ControlOption(2301,-1,result) && result==4,
        "per-device camera settings persist and malformed overrides are ignored");
    ControlOption(2300,-3,result);check(result==4,"individual camera reset");
    ControlOption(2000,-4,result);ControlOption(2032,-4,result);
    ControlOption(2303,-1,result);check(result==0,"profile reset also restores camera settings");
    ControlOption(2300,0,result);ControlOption(2201,1,result);
    ControlKey(VK_F8,true);ControlKey(VK_F8,false);ControlOption(2201,0,result);
    ControlOption(2300,-1,result);check(result==4,"F8 recovery restores camera strength as well as bindings");
    ControlOption(2312,2,result);ControlOption(2313,6,result);ControlOption(2314,1,result);ControlOption(2315,2,result);
    LoadControls(directory);
    check(ControlOption(2312,-1,result) && result==2 && ControlOption(2313,-1,result) && result==6 &&
        ControlOption(2314,-1,result) && result==1 && ControlOption(2315,-1,result) && result==2 &&
        ControlOption(2328,-1,result) && result==0,"axis-specific controller tuning persists without changing the other player");
    ControlOption(2312,-3,result);check(result==0,"individual stick tuning reset restores Original");
    ControlOption(2032,-4,result);ControlOption(2315,-1,result);check(result==0,"profile reset restores all stick tuning values");
    for(unsigned p=0;p<4;++p) for(unsigned setting=0;setting<look_sizes.size();++setting) {
        for(int value=0;value<look_sizes[setting];++value) {
            ControlOption(2300+int(p)*8+int(setting),value,result);
            std::lock_guard lock(controls_mutex);
            std::array<float,kBindingCount> input{};input[20]=.25f;input[22]=1.f;input[18]=.625f;
            ShapeLook(input,look_profiles[p]);
            check(std::all_of(input.begin(),input.end(),[](float v){return std::isfinite(v) && v>=0 && v<=1;}) && input[18]==.625f,
                "every camera preset stays bounded and leaves movement values untouched");
        }
        ControlOption(2300+int(p)*8+int(setting),look_defaults[setting],result);
    }
    ControlOption(2000,-2,result);ControlKey(VK_ESCAPE,true);ControlOption(2200,-1,result);check(result==0,"escape cancels capture");
    ClearControlKeys();
    ok=TestWheelBindings(directory) && TestLookStrengthNativeResponse() && ok;
    {std::ofstream legacy(directory/"controls.ini");legacy<<"0.0=17\n";}
    LoadControls(directory);ControlOption(2000,-1,result);check(result==VK_CONTROL,"legacy user bindings retain their keys when movement actions are added");
    ControlOption(2026,-1,result);check(result==0,"a new dash default cannot duplicate an existing legacy key");
    ControlOption(2025,-1,result);check(result==VK_SHIFT,"uncontested sprint defaults migrate into legacy profiles");
    check(!ControlOption(2057,281,result),"additional actions do not invent extra physical gamepad sources");
    ControlOption(2000,-4,result);ClearControlKeys();
    if(ok) REXLOG_INFO("Control binding tests passed: analog identity, keyboard/mouse/wheel, conflicts, persistence, bounds, reset, capture cancellation and per-profile camera adjustment");
    return ok;
}
}

void em2::RemapLocalControlState(unsigned user,rex::input::X_INPUT_STATE& state) {
    if(user>=2)return;
    std::lock_guard lock(controls_mutex);
    const auto& physical=state.gamepad;
    NotifyControllerInput(user,uint16_t(physical.buttons),physical.left_trigger,physical.right_trigger,int16_t(physical.thumb_lx),int16_t(physical.thumb_ly),int16_t(physical.thumb_rx),int16_t(physical.thumb_ry));
    raw_pads[user]=state.gamepad;
    const auto suppressed=SuppressInput(user,raw_pads[user]);if(suppressed)mod_buttons[user].store(0,std::memory_order_relaxed);
    auto mapped=suppressed?X_INPUT_GAMEPAD{}:Remap(user,raw_pads[user]);
    if(std::memcmp(&mapped,&prior_output[user],sizeof(mapped))) {++packets[user];prior_output[user]=mapped;}
    NotifyMappedAimInput(user,mapped.left_trigger>30 || mapped.right_trigger>30 || (uint16_t(mapped.buttons)&0x40));
    state.packet_number=packets[user];state.gamepad=mapped;
    em2::RecordCameraInput(user,int16_t(mapped.thumb_rx),int16_t(mapped.thumb_ry),uint16_t(mapped.buttons));
}

PPC_FUNC_IMPL(sub_831BB758) {
    const auto user=ctx.r3.u32,output=ctx.r4.u32;
    if(user<2 && output) {
        std::lock_guard lock(controls_mutex);
        auto* state=reinterpret_cast<rex::input::X_INPUT_STATE*>(PPC_RAW_ADDR(output));
        if(ReadSynchronizedControls(user,state) || ReadOnlineJoinControls(user,state)) {
            ctx.r3.u64=0;
            em2::RecordCameraInput(user,int16_t(state->gamepad.thumb_rx),int16_t(state->gamepad.thumb_ry),uint16_t(state->gamepad.buttons));
            return;
        }
    }
    __imp__sub_831BB758(ctx,base);
    static thread_local unsigned reported=0;
    if(user<4 && !(reported&(1u<<user))) {
        REXLOG_INFO("PC input bridge: player {}, native state status {:08X}",user,ctx.r3.u32);
        reported|=1u<<user;
    }
    if(user>=2 || !output || ctx.r3.u32!=0) return;
    auto* state=reinterpret_cast<rex::input::X_INPUT_STATE*>(PPC_RAW_ADDR(output));
    em2::RemapLocalControlState(user,*state);
}

PPC_FUNC_IMPL(sub_830ACF88) {
    native_deadzone_bits.store(PPC_LOAD_U32(0x833B65FC),std::memory_order_relaxed);
    __imp__sub_830ACF88(ctx,base);
}

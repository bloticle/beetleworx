#include "pc_settings.h"
#include "demo_support.h"
#include "content_setup.h"
#include <rex/ui/presenter.h>
#include "online_runtime.h"
#include "online_continue.h"
#include "online_native_menu.h"
#include "controls.h"
#include "engine_workbench.h"
#include "gameplay_mods.h"
#include "mouse_look.h"
#include "restored_gremlin_names.h"
#include "em1_ink_restoration.h"
#include "shadow_kernel.h"
#include "shadow_resize.h"
#include "shadow_distance.h"
#include "bloom_kernel.h"
#include "renderer_graph.h"
#include "hdr_settings.h"
#include "scene_lighting_settings.h"
#include "hdr_output.h"
#include "ppc_context.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

REXCVAR_DECLARE(bool, vsync);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_y);
REXCVAR_DECLARE(int32_t, texture_cache_memory_limit_soft);
REXCVAR_DECLARE(int32_t, texture_cache_memory_limit_hard);
REXCVAR_DECLARE(int32_t, texture_cache_memory_limit_soft_lifetime);
REXCVAR_DECLARE(int32_t, query_occlusion_fake_sample_count);
REXCVAR_DECLARE(bool, gpu_allow_invalid_fetch_constants);
REXCVAR_DECLARE(bool, d3d12_allow_variable_refresh_rate_and_tearing);
REXCVAR_DECLARE(int32_t, em2_anisotropic_override);
REXCVAR_DECLARE(bool,em2_restore_ink_drips);

namespace em2 {
namespace {
constexpr std::array<const char*, kPCOptionCount> names = {
    "resolution", "window_mode", "frame_limit", "render_scale", "player_shadow",
    "environment_shadow", "texture_budget", "occlusion_workaround", "legacy_textures",
    "performance_hud", "texture_lifetime", "tearing", "anisotropic_filtering", "lighting_ray_casts", "lighting_update_detail", "gameplay_timing", "gameplay_view_width", "shadow_samples", "bloom_sampling", "camera_response", "mouse_camera", "hdr_output", "hdr_paper_white", "hdr_peak", "hdr_exposure", "material_ambient_occlusion", "screen_upscaler", "upscale_sharpness", "distance_shadow_resolution", "center_cursor_mouse_switch", "center_cursor_aim", "confine_mouse", "hide_mouse_cursor", "mouse_input_priority", "mouse_takeover_threshold", "center_cursor_capture", "mouse_gain_x", "mouse_gain_y", "restored_gremlin_names", "original_ink_drips"};
constexpr std::array<int, kPCOptionCount> sizes = {5,2,10,3,5,4,5,2,2,2,5,2,5,2,3,2,5,4,3,3,2,2,7,7,7,3,3,3,4,2,2,2,2,2,4,2,9,9,2,2};
constexpr std::array<int, kPCOptionCount> defaults = {0,0,0,0,0,1,1,1,0,1,2,1,4,1,0,0,0,0,2,0,1,0,3,3,3,0,2,1,0,1,1,1,1,0,1,1,3,3,0,0};
std::array<int, kPCOptionCount> choices = defaults;
std::array<uint64_t,kPCOptionCount> revisions{};
constexpr std::array<int,10> caps = {30,40,50,60,90,120,144,165,240,0};
std::atomic<int> frame_limit{30};
constexpr std::array<float,5> view_widths={1.f,1.1f,1.25f,1.5f,1.75f};
std::atomic<float> gameplay_view_width{1.f};
std::atomic<int> camera_response{0};
std::atomic<bool> mouse_camera{true};
std::atomic<uint64_t> mouse_cursor_options{0};
std::atomic<unsigned> native_lighting{1};
std::mutex settings_mutex;
std::filesystem::path settings_file;
std::function<void(PCSettingRequest)> changed;
std::function<void(int,int)> support_action;

bool MatchesRequest(PCSettingRequest request) {
    return request.index>=0 && request.index<kPCOptionCount &&
        choices[request.index]==request.choice && revisions[request.index]==request.revision;
}
void PublishChoices() {
    frame_limit.store(caps[choices[2]],std::memory_order_relaxed);
    gameplay_view_width.store(view_widths[choices[16]],std::memory_order_relaxed);
    camera_response.store(choices[19],std::memory_order_relaxed);
    mouse_camera.store(choices[20]!=0,std::memory_order_relaxed);
    uint64_t mouse_options=0;for(unsigned i=0;i<9;++i)mouse_options|=uint64_t(choices[29+i])<<(i*4);
    mouse_cursor_options.store(mouse_options,std::memory_order_release);
    ConfigureRestoredGremlinNames(choices[kPCRestoredGremlinNames]!=0);
    REXCVAR_SET(em2_restore_ink_drips,choices[kPCOriginalInkDrips]!=0);
    native_lighting.store(unsigned(choices[13]|(choices[14]<<1)),std::memory_order_relaxed);
}

class FramePacingSchedule {
public:
    using Clock=std::chrono::steady_clock;
    using Time=Clock::time_point;
    Time BeginFrame(Time now,int cap) {
        if(depth_++) return now;
        prepared_=true;
        return Reserve(now,cap);
    }
    Time Present(Time now,int cap) {
        // Exactly one presentation uses the slot reserved before input. An
        // extra loading/movie present still needs a slot of its own.
        if(depth_ && prepared_) {prepared_=false;return now;}
        return Reserve(now,cap);
    }
    void EndFrame() {
        if(depth_ && !--depth_) prepared_=false;
    }
private:
    Time Reserve(Time now,int cap) {
        if(cap!=previous_cap_ || cap==0) {deadline_=now;previous_cap_=cap;}
        if(cap==0) return now;
        const auto period=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/cap));
        if(now>deadline_+period) deadline_=now;
        const auto ready=std::max(now,deadline_);
        deadline_+=period;
        return ready;
    }
    Time deadline_{};
    int previous_cap_=-1;
    unsigned depth_=0;
    bool prepared_=false;
};
thread_local FramePacingSchedule frame_pacing;

void WaitForFrameSlot(FramePacingSchedule::Time ready) {
    if(ready>FramePacingSchedule::Clock::now()) std::this_thread::sleep_until(ready);
}

void Save() {
    auto temporary = settings_file;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::trunc | std::ios::binary);
    stream << "# Epic Mickey 2 PC settings; choice indexes from the in-game menu.\n";
    for (size_t i=0; i<choices.size(); ++i) stream << names[i] << '=' << choices[i] << '\n';
    stream.close();
    if (!stream || !MoveFileExW(temporary.c_str(), settings_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        REXLOG_ERROR("Unable to save PC settings: {}", settings_file.string());
    }
}
}

void LoadPCSettings(const std::filesystem::path& directory) {
    std::lock_guard lock(settings_mutex);
    choices=defaults;
    std::filesystem::create_directories(directory);
    settings_file = directory / "pc-settings.ini";
    std::ifstream stream(settings_file);
    std::string line;
    while (std::getline(stream, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        for (size_t i=0; i<names.size(); ++i) {
            if (line.substr(0,equal) != names[i]) continue;
            try {
                const auto value = std::stoi(line.substr(equal+1));
                if (value>=0 && value<sizes[i]) choices[i]=value;
            } catch (...) { /* Ignore malformed individual values. */ }
        }
    }
    for(auto& revision:revisions)++revision;
    PublishChoices();
}

int PCSetting(int index) {
    std::lock_guard lock(settings_mutex);
    return index>=0 && index<kPCOptionCount ? choices[index] : -1;
}
HdrOutputSettings PCHdrOutputSettings(){
    constexpr float white[]={80,120,160,200,250,300,400},peak[]={400,600,800,1000,1500,2000,4000},exposure[]={.25f,.5f,.70710678f,1,1.41421356f,2,4};
    std::lock_guard lock(settings_mutex);return {exposure[choices[kPCHdrExposure]],white[choices[kPCHdrPaperWhite]],peak[choices[kPCHdrPeak]],1};
}

int ChangePCSetting(int index, int choice) {
    std::function<void(PCSettingRequest)> notify;
    PCSettingRequest request;
    {
        std::lock_guard lock(settings_mutex);
        if(index<0 || index>=kPCOptionCount || choice<0 || choice>=sizes[index]) return -1;
        if(index==kPCOriginalInkDrips && choice && !em1::OriginalInkPakMounted())return -1;
        choices[index]=choice;
        request={index,choice,++revisions[index]};
        PublishChoices();
        if(index==kPCDistanceShadows)ConfigureNativeShadowDistance(unsigned(choice));
        Save();
        notify=changed;
    }
    REXLOG_INFO("PC setting {} = {}", names[index], choice);
    if(index==20)ResetMouseLook();
    if(notify) notify(request);
    return choice;
}
const char* PCSettingName(int index){return index>=0 && index<kPCOptionCount?names[index]:"";}
int PCSettingChoiceCount(int index){return index>=0 && index<kPCOptionCount?sizes[index]:0;}
int PCSettingDefault(int index){return index>=0 && index<kPCOptionCount?defaults[index]:-1;}

PCSettingRequest SnapshotPCSetting(int index) {
    std::lock_guard lock(settings_mutex);
    return index>=0 && index<kPCOptionCount?PCSettingRequest{index,choices[index],revisions[index]}:PCSettingRequest{};
}
bool IsCurrentPCSettingRequest(PCSettingRequest request) {
    std::lock_guard lock(settings_mutex);return MatchesRequest(request);
}
bool RevertPCSettingIfUnchanged(PCSettingRequest request,int actual) {
    std::unique_lock lock(settings_mutex);
    if(!MatchesRequest(request) || actual<0 || actual>=sizes[request.index])return false;
    choices[request.index]=actual;++revisions[request.index];PublishChoices();
    if(request.index==kPCDistanceShadows)ConfigureNativeShadowDistance(unsigned(actual));
    Save();lock.unlock();
    if(request.index==20)ResetMouseLook();
    return true;
}
void SetPCSettingsChangedCallback(std::function<void(PCSettingRequest)> callback) {
    std::lock_guard lock(settings_mutex);
    changed=std::move(callback);
}
void SetPCSupportActionCallback(std::function<void(int,int)> callback) {
    std::lock_guard lock(settings_mutex);support_action=std::move(callback);
}
bool NativePCSupportOption(int id,int command,int& result) {
    if(id<7300 || id>7314)return false;
    result=0;
    if(command==-1){
        const auto state=SnapshotDemoSupport();
        if(id==7310)result=int(state.state);
        else if(id==7314)result=state.install_on_exit?1:0;
        else if(id==7306)result=state.auto_update?1:0;
        return true;
    }
    if((id<=7309 && id!=7306 && command==1) || (id==7306 && (command==0 || command==1))){
        std::function<void(int,int)> callback;
        {std::lock_guard lock(settings_mutex);callback=support_action;}
        if(callback){callback(id,command);result=1;}
    }
    return true;
}

int FrameLimit() { return frame_limit.load(std::memory_order_relaxed); }
float GameplayViewWidth() {return gameplay_view_width.load(std::memory_order_relaxed);}
int CameraResponse() {return camera_response.load(std::memory_order_relaxed);}
bool MouseCameraEnabled() {return mouse_camera.load(std::memory_order_relaxed);}
MouseCursorOptions GetMouseCursorOptions() {
    const auto bits=mouse_cursor_options.load(std::memory_order_acquire);
    auto value=[&](unsigned i){return unsigned((bits>>(i*4))&15);};
    constexpr float gains[]={.25f,.5f,.75f,1.f,1.25f,1.5f,2.f,3.f,4.f};
    return {bool(value(0)),bool(value(1)),bool(value(2)),bool(value(3)),bool(value(4)),bool(value(6)),
        1u<<std::min(value(5),3u),gains[std::min(value(7),8u)],gains[std::min(value(8),8u)]};
}

GuestFramePacingScope::GuestFramePacingScope() {
    WaitForFrameSlot(frame_pacing.BeginFrame(FramePacingSchedule::Clock::now(),FrameLimit()));
}
GuestFramePacingScope::~GuestFramePacingScope() {frame_pacing.EndFrame();}

void PaceGuestPresentation() {
    WaitForFrameSlot(frame_pacing.Present(FramePacingSchedule::Clock::now(),FrameLimit()));
}

void ApplyPCStartupSettings() {
    ConfigureNativeShadowDistance(unsigned(PCSetting(kPCDistanceShadows)));
    // Guest vblank waits no longer impose the console frame cap. The host
    // limiter reserves each frame before input/update, with a presentation
    // fallback for paths that do not enter the native outer game frame.
    REXCVAR_SET(vsync,false);
    const auto scale=PCSetting(3)+1;
    REXCVAR_SET(draw_resolution_scale_x,scale);
    REXCVAR_SET(draw_resolution_scale_y,scale);
    const int budgets[]={256,512,768,1024,2048};
    REXCVAR_SET(texture_cache_memory_limit_soft,budgets[PCSetting(6)]);
    REXCVAR_SET(texture_cache_memory_limit_hard,budgets[PCSetting(6)]*2);
    const int lifetimes[]={5,15,30,60,120};
    REXCVAR_SET(texture_cache_memory_limit_soft_lifetime,lifetimes[PCSetting(10)]);
    REXCVAR_SET(query_occlusion_fake_sample_count,PCSetting(7) ? -1 : 1000);
    REXCVAR_SET(gpu_allow_invalid_fetch_constants,PCSetting(8)!=0);
    REXCVAR_SET(d3d12_allow_variable_refresh_rate_and_tearing,PCSetting(11)!=0);
    const int anisotropy[]={-1,2,3,4,5};
    REXCVAR_SET(em2_anisotropic_override,anisotropy[PCSetting(12)]);
    const unsigned shadow_samples[]={4,8,16,20};
    ConfigureShadowKernel(shadow_samples[PCSetting(17)]);
    ConfigureBloomSampling(unsigned(PCSetting(18)));
}

void ApplyPCPresentationSettings(rex::ui::Presenter& presenter){
    using Config=rex::ui::Presenter::GuestOutputPaintConfig;
    auto config=presenter.GetGuestOutputPaintConfigFromUIThread();
    const auto mode=PCSetting(kPCScreenUpscaler),sharpness=std::clamp(PCSetting(kPCUpscaleSharpness),0,2);
    config.SetEffect(mode==2?Config::Effect::kFsr:mode==1?Config::Effect::kCas:Config::Effect::kBilinear);
    constexpr float cas[]={0.0f,0.2f,0.4f},fsr[]={1.0f,0.5f,0.2f};
    config.SetCasAdditionalSharpness(cas[sharpness]);config.SetFsrSharpnessReduction(fsr[sharpness]);
    config.SetFsrMaxUpsamplingPasses(Config::kFsrMaxUpscalingPassesMax);
    config.SetDither(mode!=0);
    presenter.SetGuestOutputPaintConfigFromUIThread(config);
}

unsigned NativeLightingSettings(){return native_lighting.load(std::memory_order_relaxed);}
void ApplyPCLiveGPUSetting(int index) {
    if(index==6) {
        const int budgets[]={256,512,768,1024,2048};
        const auto budget=budgets[PCSetting(6)];
        REXCVAR_SET(texture_cache_memory_limit_soft,budget);
        REXCVAR_SET(texture_cache_memory_limit_hard,budget*2);
    }
    if(index==10){const int seconds[]={5,15,30,60,120};REXCVAR_SET(texture_cache_memory_limit_soft_lifetime,seconds[PCSetting(10)]);}
    if(index==7)REXCVAR_SET(query_occlusion_fake_sample_count,PCSetting(7)?-1:1000);
    if(index==8)REXCVAR_SET(gpu_allow_invalid_fetch_constants,PCSetting(8)!=0);
    if(index==12){const int anisotropy[]={-1,2,3,4,5};REXCVAR_SET(em2_anisotropic_override,anisotropy[PCSetting(12)]);}
    if(index==17){const unsigned quality[]={4,8,16,20};ConfigureShadowKernel(quality[PCSetting(17)]);}
    if(index==18)ConfigureBloomSampling(unsigned(PCSetting(18)));
}

void WriteGameConfigOverlay(const std::filesystem::path& original,
                            const std::filesystem::path& destination) {
    std::ifstream source(original, std::ios::binary);
    if(!source) throw std::runtime_error("Missing original ConfigFiles.ini");
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::trunc);
    const int player[]={128,256,512,1024,2048};
    const int environment[]={512,1024,2048,4096};
    const char* lighting_distance[]={"1.00","0.50","0.25"};
    std::string line;
    while(std::getline(source,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.starts_with("ShadowMapPlayerSize=")) line="ShadowMapPlayerSize="+std::to_string(player[PCSetting(4)]);
        if(line.starts_with("ShadowMapEnvSize=")) line="ShadowMapEnvSize="+std::to_string(environment[PCSetting(5)]);
        // Native hashed lookup 7790627F at 827A4538 caches this flag in
        // 8339A1C4. Keep the authored ray-cast lighting enabled by default.
        if(line.starts_with("SphericalHarmonicLightingRayCast=")) line=std::string("SphericalHarmonicLightingRayCast=")+(PCSetting(13)?"true":"false");
        // 827A4610 squares the configured distance. Native consumers at
        // 8279F040 and 827A19D0 request lighting updates after movement exceeds it.
        if(line.starts_with("SphericalHarmonicLightingRayCastDistanceThreshold=")) line=std::string("SphericalHarmonicLightingRayCastDistanceThreshold=")+lighting_distance[PCSetting(14)];
        // Keep room for later quality changes even when starting low. This
        // console bucket cache reserves 156 MiB up front; ordinary native GPU
        // allocations can instead use the arena as resources are needed.
        if(line.starts_with("BucketMemoryUse_GPU=")) line="BucketMemoryUse_GPU=false";
        output << line << '\n';
    }
    if(!output) throw std::runtime_error("Unable to write PC game config overlay");
}

namespace {
std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),{});
}
}

bool TestPCSettings(const std::filesystem::path& directory,
                    const std::filesystem::path& original_config) {
    auto require=[](bool success,const char* message) {
        if(!success) throw std::runtime_error(message);
    };
    try {
        // Controlled time validates the real pacing policy without depending
        // on Windows scheduling jitter or sending input to a game window.
        using Time=FramePacingSchedule::Time;
        using namespace std::chrono_literals;
        for(const auto cap:caps) {
            FramePacingSchedule pacing;
            Time now{};
            const auto period=cap?std::chrono::duration_cast<FramePacingSchedule::Clock::duration>(std::chrono::duration<double>(1.0/cap)):0ns;
            for(int i=0;i<120;++i) {
                const auto start=pacing.BeginFrame(now,cap);
                require(start==Time{}+(cap?i*period:i*1ms),"Frame starts do not follow the selected cap");
                // A fresh input sample and 1 ms of native work follow the wait.
                now=start+1ms;
                require(pacing.Present(now,cap)==now,"Limiter waited with an already-sampled input frame");
                pacing.EndFrame();
            }
        }
        {
            FramePacingSchedule pacing;
            Time now{};
            const auto period=std::chrono::duration_cast<FramePacingSchedule::Clock::duration>(std::chrono::duration<double>(1.0/60));
            require(pacing.BeginFrame(now,60)==now,"First frame must not wait");
            require(pacing.BeginFrame(now,60)==now,"Nested frame must not reserve a second slot");
            require(pacing.Present(now+1ms,60)==now+1ms,"Nested presentation must use the reserved slot");
            pacing.EndFrame();
            require(pacing.Present(now+2ms,60)==now+period,"Extra presentation must retain its cap");
            pacing.EndFrame();
            require(pacing.Present(now+period+1ms,60)==now+2*period,"Out-of-frame presentation must be paced");
            now+=1s;
            require(pacing.BeginFrame(now,60)==now,"Long stalls must discard expired pacing debt");
            pacing.EndFrame(); // A frame which never presents cannot leave a credit.
            require(pacing.Present(now+1ms,60)==now+period,"Aborted frame leaked a presentation credit");
            require(pacing.BeginFrame(now+2ms,120)==now+2ms,"Cap change retained an old deadline");
            pacing.EndFrame();
            require(pacing.Present(now+3ms,0)==now+3ms,"Uncapped fallback must not wait");
        }
        require(!std::filesystem::exists(directory),"Test directory already exists");
        LoadPCSettings(directory);
        std::string gremlin_error;require(TestRestoredGremlinNames(gremlin_error),gremlin_error.c_str());
        require(!RestoredGremlinNamesEnabled(),"Restored Gremlin names must be opt-in");
        ChangePCSetting(kPCRestoredGremlinNames,1);LoadPCSettings(directory);
        require(RestoredGremlinNamesEnabled(),"Restored Gremlin name option did not persist/apply live");
        ChangePCSetting(kPCRestoredGremlinNames,0);
        REXLOG_INFO("Gremlin dialogue-name restoration passed authored names, both native BSQ call contexts, protected guest pages, caller/FP/error preservation and live option persistence");
        require(!em1::OriginalInkPakMounted() && ChangePCSetting(kPCOriginalInkDrips,1)==-1,"Unavailable source ink assets must not admit an enabled option");
        em1::SetOriginalInkPakMounted(true);
        const auto ink_choice=ChangePCSetting(kPCOriginalInkDrips,1);LoadPCSettings(directory);
        const bool ink_saved=PCSetting(kPCOriginalInkDrips)==1 && REXCVAR_GET(em2_restore_ink_drips);
        em1::SetOriginalInkPakMounted(false);ChangePCSetting(kPCOriginalInkDrips,0);
        require(ink_choice==1 && ink_saved && !REXCVAR_GET(em2_restore_ink_drips),"Live ink option admission/persistence/reset failed");
        require(FrameLimit()==30 && PCSetting(7)==1 && PCSetting(13)==1,"Incorrect PC defaults");
        require(PCSetting(kPCHdrMode)==0 && PCHdrOutputSettings().paper_white_nits==200 && PCHdrOutputSettings().peak_nits==1000 && PCHdrOutputSettings().exposure==1,"HDR defaults failed");
        require(TestHdrSettingRequestOrdering(),"HDR status request ordering failed");
        require(PCSetting(kPCSceneLighting)==0 && TestSceneLightingRequestOrdering(),"Material AO defaults/request ordering failed");
        WriteGameConfigOverlay(original_config,directory/"ConfigFiles.ini");
        require(ReadText(directory/"ConfigFiles.ini").find("BucketMemoryUse_GPU=false")!=std::string::npos,
            "Low-quality startup must preserve arena room for later graphics quality changes");
        require(ChangePCSetting(2,3)==3 && FrameLimit()==60,"Frame cap change failed");
        require(ChangePCSetting(4,3)==3 && ChangePCSetting(5,2)==2,"Shadow choice change failed");
        require(ChangePCSetting(12,4)==4 && ChangePCSetting(12,5)==-1,"Anisotropy choice bounds failed");
        auto saved=ReadText(directory/"pc-settings.ini");
        require(saved.find("frame_limit=3\n")!=std::string::npos,"Settings did not persist");
        require(saved.find("anisotropic_filtering=4\n")!=std::string::npos,"Anisotropy setting did not persist");
        require(ChangePCSetting(2,999)==-1 && ChangePCSetting(99,0)==-1,"Invalid setting accepted");
        require(ReadText(directory/"pc-settings.ini")==saved,"Rejected setting changed the file");
        LoadPCSettings(directory);
        require(FrameLimit()==60 && PCSetting(4)==3 && PCSetting(5)==2,"Settings reload failed");
        require(PCSetting(12)==4,"Anisotropy setting did not reload");
        const auto original=ReadText(original_config);
        WriteGameConfigOverlay(original_config,directory/"ConfigFiles.ini");
        const auto overlay=ReadText(directory/"ConfigFiles.ini");
        require(overlay.find("ShadowMapPlayerSize=1024")!=std::string::npos && overlay.find("ShadowMapEnvSize=2048")!=std::string::npos,"Shadow config override failed");
        require(overlay.find("BucketMemoryUse_GPU=false")!=std::string::npos,"High-quality startup GPU bucket reservation disabled");
        require(overlay.find("SphericalHarmonicLightingRayCast=true\r\n")!=std::string::npos,"Default lighting ray casts were not preserved");
        require(ChangePCSetting(13,0)==0 && ChangePCSetting(13,2)==-1,"Lighting ray-cast choice bounds failed");
        LoadPCSettings(directory);
        require(PCSetting(13)==0,"Lighting ray-cast setting did not persist and reload");
        WriteGameConfigOverlay(original_config,directory/"ConfigFiles.ini");
        require(ReadText(directory/"ConfigFiles.ini").find("SphericalHarmonicLightingRayCast=false\r\n")!=std::string::npos,"Lighting ray casts did not disable in the overlay");
        ChangePCSetting(13,1);
        WriteGameConfigOverlay(original_config,directory/"ConfigFiles.ini");
        require(ReadText(directory/"ConfigFiles.ini").find("SphericalHarmonicLightingRayCast=true\r\n")!=std::string::npos,"Lighting ray casts did not re-enable in the overlay");
        require(PCSetting(14)==0 && ChangePCSetting(14,3)==-1,"Lighting update defaults/bounds failed");
        for(int detail=0;detail<3;++detail) {
            ChangePCSetting(14,detail);
            LoadPCSettings(directory);
            require(PCSetting(14)==detail,"Lighting update detail did not persist");
            WriteGameConfigOverlay(original_config,directory/"ConfigFiles.ini");
            const char* distances[]={"1.00","0.50","0.25"};
            require(ReadText(directory/"ConfigFiles.ini").find(std::string("SphericalHarmonicLightingRayCastDistanceThreshold=")+distances[detail]+"\r\n")!=std::string::npos,"Lighting update distance overlay failed");
        }
        require(ReadText(original_config)==original,"Original game config was changed");
        // Run the actual script callback against a small synthetic guest frame.
        std::vector<uint8_t> bytes(8192,0);
        auto* base=bytes.data();
        PPCContext ctx{};
        ctx.r1.u32=0x1000;
        ctx.r3.u32=0x100;
        PPC_STORE_U32(0x100,0x200);
        PPC_STORE_U32(0x104,2);
        PPC_STORE_U32(0x108,0x300);
        PPC_STORE_U32(0x204,3);
        PPC_STORE_U32(0x214,3);
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1002.0));
        PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(-1.0));
        extern void TestPCOptionCallback(PPCContext&,uint8_t*);
        TestPCOptionCallback(ctx,base);
        require(PPC_LOAD_U32(0x304)==3 && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==3.0,"Script callback query failed");
        require(ctx.r1.u32==0x1000,"Script callback corrupted the guest stack");
        {
            std::function<void(int,int)> previous;
            {std::lock_guard lock(settings_mutex);previous=std::move(support_action);}
            struct RestoreSupport {std::function<void(int,int)> value;~RestoreSupport(){SetPCSupportActionCallback(std::move(value));}} restore{std::move(previous)};
            std::vector<std::pair<int,int>> actions;
            SetPCSupportActionCallback([&](int id,int command){actions.emplace_back(id,command);});
            auto support=[&](int id,int command){
                PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(double(id)));
                PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(double(command)));
                TestPCOptionCallback(ctx,base);
                require(ctx.r1.u32==0x1000,"Support script callback corrupted the guest stack");
                return std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)));
            };
            for(int id=7300;id<=7309;++id)require(support(id,1)==1,"Support action was not queued");
            require(support(7306,0)==1 && actions.size()==11 && actions.back()==std::pair<int,int>{7306,0},"Support explicit Off action was lost");
            const auto snapshot=SnapshotDemoSupport();
            require(support(7310,-1)==int(snapshot.state) && support(7314,-1)==int(snapshot.install_on_exit) &&
                    support(7306,-1)==int(snapshot.auto_update) && actions.size()==11,"Support status query mutated an action");
            SetPCSupportActionCallback({});
            require(support(7300,1)==0,"Detached support action retained its UI owner");
        }
        PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(-1.0));
        QueueRendererScaleChange();
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1998.0));TestPCOptionCallback(ctx,base);
        require(std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==double(int(LiveRenderScaleStatus::Queued)) &&
                ctx.r1.u32==0x1000,"Render scale status script bridge failed");
        ChangePCSetting(3,1);const auto old_scale_request=SnapshotPCSetting(3);
        ChangePCSetting(3,2);RevertPCSettingIfUnchanged(old_scale_request,0);
        require(PCSetting(3)==2,"Stale failed render request overwrote newer selection");
        require(RevertPCSettingIfUnchanged(SnapshotPCSetting(3),1),"Current failed render request was not rolled back");LoadPCSettings(directory);
        require(PCSetting(3)==1,"Failed render selection rollback did not persist");
        {
            const auto first=SnapshotPCSetting(3);
            ChangePCSetting(3,2);ChangePCSetting(3,1);
            require(!IsCurrentPCSettingRequest(first) && !RevertPCSettingIfUnchanged(first,0) && PCSetting(3)==1,
                "ABA selection allowed an old failure to undo a newer request of the same value");
            const auto retry=SnapshotPCSetting(3);ChangePCSetting(3,1);
            require(!RevertPCSettingIfUnchanged(retry,0),"Retrying the same choice did not invalidate its older failure");
            const auto current=SnapshotPCSetting(3);ChangePCSetting(6,1);
            require(IsCurrentPCSettingRequest(current),"Unrelated option invalidated a render request");
            require(!RevertPCSettingIfUnchanged(current,3) && IsCurrentPCSettingRequest(current),"Invalid rollback changed request state");
            require(RevertPCSettingIfUnchanged(current,0) && !IsCurrentPCSettingRequest(current) &&
                !RevertPCSettingIfUnchanged(current,2),"Completed rollback could be applied twice");
            const auto before_reload=SnapshotPCSetting(3);LoadPCSettings(directory);
            require(PCSetting(3)==0 && !IsCurrentPCSettingRequest(before_reload),"Reload left a stale request valid");
            require(!IsCurrentPCSettingRequest(SnapshotPCSetting(-1)) && !RevertPCSettingIfUnchanged({},0),"Invalid request was accepted");
            ChangePCSetting(2,3);const auto fps=SnapshotPCSetting(2);
            require(FrameLimit()==60 && RevertPCSettingIfUnchanged(fps,0) && FrameLimit()==30,
                "Rollback did not republish the effective setting");
        }
        {
            std::vector<PCSettingRequest> queued;
            std::function<void(PCSettingRequest)> previous;
            {std::lock_guard lock(settings_mutex);previous=changed;}
            struct RestoreCallback {std::function<void(PCSettingRequest)> value;~RestoreCallback(){SetPCSettingsChangedCallback(std::move(value));}}restore_callback{std::move(previous)};
            bool nested=false;
            SetPCSettingsChangedCallback([&](PCSettingRequest request) {
                if(!nested){nested=true;ChangePCSetting(3,1);}
                queued.push_back(request);
            });
            ChangePCSetting(3,2);
            require(queued.size()==2 && queued[0].choice==1 && queued[1].choice==2 &&
                IsCurrentPCSettingRequest(queued[0]) && !IsCurrentPCSettingRequest(queued[1]),
                "Reentrant out-of-order notifications lost their exact selection snapshots");
            unsigned applied=0;int applied_choice=-1;
            for(const auto request:queued)if(IsCurrentPCSettingRequest(request)){++applied;applied_choice=request.choice;}
            require(applied==1 && applied_choice==1,"Out-of-order stale work reached the deferred apply gate");
            queued.clear();
            for(int choice:{0,2,1,0,2,0,1,2})ChangePCSetting(3,choice);
            applied=0;for(const auto request:queued)if(IsCurrentPCSettingRequest(request))++applied;
            require(applied==1 && IsCurrentPCSettingRequest(queued.back()),"Rapid choices failed to coalesce to the final request");
            const auto count=queued.size();const auto valid=SnapshotPCSetting(3);
            require(ChangePCSetting(3,3)==-1 && ChangePCSetting(-1,0)==-1 && queued.size()==count &&
                IsCurrentPCSettingRequest(valid),"Rejected choice emitted or invalidated an apply request");
        }
        require(TestRendererScaleRequestOrdering(),"Render-scale status ordering failed");
        REXLOG_INFO("Live settings request regressions passed: same-value ABA/retry, persistent single rollback, effective-value publication, reload invalidation, independent options, reentrant out-of-order callbacks and rapid-choice coalescing");
        require(PCSetting(15)==0 && ChangePCSetting(15,2)==-1,"Gameplay timing defaults/bounds failed");
        // Exercise the in-game SetOption callback, then reload its saved choice.
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1015.0));
        PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(1.0));
        TestPCOptionCallback(ctx,base);
        require(std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==1.0 && PCSetting(15)==1,"Native timing script selection failed");
        LoadPCSettings(directory);
        require(PCSetting(15)==1,"Native timing selection did not persist");
        PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(0.0));
        TestPCOptionCallback(ctx,base);
        LoadPCSettings(directory);
        require(PCSetting(15)==0,"Compatibility timing selection did not persist");
        require(PCSetting(kPCDistanceShadows)==0 && PCSettingChoiceCount(kPCDistanceShadows)==4,"Distance shadow defaults/bounds failed");
        for(unsigned choice=0;choice<4;++choice){
            PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(double(kPCOptionBase+kPCDistanceShadows)));
            PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(double(choice)));
            TestPCOptionCallback(ctx,base);
            require(NativeShadowDistanceSetting()==choice && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==double(choice),"Native menu did not apply distance shadow preset live");
            ConfigureNativeShadowDistance(0);LoadPCSettings(directory);ApplyPCStartupSettings();
            require(PCSetting(kPCDistanceShadows)==int(choice) && NativeShadowDistanceSetting()==choice,"Distance shadow preset did not survive reload/startup");
        }
        const auto distance_saved=ReadText(directory/"pc-settings.ini");
        require(ChangePCSetting(kPCDistanceShadows,4)==-1 && ReadText(directory/"pc-settings.ini")==distance_saved && NativeShadowDistanceSetting()==3,"Invalid distance preset changed live state or persistence");
        ChangePCSetting(kPCDistanceShadows,2);const auto old_distance=SnapshotPCSetting(kPCDistanceShadows);
        ChangePCSetting(kPCDistanceShadows,1);
        require(!RevertPCSettingIfUnchanged(old_distance,0) && NativeShadowDistanceSetting()==1,"Stale distance preset rollback overwrote newer selection");
        require(RevertPCSettingIfUnchanged(SnapshotPCSetting(kPCDistanceShadows),0) && NativeShadowDistanceSetting()==0,"Distance preset rollback did not restore live fixed quality");
        require(PCSetting(16)==0 && GameplayViewWidth()==1.f && ChangePCSetting(16,5)==-1,
                "View width defaults/bounds failed");
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1016.0));
        PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(3.0));
        TestPCOptionCallback(ctx,base);
        require(PCSetting(16)==3 && GameplayViewWidth()==1.5f &&
                std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==3.0,"View width script selection failed");
        LoadPCSettings(directory);
        require(PCSetting(16)==3 && GameplayViewWidth()==1.5f,"View width selection did not persist");
        require(PCSetting(17)==0 && ChangePCSetting(17,4)==-1,"Shadow sampling defaults/bounds failed");
        const auto prior_shadow=NativeShadowSamples();
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1017.0));
        PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(2.0));
        TestPCOptionCallback(ctx,base);
        LoadPCSettings(directory);
        require(PCSetting(17)==2 && NativeShadowSamples()==prior_shadow &&
            std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==2.0,"Shadow sampling bridge/persistence/queued GPU policy failed");
        ApplyPCLiveGPUSetting(17);require(NativeShadowSamples()==16,"16-sample live GPU selection failed");
        require(ChangePCSetting(17,1)==1 && NativeShadowSamples()==16,"Shadow sampling changed a live shader policy");
        ApplyPCLiveGPUSetting(17);require(NativeShadowSamples()==8,"8-sample live GPU selection failed");
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1017.0));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(3.0));
        TestPCOptionCallback(ctx,base);
        require(PCSetting(17)==3 && NativeShadowSamples()==8 && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==3.0,"Adaptive shadow selection must wait for the GPU callback");
        ApplyPCLiveGPUSetting(17);require(NativeShadowSamples()==20,"Adaptive shadow live GPU selection failed");
        LoadPCSettings(directory);require(PCSetting(17)==3,"Adaptive shadow selection must persist");
        require(PCSetting(18)==2 && ChangePCSetting(18,3)==-1,"Bloom defaults/bounds failed");
        const auto prior_bloom=NativeBloomSampling();
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1018.0));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(0.0));
        TestPCOptionCallback(ctx,base);LoadPCSettings(directory);
        require(PCSetting(18)==0 && NativeBloomSampling()==prior_bloom &&
            std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==0.,"Bloom callback/persistence/queued GPU policy failed");
        ApplyPCLiveGPUSetting(18);require(NativeBloomSampling()==0,"Bloom Off live GPU selection failed");
        require(ChangePCSetting(18,1)==1 && NativeBloomSampling()==0,"Bloom changed live shader policy");
        ApplyPCLiveGPUSetting(18);require(NativeBloomSampling()==1,"Original bloom live GPU selection failed");
        require(ChangePCSetting(18,2)==2,"Smooth bloom selection failed");
        ApplyPCLiveGPUSetting(18);require(NativeBloomSampling()==2,"Smooth bloom live GPU selection failed");
        const int budgets[]={256,512,768,1024,2048},lifetimes[]={5,15,30,60,120},anisotropy[]={-1,2,3,4,5};
        for(int choice=0;choice<5;++choice) {
            ChangePCSetting(6,choice);ApplyPCLiveGPUSetting(6);
            require(REXCVAR_GET(texture_cache_memory_limit_soft)==budgets[choice] && REXCVAR_GET(texture_cache_memory_limit_hard)==budgets[choice]*2,"Live cache budget failed");
            ChangePCSetting(10,choice);ApplyPCLiveGPUSetting(10);require(REXCVAR_GET(texture_cache_memory_limit_soft_lifetime)==lifetimes[choice],"Live cache lifetime failed");
            ChangePCSetting(12,choice);ApplyPCLiveGPUSetting(12);require(REXCVAR_GET(em2_anisotropic_override)==anisotropy[choice],"Live anisotropy failed");
        }
        for(int detail=0;detail<3;++detail)for(int rays=0;rays<2;++rays) {
            ChangePCSetting(13,rays);ChangePCSetting(14,detail);
            require(NativeLightingSettings()==unsigned(rays|(detail<<1)),"Live lighting selection failed");
        }
        require(PCSetting(19)==0 && CameraResponse()==0 && ChangePCSetting(19,3)==-1,"Camera response defaults/bounds failed");
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1019.0));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(2.0));
        TestPCOptionCallback(ctx,base);
        require(CameraResponse()==2 && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==2.0,"Camera response script callback failed");
        LoadPCSettings(directory);require(PCSetting(19)==2 && CameraResponse()==2,"Camera response did not persist");
        require(MouseCameraEnabled() && ChangePCSetting(20,2)==-1,"Mouse camera defaults/bounds failed");
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1020.0));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(1.0));
        TestPCOptionCallback(ctx,base);
        require(MouseCameraEnabled() && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==1.0,"Mouse camera script callback failed");
        LoadPCSettings(directory);require(MouseCameraEnabled(),"Mouse camera selection did not persist");
        for(int option=kPCHdrMode;option<=kPCHdrExposure;++option){
            for(int choice=0;choice<sizes[option];++choice){
                PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(double(kPCOptionBase+option)));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(double(choice)));TestPCOptionCallback(ctx,base);
                require(PCSetting(option)==choice && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==choice,"HDR script option callback failed");
                LoadPCSettings(directory);const auto hdr=PCHdrOutputSettings();require(PCSetting(option)==choice && hdr.paper_white_nits>=80 && hdr.paper_white_nits<=400 && hdr.peak_nits>=hdr.paper_white_nits && hdr.exposure>=.25f && hdr.exposure<=4,"HDR persistence/calibration mapping failed");
            }
            require(ChangePCSetting(option,sizes[option])==-1,"HDR option bounds failed");
        }
        ChangePCSetting(kPCHdrMode,1);const auto hdr_request=SnapshotPCSetting(kPCHdrMode);QueueHdrSettingRequest(hdr_request.revision);
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1997.0));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(-1.0));TestPCOptionCallback(ctx,base);require(std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==int(HdrSettingStatus::Queued),"HDR status script callback failed");
        ChangePCSetting(kPCHdrMode,0);ChangePCSetting(kPCHdrMode,1);require(!RevertPCSettingIfUnchanged(hdr_request,0),"Stale HDR failure overwrote newer selection");
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1025.0));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(1.0));TestPCOptionCallback(ctx,base);
        require(PCSetting(kPCSceneLighting)==1 && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==1,"Material AO script callback failed");
        LoadPCSettings(directory);require(PCSetting(kPCSceneLighting)==1 && ChangePCSetting(kPCSceneLighting,2)==2 && ChangePCSetting(kPCSceneLighting,3)==-1,"Scene lighting persistence/bounds failed");LoadPCSettings(directory);require(PCSetting(kPCSceneLighting)==2,"Indirect lighting persistence failed");
        const auto lighting_request=SnapshotPCSetting(kPCSceneLighting);QueueSceneLightingSetting(lighting_request.revision);
        PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(1996.0));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(-1.0));TestPCOptionCallback(ctx,base);
        require(std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==int(SceneLightingSettingStatus::Queued),"Material AO status callback failed");
        ChangePCSetting(kPCSceneLighting,0);ChangePCSetting(kPCSceneLighting,1);require(!RevertPCSettingIfUnchanged(lighting_request,0),"Stale AO failure overwrote newer selection");
        for(int option=kPCCenterOnMouse;option<=kPCMouseGainY;++option) {
            for(int choice=0;choice<sizes[option];++choice) {
                PPC_STORE_U64(0x208,std::bit_cast<uint64_t>(double(kPCOptionBase+option)));PPC_STORE_U64(0x218,std::bit_cast<uint64_t>(double(choice)));TestPCOptionCallback(ctx,base);
                require(PCSetting(option)==choice && std::bit_cast<double>(uint64_t(PPC_LOAD_U64(0x308)))==choice,"Mouse option native script callback failed");
                LoadPCSettings(directory);require(PCSetting(option)==choice,"Mouse option did not persist");
            }
            require(ChangePCSetting(option,sizes[option])==-1,"Mouse option bounds failed");
            ChangePCSetting(option,defaults[option]);
        }
        const auto mouse=GetMouseCursorOptions();
        require(mouse.center_switch && mouse.center_aim && mouse.confine && mouse.hide && !mouse.always_mouse && mouse.center_capture && mouse.takeover_counts==2 && mouse.gain_x==1 && mouse.gain_y==1,"Published mouse defaults failed");
        std::ofstream(directory/"pc-settings.ini",std::ios::trunc) << "frame_limit=999\nresolution=garbage\nplayer_shadow=-5\nshadow_samples=999\nbloom_sampling=999\ncamera_response=999\n";
        LoadPCSettings(directory);
        ApplyPCStartupSettings();require(PCSetting(17)==0 && NativeShadowSamples()==4,"Malformed shadow quality must return to Original");
        require(PCSetting(18)==2 && NativeBloomSampling()==2,"Malformed bloom must fall back to Smooth");
        require(PCSetting(19)==0 && CameraResponse()==0,"Malformed camera response must fall back to Original");
        require(PCSetting(kPCDistanceShadows)==0 && NativeShadowDistanceSetting()==0,"Missing distance preset must retain fixed shadow quality");
        require(MouseCameraEnabled(),"Missing mouse camera setting must default to On");
        require(PCSetting(kPCSceneLighting)==0,"Missing material AO setting must default to Off");
        require(PCSetting(kPCHdrMode)==0 && PCHdrOutputSettings().exposure==1 && PCHdrOutputSettings().paper_white_nits==200 && PCHdrOutputSettings().peak_nits==1000,"Missing HDR settings must use safe defaults");
        require(FrameLimit()==30 && PCSetting(0)==0 && PCSetting(4)==0 && GameplayViewWidth()==1.f,"Malformed settings did not fall back to defaults");
        std::filesystem::remove(directory/"pc-settings.ini");
        std::filesystem::remove(directory/"ConfigFiles.ini");
        std::filesystem::remove(directory);
        REXLOG_INFO("PC settings tests passed: pre-update pacing policy, persistence, bounds, reload, config overlay, script callback, malformed data");
        return true;
    } catch(const std::exception& error) {
        REXLOG_ERROR("PC settings tests failed: {}",error.what());
        return false;
    }
}
}

REXCVAR_DECLARE(bool,em2_trace_online_observations);
PPC_EXTERN_FUNC(__imp__sub_82B0D118);
PPC_EXTERN_FUNC(sub_8295FEA8);
PPC_FUNC_IMPL(sub_82B0D118) {
    // OptionsMenuState::SetOption receives the native script-argument wrapper:
    // +0 GFx::Value[16-byte stride], +4 count, +8 result GFx::Value.
    const auto params=ctx.r3.u32;
    if(params && PPC_LOAD_U32(params+4)>=2) {
        const auto arguments=PPC_LOAD_U32(params);
        if(arguments && (PPC_LOAD_U32(arguments+4)&0x8F)==3 && (PPC_LOAD_U32(arguments+20)&0x8F)==3) {
            const auto raw_id=std::bit_cast<double>(uint64_t(PPC_LOAD_U64(arguments+8)));
            const auto raw_choice=std::bit_cast<double>(uint64_t(PPC_LOAD_U64(arguments+24)));
            if(std::isfinite(raw_id) && raw_id>=7300 && raw_id<=7314 && std::floor(raw_id)==raw_id &&
               (raw_choice==-1 || raw_choice==0 || raw_choice==1)) {
                int result=0;em2::NativePCSupportOption(int(raw_id),int(raw_choice),result);
                ctx.f1.f64=result;sub_8295FEA8(ctx,base);return;
            }
            if((raw_id==7200 || raw_id==7201 || raw_id==7203) && (raw_choice==-1 || raw_choice==1)){
                int result=0;if(em2::NativeContentLibraryOption(int(raw_id),int(raw_choice),result)){
                    ctx.f1.f64=result;sub_8295FEA8(ctx,base);return;
                }
            }
            if(raw_id==1989 && raw_choice==-1){ctx.f1.f64=em2::em1::OriginalInkPakMounted()?1:0;sub_8295FEA8(ctx,base);return;}
            if(std::isfinite(raw_id) && raw_id>=2700 && raw_id<=2715 && std::floor(raw_id)==raw_id &&
               std::isfinite(raw_choice) && raw_choice>=-1 && raw_choice<=1024 && std::floor(raw_choice)==raw_choice){
                int result=0;if(em2::net::NativeOnlineMenuOption(int(raw_id),int(raw_choice),result)){
                    ctx.f1.f64=result;sub_8295FEA8(ctx,base);return;
                }
            }
            if(raw_id==2501){
                if(raw_choice>=100 && raw_choice<=415 && std::floor(raw_choice)==raw_choice && REXCVAR_GET(em2_trace_online_observations) &&
                    (em2::net::SynchronizedSessionActive() || em2::net::NativeOnlineMenuQaActive())){
                    const auto code=unsigned(raw_choice),stage=code/100,flags=code%100;
                    if(stage>=1 && stage<=4 && flags<16){
                        static std::atomic<uint64_t> seen{0};const auto bit=uint64_t(1)<<((stage-1)*16+flags);
                        if(!(seen.fetch_or(bit,std::memory_order_relaxed)&bit))REXLOG_INFO("Native online autosave UI stage {} flags {:X} (Gen3=1 enabled=2 slot-widget=4 showing=8)",stage,flags);
                    }
                }
                bool authorized=false;
                if(raw_choice==1)authorized=em2::net::ClaimNativeOnlineSaveNotice() || em2::ClaimOfflineStartupSaveNotice();
                else if(raw_choice==2)authorized=em2::ClaimNativeLobbyQaFreshSaveNotice();
                else if(raw_choice==3)authorized=em2::NativeLobbyQaFreshSaveNoticeAccepted();
                ctx.f1.f64=authorized?1:0;
                sub_8295FEA8(ctx,base);return;
            }
            if(raw_id==2500 && std::isfinite(raw_choice) && raw_choice>=-1 && raw_choice<=3 && std::floor(raw_choice)==raw_choice){
                int result=0;if(em2::EngineWorkbenchOption(2500,int(raw_choice),result)){ctx.f1.f64=result;sub_8295FEA8(ctx,base);return;}
            }
            if(std::isfinite(raw_id) && raw_id==1996) {
                ctx.f1.f64=int(em2::SceneLightingSettingsStatus());sub_8295FEA8(ctx,base);return;
            }
            if(std::isfinite(raw_id) && raw_id==1997) {
                ctx.f1.f64=int(em2::HdrSettingsStatus());sub_8295FEA8(ctx,base);return;
            }
            if(std::isfinite(raw_id) && raw_id==1998) {
                ctx.f1.f64=int(em2::RendererScaleStatus());sub_8295FEA8(ctx,base);return;
            }
            if(std::isfinite(raw_id) && raw_id==1999) {
                ctx.f1.f64=int(em2::LiveShadowResizeStatus());
                sub_8295FEA8(ctx,base);
                return;
            }
            if(std::isfinite(raw_id) && raw_id>=2000 && raw_id<=2331 && std::floor(raw_id)==raw_id &&
               std::isfinite(raw_choice) && raw_choice>=-5 && raw_choice<=1000 && std::floor(raw_choice)==raw_choice) {
                int result=0;
                if(em2::ControlOption(int(raw_id),int(raw_choice),result)) {
                    ctx.f1.f64=result;
                    sub_8295FEA8(ctx,base);
                    return;
                }
            }
            if(std::isfinite(raw_id) && raw_id>=6000 && raw_id<6100 && std::floor(raw_id)==raw_id && std::isfinite(raw_choice) && raw_choice>=-2 && raw_choice<=100 && std::floor(raw_choice)==raw_choice){
                int result=0;if(em2::GameplayModOption(int(raw_id),int(raw_choice),result)){ctx.f1.f64=result;sub_8295FEA8(ctx,base);return;}
            }
            if(std::isfinite(raw_id) && raw_id>=em2::kPCOptionBase && raw_id<em2::kPCOptionBase+em2::kPCOptionCount && std::floor(raw_id)==raw_id) {
                const auto index=int(raw_id)-em2::kPCOptionBase;
                auto result=em2::PCSetting(index);
                if(raw_choice==-2) result=em2::ChangePCSetting(index,em2::PCSettingDefault(index));
                if(std::isfinite(raw_choice) && raw_choice>=0 && raw_choice<=100 && std::floor(raw_choice)==raw_choice) result=em2::ChangePCSetting(index,int(raw_choice));
                ctx.f1.f64=result;
                sub_8295FEA8(ctx,base);
                return;
            }
        }
    }
    __imp__sub_82B0D118(ctx,base);
}

namespace em2 {
void TestPCOptionCallback(PPCContext& ctx,uint8_t* base) { sub_82B0D118(ctx,base); }
}


#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include <memory>
namespace rex::input { class InputDriver; class InputSystem; struct X_INPUT_STATE; }
namespace rex::ui { class Window; }
namespace em2::net { struct Frame; struct Pad; }
namespace em2 {
constexpr int kBindingCount=28;
constexpr int kPadSourceCount=25;
struct GameplayModInput {bool sprint=false,dash=false,pound=false,pad_sprint=false,moving=false;};
GameplayModInput ReadGameplayModInput(unsigned user);
// Consume a successful local physical-device poll. Agreed online frames
// already contain canonical controls and must bypass this transformation.
void RemapLocalControlState(unsigned user,rex::input::X_INPUT_STATE&);
constexpr int kWheelUp=512,kWheelDown=513,kWheelLeft=514,kWheelRight=515;
void LoadControls(const std::filesystem::path& directory);
void SetControlUIContext(bool active);
void ControlKey(unsigned key,bool down);
// Window wheel units (120 per detent); directions become one-tick bindings.
void ControlWheel(int horizontal,int vertical);
void ClearControlKeys();
void SetExternalControlCapture(bool captured);
void SetChatControlCapture(bool captured);
// Simulation-thread online setup: a temporary native P2 Start controller.
// Ends before canonical session input attaches; never alters saved bindings.
void SetOnlineJoinControls(bool enabled,bool press_start=false);
bool ControlOption(int id,int command,int& result);
bool TestControls(const std::filesystem::path& directory);
// Internal online adapter: publish canonical, already-remapped controller
// frames only after the simulation barrier agrees on the next exact tick.
bool BeginSynchronizedControls(uint64_t first_tick);
bool PublishSynchronizedControls(const net::Frame&);
// Read-only admission proof. Returns only a fully published frame on the
// thread that began this generation; never polls or consumes local input.
bool ReadPublishedSynchronizedControls(uint64_t& generation,net::Frame& frame);
// Owner-thread lease identity, including before the first frame is published.
uint64_t SynchronizedControlsGeneration();
// One-time raw-bootstrap -> local gameplay sampler cutover. No device poll,
// controller republish/restart or RNG operation; all sampler caches must be unused.
bool BeginSynchronizedGameplaySampling(uint64_t controls_generation,uint64_t completed_bootstrap_tick);
void EndSynchronizedControls();
// Generation-bound shutdown; cannot release a replacement controls owner.
bool EndSynchronizedControls(uint64_t generation);
// Sample once per sequential local tick, before adding the network delay.
// Repeated calls for a tick return its cached sample, not newer device input.
bool SampleLocalControls(rex::input::InputSystem&,unsigned user,uint64_t tick,net::Pad&);
struct LiveLookInput {
    int16_t x=0,y=0;
    bool operator==(const LiveLookInput&) const = default;
};
// Device-axis stage only: raw SDK deadzone gate, authored [-100,100]
// integer output and .01 scale. Action binding/response curves are separate.
bool ConvertNativeDeviceLook(const LiveLookInput&,float native_deadzone,std::array<float,2>&);
struct MouseLookFactors {float x=1,y=1;};
MouseLookFactors MouseLookStrength();
bool ControlBindingsOpen();
// Current device state for a separate camera update. Never consumes gameplay
// taps, dispatches actions, or changes canonical online input. False/zero while
// controls are suppressed or a synchronized session owns the controllers.
bool SampleLiveLook(rex::input::InputSystem&,unsigned user,LiveLookInput&);
std::unique_ptr<rex::input::InputDriver> CreateKeyboardFallback(rex::ui::Window* window);
}

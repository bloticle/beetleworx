#include "audio_lifetime.h"
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_driver.h>

namespace {
// Expose existing base-class member pointers without downcasting an object to
// an artificial derived type. Layout is supplied by the installed SDK headers.
struct SystemAccess : rex::audio::AudioSystem {
    static auto& Clients(rex::audio::AudioSystem& audio){return audio.*&SystemAccess::clients_;}
    static auto& Region(rex::audio::AudioSystem& audio){return audio.*&SystemAccess::global_critical_region_;}
};
struct DriverAccess : rex::audio::sdl::SDLAudioDriver {
    static auto Device(rex::audio::sdl::SDLAudioDriver& driver){return driver.*&DriverAccess::sdl_device_id_;}
};
}
void em2::QuiesceHostAudioCallbacks(rex::audio::AudioSystem& audio){
    auto lock=SystemAccess::Region(audio).Acquire();
    for(auto& client:SystemAccess::Clients(audio)){
        if(!client.in_use)continue;
        if(auto* driver=dynamic_cast<rex::audio::sdl::SDLAudioDriver*>(client.driver)){
            const auto device=DriverAccess::Device(*driver);
            if(device && device!=SDL_AudioDeviceID(-1)){
                SDL_PauseAudioDevice(device,1);
                SDL_LockAudioDevice(device);SDL_UnlockAudioDevice(device);
            }
        }
    }
}
void em2::ReleaseStoppedAudioClients(rex::audio::AudioSystem& audio){
    // The SDK's audio worker and title workers have stopped. Close devices
    // while the client semaphores and memory allocator still exist.
    auto& clients=SystemAccess::Clients(audio);
    for(size_t i=0;i<std::size(clients);++i)if(clients[i].in_use)audio.UnregisterClient(i);
}

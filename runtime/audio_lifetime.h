#pragma once
namespace rex::audio { class AudioSystem; }
namespace em2 {
void QuiesceHostAudioCallbacks(rex::audio::AudioSystem& audio);
void ReleaseStoppedAudioClients(rex::audio::AudioSystem& audio);
}

#pragma once
#include "online_voice_audio.h"
#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace em2::net {
struct VoiceKeys {
    std::array<uint8_t,32> to_host{},to_guest{};
    ~VoiceKeys();
};
struct ReceivedVoice {unsigned peer=0;VoicePacket packet;};
struct VoiceTransportStats {uint64_t received=0,sent=0,rejected=0,dropped=0;};
// Single network owner; no device access, threads, or simulation operations.
// Keys come from the admitted Room control channel. UDP authentication does
// not upgrade that currently unencrypted control channel into Internet TLS.
class VoiceTransport {
    friend bool TestOnlineVoiceTransport();
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    VoiceTransport();~VoiceTransport();
    VoiceTransport(const VoiceTransport&)=delete;
    VoiceTransport& operator=(const VoiceTransport&)=delete;
    bool Host(std::string_view bind_address,const std::array<uint8_t,16>& session);
    bool AddPeer(unsigned peer,std::string_view expected_ip,VoiceKeys& offer);
    bool Join(std::string_view host_address,uint16_t port,unsigned peer,const std::array<uint8_t,16>& session,const VoiceKeys&);
    void RemovePeer(unsigned peer);
    void SetRoster(uint16_t mask);
    void Poll(VoiceTime now=std::chrono::steady_clock::now());
    bool Send(const VoicePacket&,VoiceTime now=std::chrono::steady_clock::now());
    std::vector<ReceivedVoice> TakeReceived();
    bool ready()const;
    bool peer_ready(unsigned peer)const;
    uint16_t port()const;
    VoiceTransportStats stats()const;
};
bool TestOnlineVoiceTransport();
}

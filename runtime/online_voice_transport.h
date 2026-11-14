#pragma once
#include "online_voice_audio.h"
#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace em2::net {
struct VoiceKeys {
    std::array<uint8_t,32> to_host{},to_guest{};
    ~VoiceKeys();
};
struct ReceivedVoice {unsigned peer=0;VoicePacket packet;};
struct VoiceTransportStats {uint64_t received=0,sent=0,rejected=0,dropped=0;};
inline constexpr size_t kVoiceDatagramBytes=512;
struct VoiceDatagram {
    // Authenticated channel identity, not a sender claimed by packet contents.
    unsigned sender=0;
    uint16_t size=0;
    std::array<uint8_t,kVoiceDatagramBytes> data{};
};
// A separate media connection supplies bounded encrypted datagrams. Host-side
// sender IDs must come from admitted gameplay-channel mappings; guests receive
// only sender 1. The existing cipher, replay window and roster still validate
// every datagram before audio reaches the mixer. Send/Receive must be
// nonblocking, with bounded queues that discard stale media instead of
// stalling the game's network owner.
class VoiceDatagramChannel {
public:
    virtual ~VoiceDatagramChannel()=default;
    virtual bool ready()const=0;
    virtual bool Send(unsigned destination,std::span<const uint8_t>)=0;
    virtual std::optional<VoiceDatagram> Receive()=0;
};
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
    // Install before Host/Join. A media channel does not carry gameplay or
    // checkpoint traffic, and its failure never changes the Room timeline.
    bool UseDatagramChannel(std::shared_ptr<VoiceDatagramChannel>);
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

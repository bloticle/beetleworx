#pragma once
#include <memory>
#include <string>

namespace em2::net {
struct InternetRelayStatus {
    bool ready=false,failed=false;
    std::string code,invitation,error;
};
// Outbound TLS WebSockets plus loopback stream bridging in the game process.
// Room still owns the original handshake, framing, checkpoint and tick proofs.
class InternetRelay {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    InternetRelay();
    ~InternetRelay();
    InternetRelay(const InternetRelay&)=delete;
    InternetRelay& operator=(const InternetRelay&)=delete;
    bool Host(std::string https_origin,std::string version,std::string invitation);
    bool Join(std::string https_origin,std::string version,std::string room_code);
    void Stop()noexcept;
    void SetJoinable(bool);
    bool finished()const noexcept;
    InternetRelayStatus status()const;
};
}

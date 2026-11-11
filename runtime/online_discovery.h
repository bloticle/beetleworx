#pragma once
#include "online_invite.h"
#include <chrono>

namespace em2::net {
inline constexpr uint16_t kLANDiscoveryPort=41037;
struct LANDiscoveryTarget {std::string address;uint16_t port=kLANDiscoveryPort;};
struct LANGame {std::string invitation,address;uint16_t port=0;bool operator==(const LANGame&)const=default;};
std::vector<LANDiscoveryTarget> LocalLANDiscoveryTargets();
class LANAdvertiser {
public:
    LANAdvertiser();~LANAdvertiser();
    bool Start(const Invite&,std::string_view bind_address="0.0.0.0",uint16_t port=kLANDiscoveryPort);
    void Poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    void Stop();
    uint16_t port()const;
    const std::string& error()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
class LANBrowser {
public:
    LANBrowser();~LANBrowser();
    bool Start(const std::vector<LANDiscoveryTarget>&,std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    void Poll(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
    void Stop();
    bool scanning()const;
    const std::vector<LANGame>& games()const;
    const std::string& error()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool TestLANDiscovery();
}

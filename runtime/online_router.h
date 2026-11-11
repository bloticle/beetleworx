#pragma once
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <functional>
namespace em2::net {
class UpnpGateway;
using UpnpFactory=std::function<std::unique_ptr<UpnpGateway>()>;
enum class RouterState {Idle,Discovering,Mapping,Active,Failed};
struct RouterStatus {RouterState state=RouterState::Idle;std::string address,error;uint16_t port=0;};
// Single-owner nonblocking PCP/NAT-PMP/UPnP lease. Start is explicitly requested by the
// host. Transfer ownership to Session before gameplay to keep renewing it.
class RouterMapping {
public:
    using Clock=std::chrono::steady_clock;using Time=Clock::time_point;
    RouterMapping();~RouterMapping();
    RouterMapping(const RouterMapping&)=delete;RouterMapping& operator=(const RouterMapping&)=delete;
    bool Start(std::string_view local_address,uint16_t internal_port);
    void Poll(Time now=Clock::now());
    void Stop();
    const RouterStatus& status()const;
    uint16_t internal_port()const;
private:
    bool Open(std::string_view local,uint16_t internal,std::string_view gateway,uint16_t gateway_port,Time,bool prefer_pcp=false,UpnpFactory fallback={});
    struct Impl;std::unique_ptr<Impl> impl_;
    friend bool TestRouterMapping();
    friend bool TestUpnpRouterMapping();
};
bool TestRouterMapping();
std::optional<std::string> InternetInvitation(std::string_view,const RouterStatus&);
}

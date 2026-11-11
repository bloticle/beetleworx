#pragma once
#include "online_router.h"
namespace em2::net {
struct UpnpEntry {
    std::string client,description;
    uint16_t internal=0;
    uint32_t lifetime=0;
    bool enabled=false;
};
enum class UpnpQuery {Missing,Found,Failed};
// All backend methods (including construction/destruction) run on one COM MTA
// worker. Tests inject an in-memory backend; they never discover a real router.
class UpnpGateway {
public:
    virtual ~UpnpGateway()=default;
    virtual bool Discover(std::string_view gateway,std::string& error)=0;
    virtual bool ExternalAddress(std::string&,std::string& error)=0;
    virtual UpnpQuery Query(uint16_t external,UpnpEntry&,std::string& error)=0;
    virtual bool Add(uint16_t external,const UpnpEntry&,std::string& error)=0;
    virtual bool Delete(uint16_t external,std::string& error)=0;
};
std::unique_ptr<UpnpGateway> MakeNativeUpnpGateway();
class UpnpLease {
public:
    using Time=RouterMapping::Time;
    UpnpLease();~UpnpLease();
    bool Start(std::string local,std::string gateway,uint16_t internal,Time,UpnpFactory);
    void Poll(Time);
    void Stop();
    const RouterStatus& status()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    friend bool TestUpnpRouterMapping();
};
bool TestUpnpRouterMapping();
bool TestNativeUpnpAutomation();
bool UpnpWorkerIdleForTest();
}

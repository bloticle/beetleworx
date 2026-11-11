#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace em2 {
struct CameraTargetPose {
    uint32_t registry=0,handle=0,entity=0,component=0;
    std::array<float,3> position{};
};

// Camera queries need the same simulation interval as the visible character.
// The native entity component remains authoritative outside those queries.
class CameraTargetHistory {
public:
    void Reset();
    void Capture(const CameraTargetPose&,uint64_t tick,bool discontinuity=false);
    std::optional<std::array<float,3>> Sample(const CameraTargetPose&,uint64_t tick,double alpha)const;
private:
    CameraTargetPose previous_{},current_{};
    uint64_t tick_=0;
    bool valid_=false;
};
class CameraTargetQueryScope {
public:
    CameraTargetQueryScope(const CameraTargetHistory&,const CameraTargetPose&,uint64_t tick,double alpha);
    ~CameraTargetQueryScope();
    CameraTargetQueryScope(const CameraTargetQueryScope&)=delete;
    static void OverridePosition(uint32_t entity,uint32_t output,uint8_t* base);
private:
    struct Query {
        const CameraTargetHistory* history;
        CameraTargetPose target;
        uint64_t tick;
        double alpha;
    };
    inline static thread_local const Query* active_=nullptr;
    Query query_;
    const Query* previous_;
};
bool TestCameraTargetHistory();
}

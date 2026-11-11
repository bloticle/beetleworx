#pragma once
#include <mutex>
namespace rex::graphics {class GraphicsSystem;}
namespace em2 {
class Em2CommandProcessor;
// GPU-worker only, outside packet execution. Drain one published prefix, then
// pin its interpreted state and hold the native recursive memory lock. Later
// drains flush/fence migration GPU work without consuming newly queued packets.
// Watched CPU invalidation waits on the same lock. Raw writes to unwatched
// memory and guest simulation are not paused. Busy memory regions reject
// without waiting; callers retry later. No nested or cross-thread scopes.
class GraphicsResourceCheckpoint {
public:
    explicit GraphicsResourceCheckpoint(Em2CommandProcessor&);
    ~GraphicsResourceCheckpoint();
    bool valid() const{return processor_!=nullptr;}
    bool owns(const Em2CommandProcessor& cp) const{return processor_==&cp;}
    GraphicsResourceCheckpoint(const GraphicsResourceCheckpoint&)=delete;
    GraphicsResourceCheckpoint& operator=(const GraphicsResourceCheckpoint&)=delete;
private:
    Em2CommandProcessor* processor_=nullptr;
    std::unique_lock<std::recursive_mutex> memory_lock_;
};

// The caller must stop producing commands that reference the resources it will
// replace. Drains published guest commands, then awaits native GPU submissions.
bool SynchronizeGraphicsForResourceChange();
bool TestGraphicsResourceSynchronization(rex::graphics::GraphicsSystem&);
bool TestGraphicsResourceSynchronizationDispatch();
}

#pragma once
#include <rex/graphics/util/draw.h>
namespace rex::graphics {class TextureCache;}
namespace em2 {
struct ResolveWriteObservation {
    rex::graphics::TextureCache* cache;
    uint32_t address,length,scale_x,scale_y;
    // Valid only for this callback. Null means no trustworthy matching native
    // geometry was observed; consumers must treat the write as untyped.
    const rex::graphics::draw_util::ResolveInfo* info;
};
struct ResolveWriteObserver {
    void* context=nullptr;
    void (*written)(void*,const ResolveWriteObservation&) noexcept=nullptr;
    // Optional geometry preparation before allocation/dispatch succeeds. This
    // must not publish validity; written remains the only commit notification.
    void (*prepared)(void*,const ResolveWriteObservation&) noexcept=nullptr;
};
// GPU-worker scope around one native IssueCopy. No trace re-read or duplicate
// GetResolveInfo call. Nested scopes restore their previous observation state.
// Callback follows the native MarkRangeAsResolved under the global memory lock.
// This observes writes only; typed storage and CPU invalidation tracking are
// separate requirements before resolved memory can migrate.
class ResolveWriteScope {
public:
    ResolveWriteScope(rex::graphics::TextureCache*,ResolveWriteObserver*) noexcept;
    ~ResolveWriteScope();
    ResolveWriteScope(const ResolveWriteScope&)=delete;
    ResolveWriteScope& operator=(const ResolveWriteScope&)=delete;
private:
    rex::graphics::TextureCache* cache_;
    ResolveWriteObserver* observer_;
    ResolveWriteScope* previous_;
    rex::graphics::draw_util::ResolveInfo info_{};
    uint32_t sx_=0,sy_=0;
    bool valid_=false;
    friend void ObserveNativeResolveInfo(bool,const rex::graphics::draw_util::ResolveInfo&,uint32_t,uint32_t) noexcept;
    friend void ObserveNativeResolveWrite(rex::graphics::TextureCache*,uint32_t,uint32_t) noexcept;
    friend bool TestResolveWriteObserver(class Em2CommandProcessor&);
};
void ObserveNativeResolveInfo(bool,const rex::graphics::draw_util::ResolveInfo&,uint32_t,uint32_t) noexcept;
void ObserveNativeResolveWrite(rex::graphics::TextureCache*,uint32_t,uint32_t) noexcept;
bool TestResolveWriteObserver(class Em2CommandProcessor&);
}

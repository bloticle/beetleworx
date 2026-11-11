#pragma once
namespace rex::graphics::d3d12 {class D3D12RenderTargetCache;}
namespace em2 {
class Em2CommandProcessor;class GraphicsResourceCheckpoint;
// Temporary EDRAM system views for native bindless staging operations. All
// caches and the caller's checkpoint must outlive this scope. Bindful mode is
// a validated no-op. Changing descriptor targets requires GPU completion.
// Explicitly Restore before discarding a failed stage. A failed GPU drain
// leaves bindings/resources retained for recovery; do not resume the old graph.
class EdramSystemBindings {
public:
    EdramSystemBindings(Em2CommandProcessor&,const GraphicsResourceCheckpoint&,
        rex::graphics::d3d12::D3D12RenderTargetCache& staging);
    ~EdramSystemBindings();
    bool valid() const{return processor_!=nullptr;}
    bool Restore();
    // Keep the staging views only after its cache has become the processor's
    // current render-target cache. Full graph publication is the caller's job.
    bool Commit();
    EdramSystemBindings(const EdramSystemBindings&)=delete;
    EdramSystemBindings& operator=(const EdramSystemBindings&)=delete;
private:
    Em2CommandProcessor* processor_=nullptr;
    rex::graphics::d3d12::D3D12RenderTargetCache *previous_=nullptr,*target_=nullptr;
    bool bindless_=false;
};
}

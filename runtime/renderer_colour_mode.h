#pragma once
#include <functional>
#include <memory>
namespace rex::ui {class Presenter;}
namespace rex::graphics {class GraphicsSystem;}
namespace em2 {
class Em2CommandProcessor;
enum class LiveColourModeStatus {Applied,Busy,CaptureFailed,BuildFailed,RestoreFailed,PresenterFailed,GpuFailure,Unsupported};
// Called while GPU producers are stopped at a resource checkpoint. The host
// marshals to the UI thread, detaches its window/drawers, exchanges ownership,
// then reattaches them. Success leaves the previous presenter in the argument;
// failure must leave everything unchanged. The callback also supports rollback
// and must not throw after changing ownership or attachment.
using PresenterExchange=std::function<bool(std::unique_ptr<rex::ui::Presenter>&)>;
void ExchangeGraphicsPresenter(rex::graphics::GraphicsSystem&,std::unique_ptr<rex::ui::Presenter>&);
LiveColourModeStatus ApplyRendererColourMode(Em2CommandProcessor&,bool hdr,const PresenterExchange&);
}

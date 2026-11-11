#include "presenter_ui_exchange.h"
#include "renderer_colour_mode.h"
#include <rex/graphics/graphics_system.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <dxgi1_6.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <exception>
namespace {
struct Request {
 enum class Phase {Queued,Running,Done,Cancelled};
 std::mutex mutex;std::condition_variable changed;Phase phase=Phase::Queued;bool result=false;
 std::unique_ptr<rex::ui::Presenter> candidate;
 std::exception_ptr error;
 void Cancel(){std::lock_guard lock(mutex);if(phase==Phase::Queued){phase=Phase::Cancelled;changed.notify_all();}}
};
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct ConnectionTag{friend auto MemberOf(ConnectionTag);};template struct Access<ConnectionTag,&rex::ui::Presenter::surface_paint_connection_state_>;
struct ConnectedTag{friend auto MemberOf(ConnectedTag);};template struct Access<ConnectedTag,&rex::ui::Presenter::IsConnectedSurfacePaintConnectionState>;
struct WindowPresenterTag{friend auto MemberOf(WindowPresenterTag);};template struct Access<WindowPresenterTag,&rex::ui::Window::presenter>;
struct PaintTag{friend auto MemberOf(PaintTag);};template struct Access<PaintTag,&rex::ui::d3d12::D3D12Presenter::paint_context_>;
bool Connected(rex::ui::Presenter* p){return p && MemberOf(ConnectedTag{})(p->*MemberOf(ConnectionTag{}));}
}
bool em2::DesktopHdrEnabledFromUIThread(rex::ui::Presenter* presenter){
 auto* p=dynamic_cast<rex::ui::d3d12::D3D12Presenter*>(presenter);if(!p || !Connected(p))return false;
 auto* swap=(p->*MemberOf(PaintTag{})).swap_chain.Get();if(!swap)return false;
 Microsoft::WRL::ComPtr<IDXGIOutput> output;Microsoft::WRL::ComPtr<IDXGIOutput6> advanced;DXGI_OUTPUT_DESC1 desc{};
 return SUCCEEDED(swap->GetContainingOutput(&output)) && SUCCEEDED(output.As(&advanced)) && SUCCEEDED(advanced->GetDesc1(&desc)) &&
  (desc.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || desc.ColorSpace==DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020);
}
struct em2::PresenterUiExchange::State {
 std::mutex mutex;bool closed=false;Post post;Handler handler;std::thread::id owner=std::this_thread::get_id();std::chrono::milliseconds timeout;std::vector<std::weak_ptr<Request>> pending;
};
em2::PresenterUiExchange::PresenterUiExchange(Post post,Handler handler,std::chrono::milliseconds timeout):state_(std::make_shared<State>()){
 state_->post=std::move(post);state_->handler=std::move(handler);state_->timeout=timeout;
}
em2::PresenterUiExchange::~PresenterUiExchange(){Close();}
void em2::PresenterUiExchange::Close(){
 auto s=state_;std::lock_guard lock(s->mutex);s->closed=true;s->handler={};for(auto& weak:s->pending)if(auto r=weak.lock())r->Cancel();s->pending.clear();
}
bool em2::PresenterUiExchange::Exchange(std::unique_ptr<rex::ui::Presenter>& candidate){
 auto s=state_;if(std::this_thread::get_id()==s->owner)return false;
 auto r=std::make_shared<Request>();Post post;
 {std::lock_guard lock(s->mutex);if(s->closed || !s->post || !s->handler)return false;post=s->post;std::erase_if(s->pending,[](const auto& weak){return weak.expired();});s->pending.push_back(r);r->candidate=std::move(candidate);}
 try {post([s,r]{
  Handler handler;
  {std::lock_guard shared(s->mutex);std::lock_guard request(r->mutex);
   if(r->phase!=Request::Phase::Queued)return;
   if(s->closed || std::this_thread::get_id()!=s->owner){r->phase=Request::Phase::Cancelled;r->changed.notify_all();return;}
   try{handler=s->handler;}catch(...){r->phase=Request::Phase::Cancelled;r->changed.notify_all();return;}
   r->phase=Request::Phase::Running;
  }
  bool result=false;std::exception_ptr error;try{result=handler(r->candidate);}catch(...){error=std::current_exception();}
  // A thrown attachment operation may have changed ownership. Propagate it to
  // the GPU coordinator rather than reporting an unchanged failed operation.
  {std::lock_guard lock(r->mutex);r->result=result;r->error=error;r->phase=Request::Phase::Done;}
  r->changed.notify_all();
 });}catch(...){r->Cancel();}
 std::unique_lock lock(r->mutex);
 if(!r->changed.wait_for(lock,s->timeout,[&]{return r->phase!=Request::Phase::Queued;}))r->phase=Request::Phase::Cancelled;
 // Once attachment starts, ownership cannot safely be handed back on timeout.
 r->changed.wait(lock,[&]{return r->phase!=Request::Phase::Running;});
 candidate=std::move(r->candidate);const bool result=r->phase==Request::Phase::Done && r->result;auto error=r->error;lock.unlock();if(error)std::rethrow_exception(error);return result;
}
bool em2::ExchangeWindowPresenter(rex::graphics::GraphicsSystem& graphics,rex::ui::Window& window,rex::ui::ImmediateDrawer* immediate,rex::ui::ImGuiDrawer* imgui,std::unique_ptr<rex::ui::Presenter>& candidate){
 auto* original=graphics.presenter();if(!candidate || !original || (window.*MemberOf(WindowPresenterTag{}))()!=original || !Connected(original))return false;
 const auto old_config=original->GetGuestOutputPaintConfigFromUIThread(),candidate_config=candidate->GetGuestOutputPaintConfigFromUIThread();bool swapped=false;
 auto detach=[&]{if(imgui)imgui->SetPresenterAndImmediateDrawer(nullptr,nullptr);if(immediate)immediate->SetPresenter(nullptr);window.SetPresenter(nullptr);};
 auto attach=[&]{auto* p=graphics.presenter();if(immediate)immediate->SetPresenter(p);if(imgui)imgui->SetPresenterAndImmediateDrawer(p,immediate);window.SetPresenter(p);};
 try {detach();ExchangeGraphicsPresenter(graphics,candidate);swapped=true;graphics.presenter()->SetGuestOutputPaintConfigFromUIThread(old_config);attach();if(Connected(graphics.presenter()))return true;}catch(...){}
 try {detach();if(swapped)ExchangeGraphicsPresenter(graphics,candidate);candidate->SetGuestOutputPaintConfigFromUIThread(candidate_config);original->SetGuestOutputPaintConfigFromUIThread(old_config);attach();if(graphics.presenter()!=original || (window.*MemberOf(WindowPresenterTag{}))()!=original || !Connected(original))throw std::runtime_error("Presenter window rollback did not reconnect");}
 catch(...){throw std::runtime_error("Presenter window/overlay rollback failed");}
 return false;
}

#include "presenter_ui_exchange.h"
#include "renderer_colour_mode.h"
#include "renderer_graph.h"
#include "hdr_presenter.h"
#include "gpu_constants_internal.h"
#include <rex/graphics/graphics_system.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/logging.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <fstream>
namespace {
class UiThread {
 std::mutex mutex_;std::condition_variable ready_;std::deque<std::function<void()>> normal_,control_;bool paused_=false,stop_=false;std::thread thread_;
public:
 UiThread():thread_([this]{for(;;){std::function<void()> task;{std::unique_lock lock(mutex_);ready_.wait(lock,[&]{return stop_ || !control_.empty() || (!paused_ && !normal_.empty());});if(stop_)return;auto& q=control_.empty()?normal_:control_;task=std::move(q.front());q.pop_front();}task();}}){}
 ~UiThread(){{std::lock_guard lock(mutex_);stop_=true;}ready_.notify_all();thread_.join();}
 void Post(std::function<void()> task,bool control=false){{std::lock_guard lock(mutex_);(control?control_:normal_).push_back(std::move(task));}ready_.notify_all();}
 void Invoke(std::function<void()> task,bool control=true){auto done=std::make_shared<std::promise<void>>();auto result=done->get_future();Post([task=std::move(task),done]{try{task();done->set_value();}catch(...){done->set_exception(std::current_exception());}},control);result.get();}
 void Pause(bool pause){{std::lock_guard lock(mutex_);paused_=pause;}ready_.notify_all();}
};
using Base=rex::graphics::CommandProcessor;
template<class Tag,auto Member>struct Access{friend auto MemberOf(Tag){return Member;}};
struct GraphicsTag{friend auto MemberOf(GraphicsTag);};template struct Access<GraphicsTag,&Base::graphics_system_>;
}
bool em2::TestPresenterUiExchange(Em2CommandProcessor& cp,const std::filesystem::path& directory){
 if(!cp.IsGraphicsWorker() || !cp.DrainForResourceChange())return false;
 auto* graphics=cp.*MemberOf(GraphicsTag{});if(!graphics)return false;
 UiThread ui;std::shared_ptr<PresenterUiExchange> bridge;std::atomic<unsigned> posts{0},calls{0};std::thread::id owner;bool wrong_owner=false;
 std::mutex posted_mutex;std::condition_variable posted;
 ui.Invoke([&]{owner=std::this_thread::get_id();bridge=std::make_shared<PresenterUiExchange>(
  [&](std::function<void()> task){ui.Post(std::move(task));{std::lock_guard lock(posted_mutex);posts.fetch_add(1);}posted.notify_all();},
  [&](std::unique_ptr<rex::ui::Presenter>& candidate){wrong_owner|=std::this_thread::get_id()!=owner;++calls;ExchangeGraphicsPresenter(*graphics,candidate);return true;},std::chrono::milliseconds(250));});
 PipelineShaderSnapshot before;if(!CapturePipelineShaders(cp,before))return false;
 auto exchange=[&](std::unique_ptr<rex::ui::Presenter>& candidate){return bridge->Exchange(candidate);};
 for(bool hdr:{!before.hdr_scene,before.hdr_scene})if(ApplyRendererColourMode(cp,hdr,exchange)!=LiveColourModeStatus::Applied)return false;
 PipelineShaderSnapshot after;if(!CapturePipelineShaders(cp,after) || after.hdr_scene!=before.hdr_scene || calls!=2 || wrong_owner)return false;
 std::unique_ptr<rex::ui::Presenter> candidate=rex::ui::d3d12::D3D12Presenter::Create({},cp.GetD3D12Provider());if(!candidate)return false;auto* original=candidate.get();auto* current=graphics->presenter();
 ui.Pause(true);const bool timeout_cancelled=!bridge->Exchange(candidate) && candidate.get()==original && graphics->presenter()==current && calls==2;
 const auto previous_posts=posts.load();std::promise<bool> cancelled;auto result=cancelled.get_future();
 std::thread waiting([&]{try{cancelled.set_value(bridge->Exchange(candidate));}catch(...){cancelled.set_exception(std::current_exception());}});
 bool enqueued=false;{std::unique_lock lock(posted_mutex);enqueued=posted.wait_for(lock,std::chrono::seconds(1),[&]{return posts.load()>previous_posts;});}
 const bool pending_before_close=result.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready;
 ui.Invoke([&]{bridge->Close();});waiting.join();const bool shutdown_cancelled=enqueued && pending_before_close && !result.get() && candidate.get()==original && graphics->presenter()==current;
 ui.Pause(false);ui.Invoke([]{},false);const bool no_late_handler=calls==2 && !bridge->Exchange(candidate) && candidate.get()==original;
 std::shared_ptr<PresenterUiExchange> throwing;bool owner_guard=false,propagated=false;
 ui.Invoke([&]{throwing=std::make_shared<PresenterUiExchange>([&](std::function<void()> task){ui.Post(std::move(task));},[](std::unique_ptr<rex::ui::Presenter>&)->bool{throw std::runtime_error("injected UI failure");});std::unique_ptr<rex::ui::Presenter> empty;owner_guard=!throwing->Exchange(empty);});
 try{throwing->Exchange(candidate);}catch(const std::runtime_error&){propagated=candidate.get()==original;}
 ui.Invoke([&]{throwing->Close();bridge->Close();});
 std::filesystem::create_directories(directory);std::ofstream report(directory/"presenter-ui-exchange-results.json");report<<"{\n\"published_mode_changes\":2,\n\"handler_calls\":"<<calls.load()<<",\n\"timeout_cancelled\":"<<(timeout_cancelled?"true":"false")<<",\n\"shutdown_cancelled\":"<<(shutdown_cancelled?"true":"false")<<",\n\"no_late_handler\":"<<(no_late_handler?"true":"false")<<",\n\"owner_thread_guard\":"<<(owner_guard?"true":"false")<<",\n\"exception_propagated\":"<<(propagated?"true":"false")<<",\n\"scope\":\"real native graph/presenter publication through dedicated simulated UI thread; cancellation, close and late delivery with owned native presenters; no native window/drawers/Present/title\"\n}\n";
 REXLOG_INFO("Presenter UI transfer: two native mode publications, handler calls {}, timeout {}, shutdown {}, late {}, owner guard {}, exception {}; dedicated UI executor, no window",calls.load(),timeout_cancelled,shutdown_cancelled,no_late_handler,owner_guard,propagated);
 return timeout_cancelled && shutdown_cancelled && no_late_handler && owner_guard && propagated && !wrong_owner && report.good();
}

#include "guest_shutdown.h"
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/xthread.h>
#include <rex/logging.h>
#include <rex/cvar.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <windows.h>
REXCVAR_DEFINE_BOOL(em2_cooperative_title_shutdown,true,"Debug","Startup-only: use cancellable native guest waits and join title threads before module teardown").lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
namespace {
using namespace std::chrono_literals;
using Result=rex::thread::WaitResult;
using Duration=std::chrono::milliseconds;
thread_local bool guest_execution=false;
struct StopState {
 std::atomic<bool> requested{false};
 std::mutex threads_mutex;
 std::vector<rex::kernel::object_ref<rex::kernel::XThread>> started;
 std::unique_ptr<rex::thread::Event> event=rex::thread::Event::CreateManualResetEvent(false);
 StopState(){if(!event)throw std::runtime_error("Cannot create guest shutdown event");}
};
StopState& Stop(){static StopState state;return state;}
Duration NativeTimeout(Duration duration){const auto ms=DWORD(duration.count());return ms==INFINITE?Duration::max():Duration(ms);}
struct Deadline {
 bool infinite;std::chrono::steady_clock::time_point end;
 explicit Deadline(Duration timeout):infinite(timeout==Duration::max()),end(infinite?std::chrono::steady_clock::time_point::max():std::chrono::steady_clock::now()+timeout){}
 Duration remaining()const{if(infinite)return Duration::max();const auto rest=end-std::chrono::steady_clock::now();return rest<=rest.zero()?Duration(0):std::chrono::ceil<Duration>(rest);}
};
}
bool em2::CooperativeGuestShutdownEnabled(){static const bool startup=REXCVAR_GET(em2_cooperative_title_shutdown);return startup;}
em2::GuestExecutionScope::GuestExecutionScope(bool enabled):previous_(guest_execution){
 guest_execution=enabled;
 if(!enabled || previous_)return;
 auto* current=rex::kernel::XThread::GetCurrentThread();
 if(!current || !current->is_guest_thread())return;
 // Keep host handles reachable after the native guest releases its last
 // public handle, until the host thread has actually exited.
 std::vector<rex::kernel::object_ref<rex::kernel::XThread>> retired;
 auto& stop=Stop();
 {std::lock_guard lock(stop.threads_mutex);
  for(auto it=stop.started.begin();it!=stop.started.end();){
   if((*it)->thread() && rex::thread::Wait((*it)->thread(),false,0ms)==Result::kSuccess){retired.push_back(std::move(*it));it=stop.started.erase(it);}else ++it;
  }
  if(std::none_of(stop.started.begin(),stop.started.end(),[&](const auto& value){return value.get()==current;})){
   current->Retain();rex::kernel::object_ref<rex::kernel::XThread> retained(current);stop.started.push_back(std::move(retained));
  }
 }
 // XThread destruction may acquire the kernel region; never release the
 // retired references while holding the registry mutex.
}
em2::GuestExecutionScope::~GuestExecutionScope(){guest_execution=previous_;}
bool em2::GuestShutdownRequested(){return guest_execution && Stop().requested.load(std::memory_order_acquire);}
void em2::PollGuestShutdown(){if(GuestShutdownRequested())throw GuestShutdown{};}
void em2::RequestGuestShutdown(){Stop().requested.store(true,std::memory_order_release);Stop().event->Set();}
std::pair<Result,size_t> em2::GuestWaitAny(std::vector<rex::thread::WaitHandle*> handles,bool alertable,Duration timeout){
 if(!guest_execution)return rex::thread::WaitAny(std::move(handles),alertable,timeout);
 PollGuestShutdown();
 if(handles.empty() || handles.size()>=MAXIMUM_WAIT_OBJECTS){
  Deadline deadline(NativeTimeout(timeout));
  for(;;){const auto left=deadline.remaining();auto result=rex::thread::WaitAny(handles,alertable,std::min(left,10ms));PollGuestShutdown();if(result.first!=Result::kTimeout || left<=10ms)return result;}
 }
 const auto count=handles.size();handles.push_back(Stop().event.get());
 auto result=rex::thread::WaitAny(std::move(handles),alertable,timeout);
 PollGuestShutdown();
 if(result.first==Result::kSuccess && result.second==count)throw GuestShutdown{};
 return result;
}
Result em2::GuestWait(rex::thread::WaitHandle* handle,bool alertable,Duration timeout){
 if(!guest_execution)return rex::thread::Wait(handle,alertable,timeout);
 PollGuestShutdown();
 // Two stack handles avoid allocating on the common single-object path.
 HANDLE handles[]={handle->native_handle(),Stop().event->native_handle()};
 const DWORD result=WaitForMultipleObjectsEx(2,handles,FALSE,DWORD(timeout.count()),alertable?TRUE:FALSE);
 PollGuestShutdown();
 if(result==WAIT_OBJECT_0+1)throw GuestShutdown{};
 if(result==WAIT_OBJECT_0)return Result::kSuccess;
 if(result==WAIT_ABANDONED_0)return Result::kAbandoned;
 if(result==WAIT_IO_COMPLETION)return Result::kUserCallback;
 return result==WAIT_TIMEOUT?Result::kTimeout:Result::kFailed;
}
Result em2::GuestWaitAll(std::vector<rex::thread::WaitHandle*> handles,bool alertable,Duration timeout){
 if(!guest_execution)return rex::thread::WaitAll(std::move(handles),alertable,timeout);
 Deadline deadline(NativeTimeout(timeout));
 for(;;){PollGuestShutdown();const auto left=deadline.remaining();auto result=rex::thread::WaitAll(handles,alertable,std::min(left,10ms));PollGuestShutdown();if(result!=Result::kTimeout || left<=10ms)return result;}
}
Result em2::GuestSignalAndWait(rex::thread::WaitHandle* signal,rex::thread::WaitHandle* wait,bool alertable,Duration timeout){
 if(!guest_execution)return rex::thread::SignalAndWait(signal,wait,alertable,timeout);
 PollGuestShutdown();Deadline deadline(NativeTimeout(timeout));
 // Signal exactly once, preserving the native atomic signal/wait operation.
 const auto first=deadline.remaining();auto result=rex::thread::SignalAndWait(signal,wait,alertable,std::min(first,10ms));
 PollGuestShutdown();if(result!=Result::kTimeout || first<=10ms)return result;
 return GuestWait(wait,alertable,deadline.remaining());
}
void em2::GuestSleep(std::chrono::microseconds duration){
 if(!guest_execution){rex::thread::Sleep(duration);return;}
 PollGuestShutdown();
 if(duration.count()<1000){rex::thread::Sleep(duration);PollGuestShutdown();return;}
 auto result=rex::thread::Wait(Stop().event.get(),false,NativeTimeout(Duration(duration.count()/1000)));
 PollGuestShutdown();(void)result;
}
rex::thread::SleepResult em2::GuestAlertableSleep(std::chrono::microseconds duration){
 if(!guest_execution)return rex::thread::AlertableSleep(duration);
 if(duration.count()<1000){PollGuestShutdown();auto result=rex::thread::AlertableSleep(duration);PollGuestShutdown();return result;}
 PollGuestShutdown();auto result=rex::thread::Wait(Stop().event.get(),true,NativeTimeout(Duration(duration.count()/1000)));PollGuestShutdown();
 return result==Result::kUserCallback?rex::thread::SleepResult::kAlerted:rex::thread::SleepResult::kSuccess;
}
bool em2::JoinGuestShutdown(rex::kernel::KernelState& kernel,Duration timeout){
 Deadline deadline(timeout);unsigned quiet=0;
 do{
  std::vector<rex::kernel::object_ref<rex::kernel::XThread>> threads;
  {auto lock=rex::thread::global_critical_region::AcquireDirect();threads=kernel.object_table()->GetObjectsByType<rex::kernel::XThread>();}
  {std::lock_guard lock(Stop().threads_mutex);for(const auto& started:Stop().started)
   if(std::none_of(threads.begin(),threads.end(),[&](const auto& value){return value.get()==started.get();}))threads.push_back(started);}
  bool pending=false;
  for(auto& thread:threads){
   if(!thread->is_guest_thread())continue;
   auto* host=thread->thread();
   if(!host){pending=true;continue;}
   if(rex::thread::Wait(host,false,0ms)==Result::kSuccess)continue;
   pending=true;
   // A suspended guest must run far enough to unwind on its own thread.
   for(unsigned n=0;n<256 && thread->suspend_count();++n){if(thread->Resume()!=0)break;}
  }
  // A second snapshot covers children created just before a parent exits.
  // Audio/GPU guest callbacks have already been drained by the caller.
  if(!pending && ++quiet==2){
   std::vector<rex::kernel::object_ref<rex::kernel::XThread>> retired;
   {std::lock_guard lock(Stop().threads_mutex);retired.swap(Stop().started);}
   return true;
  }
  if(pending)quiet=0;
  std::this_thread::sleep_for(1ms);
 }while(deadline.remaining()>0ms);
 return false;
}
bool em2::TestGuestShutdown(){
 auto reset=[](){Stop().requested=false;Stop().event->Reset();};
 struct Reset{decltype(reset)& fn;~Reset(){fn();}} reset_at_exit{reset};
 auto check=[](bool value,const char* why){if(!value)REXLOG_ERROR("Guest shutdown regression: {}",why);return value;};
 reset();
 const auto configured=REXCVAR_GET(em2_cooperative_title_shutdown);
 const bool startup=CooperativeGuestShutdownEnabled();
 REXCVAR_SET(em2_cooperative_title_shutdown,!configured);
 const bool fixed_mode=CooperativeGuestShutdownEnabled()==startup;
 REXCVAR_SET(em2_cooperative_title_shutdown,configured);
 if(!check(fixed_mode,"changing the diagnostic flag cannot mix thread startup and shutdown modes"))return false;
 auto a=rex::thread::Event::CreateManualResetEvent(false),b=rex::thread::Event::CreateManualResetEvent(true);
 if(!a || !b)return false;
 {
  GuestExecutionScope guest(true);
  if(!check(GuestWait(a.get(),false,0ms)==Result::kTimeout && GuestWait(b.get(),false,0ms)==Result::kSuccess,"ordinary zero-time waits unchanged"))return false;
  auto any=GuestWaitAny({a.get(),b.get()},false,0ms);
  if(!check(any.first==Result::kSuccess && any.second==1 && GuestWaitAll({a.get(),b.get()},false,2ms)==Result::kTimeout,"any index and atomic all timeout"))return false;
 }
 {
  GuestExecutionScope guest(true);
  auto one=rex::thread::Semaphore::Create(1,4),zero=rex::thread::Semaphore::Create(0,4);
  if(!one || !zero)return false;
  if(!check(GuestWaitAll({one.get(),zero.get()},false,25ms)==Result::kTimeout &&
      GuestWait(one.get(),false,0ms)==Result::kSuccess && GuestWait(one.get(),false,0ms)==Result::kTimeout,
      "wait-all timeout polling never consumes only part of an atomic semaphore set"))return false;
  if(!check(GuestSignalAndWait(zero.get(),a.get(),false,25ms)==Result::kTimeout &&
      GuestWait(zero.get(),false,0ms)==Result::kSuccess && GuestWait(zero.get(),false,0ms)==Result::kTimeout,
      "signal-and-wait signals its semaphore exactly once across a split timeout"))return false;
 }
 for(unsigned mode=0;mode<6;++mode){
  reset();a->Reset();std::mutex held;std::atomic<bool> entered=false,unwound=false,cancelled=false;
  std::thread worker([&]{GuestExecutionScope guest(true);try{std::lock_guard lock(held);struct Unwind{std::atomic<bool>& flag;~Unwind(){flag=true;}} unwind{unwound};entered=true;
   switch(mode){case 0:GuestWait(a.get(),false,Duration::max());break;case 1:GuestWaitAny({a.get()},true,Duration::max());break;case 2:GuestWaitAll({a.get(),b.get()},false,Duration::max());break;case 3:GuestSignalAndWait(b.get(),a.get(),false,Duration::max());break;case 4:GuestSleep(60s);break;default:GuestAlertableSleep(60s);break;}
  }catch(const GuestShutdown&){cancelled=true;}});
  while(!entered)std::this_thread::yield();std::this_thread::sleep_for(20ms);RequestGuestShutdown();worker.join();
  if(!check(cancelled && unwound && held.try_lock(),"cancellation unwinds scopes and releases host locks"))return false;
  held.unlock();
  if(!check(rex::thread::Wait(a.get(),false,0ms)==Result::kTimeout,"cancellation does not signal the guest's wait object"))return false;
 }
 reset();
 {
  std::mutex held;std::atomic<bool> callback_started=false,callback_returned=false,outer_cancelled=false;
  auto ready=rex::thread::Event::CreateManualResetEvent(false);
  auto thread=rex::thread::Thread::Create({},[&]{GuestExecutionScope guest(true);ready->Set();try{GuestAlertableSleep(60s);}catch(const GuestShutdown&){outer_cancelled=true;}});
  if(!thread)return false;
  if(rex::thread::Wait(ready.get(),false,1s)!=Result::kSuccess){
   RequestGuestShutdown();rex::thread::Wait(thread.get(),false,Duration::max());return false;
  }
  thread->QueueUserCallback([&]{GuestApcBoundary([&]{std::lock_guard lock(held);callback_started=true;GuestWait(a.get(),false,Duration::max());});callback_returned=true;});
  const auto until=std::chrono::steady_clock::now()+1s;
  while(!callback_started && std::chrono::steady_clock::now()<until)std::this_thread::yield();
  const bool entered=callback_started;RequestGuestShutdown();
  const auto exited=rex::thread::Wait(thread.get(),false,2s);
  if(exited!=Result::kSuccess){
   REXLOG_ERROR("Guest shutdown APC regression did not exit within its deadline");
   // Retain callback captures until the owned worker really exits. A failing
   // diagnostic may remain live for its external test harness to inspect.
   rex::thread::Wait(thread.get(),false,Duration::max());return false;
  }
  if(!check(entered && callback_returned && outer_cancelled && held.try_lock(),"APC catches cancellation before returning to Windows, then outer guest unwinds"))return false;
  held.unlock();
 }
 REXLOG_INFO("Cooperative title shutdown compiled default: {}",rex::cvar::GetFlagInfo("em2_cooperative_title_shutdown")->default_value);
 reset();REXLOG_INFO("Guest shutdown regression passed: single/any/all/atomic signal-and-wait, sleep and alertable sleep, real waits, C++ unwind and host-lock release");return true;
}

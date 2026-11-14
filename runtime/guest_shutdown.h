#pragma once
#include <rex/thread.h>
#include <chrono>
#include <utility>
namespace rex::kernel {class KernelState;}
namespace em2 {
struct GuestShutdown {};
class GuestExecutionScope {
 public: explicit GuestExecutionScope(bool enabled); ~GuestExecutionScope();
 private: bool previous_;
};
bool CooperativeGuestShutdownEnabled();
void PollGuestShutdown();
bool GuestShutdownRequested();
void RequestGuestShutdown();
// Catch at the native APC boundary, before returning to the Windows dispatcher.
template<class Function> void GuestApcBoundary(Function&& function){
 if(GuestShutdownRequested())return;
 try{std::forward<Function>(function)();}catch(const GuestShutdown&){}
}

bool JoinGuestShutdown(rex::kernel::KernelState&,std::chrono::milliseconds);
rex::thread::WaitResult GuestWait(rex::thread::WaitHandle*,bool,std::chrono::milliseconds);
std::pair<rex::thread::WaitResult,size_t> GuestWaitAny(std::vector<rex::thread::WaitHandle*>,bool,std::chrono::milliseconds);
rex::thread::WaitResult GuestWaitAll(std::vector<rex::thread::WaitHandle*>,bool,std::chrono::milliseconds);
rex::thread::WaitResult GuestSignalAndWait(rex::thread::WaitHandle*,rex::thread::WaitHandle*,bool,std::chrono::milliseconds);
void GuestSleep(std::chrono::microseconds);
rex::thread::SleepResult GuestAlertableSleep(std::chrono::microseconds);
bool TestGuestShutdown();
}

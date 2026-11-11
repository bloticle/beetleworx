#include "guest_read_batch.h"
#include <rex/logging.h>
#include <thread>
bool em2::TestGuestReadBatch(){
 SYSTEM_INFO system{};GetSystemInfo(&system);const auto page=system.dwPageSize;
 auto* data=static_cast<uint8_t*>(VirtualAlloc(nullptr,3*page,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!data)return false;
 struct Release{void* p;~Release(){VirtualFree(p,0,MEM_RELEASE);}}release{data};DWORD old=0;
 if(!VirtualProtect(data+page,page,PAGE_NOACCESS,&old) || !VirtualProtect(data+2*page,page,PAGE_READWRITE|PAGE_GUARD,&old))return false;
 // Make the readable page resident; protection queries must not consume guards.
 *static_cast<volatile uint8_t*>(data)=7;
 const auto kinds_before=GuestReadBatch::QueryKinds();
 bool ok=true;const auto before=GuestReadBatch::Counts();
 {
  GuestReadBatch scope;
  ok&=GuestReadBatch::Readable(data,32);
  for(unsigned i=0;i<100;++i)ok&=GuestReadBatch::Readable(data+64+i,1);
  {GuestReadBatch nested;ok&=GuestReadBatch::Readable(data+32,32);}
  ok&=!GuestReadBatch::Readable(data+page-1,2);
  ok&=!GuestReadBatch::Readable(data+page,1);
  ok&=!GuestReadBatch::Readable(data+2*page,1);
  ok&=!GuestReadBatch::Readable(data,0);
  ok&=!GuestReadBatch::Readable(reinterpret_cast<void*>(UINTPTR_MAX-1),4);
 }
 const auto kinds_after=GuestReadBatch::QueryKinds();
 ok&=kinds_after[0]>kinds_before[0] && kinds_after[1]-kinds_before[1]==3;
 MEMORY_BASIC_INFORMATION guard{};ok&=VirtualQuery(data+2*page,&guard,sizeof(guard)) && (guard.Protect&PAGE_GUARD);
 const auto after=GuestReadBatch::Counts();ok&=after[0]-before[0]==4 && after[1]-before[1]==101;
 // Check both pages of a crossing span, including a readable protection
 // boundary. A guard query above must leave the guard armed.
 if(!VirtualProtect(data+page,page,PAGE_READWRITE,&old))return false;
 *static_cast<volatile uint8_t*>(data+page)=9;
 {GuestReadBatch scope;ok&=GuestReadBatch::Readable(data+page-4,8);}
 if(!VirtualProtect(data+page,page,PAGE_READONLY,&old))return false;
 {GuestReadBatch scope;ok&=!GuestReadBatch::Readable(data+page-4,8);ok&=GuestReadBatch::Readable(data+page,8);}
 // Changed mappings must never reuse a region from a preceding inspection.
 if(!VirtualProtect(data,page,PAGE_NOACCESS,&old))return false;
 {GuestReadBatch scope;ok&=!GuestReadBatch::Readable(data,1);}
 if(!VirtualProtect(data,page,PAGE_READWRITE,&old))return false;
 {GuestReadBatch scope;ok&=GuestReadBatch::Readable(data,1);}
 const auto threaded=GuestReadBatch::Counts();bool one=false,two=false;
 std::thread a([&]{GuestReadBatch scope;one=GuestReadBatch::Readable(data,1);});
 std::thread b([&]{GuestReadBatch scope;two=GuestReadBatch::Readable(data,1);});a.join();b.join();
 const auto done=GuestReadBatch::Counts();ok&=one && two && done[0]-threaded[0]==2;
 // A guest callback can change protection while an outer read is active.
 // Its own nested inspection and reads after it must both see fresh mappings.
 {
  GuestReadBatch outer;ok&=GuestReadBatch::Readable(data,1);
  {
   GuestReadBatch::CallbackBoundary callback;
   if(!VirtualProtect(data,page,PAGE_NOACCESS,&old))return false;
   {GuestReadBatch inside;ok&=!GuestReadBatch::Readable(data,1);}
  }
  ok&=!GuestReadBatch::Readable(data,1);
  {GuestReadBatch::CallbackBoundary callback;if(!VirtualProtect(data,page,PAGE_READWRITE,&old))return false;}
  ok&=GuestReadBatch::Readable(data,1);
 }
 REXLOG_INFO("Read-only inspection cache {}: real protection/guard boundaries, overflow, nested reuse, mapping changes between scopes independent threads and callback mapping changes",ok?"passed":"failed");return ok;
}

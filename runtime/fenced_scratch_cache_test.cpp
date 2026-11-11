#include "fenced_scratch_cache.h"
#include <rex/logging.h>
#include <vector>
namespace {
struct GpuUse {unsigned identity;uint64_t last_submission;};
struct Resource {unsigned identity;std::vector<unsigned>& destroyed;Resource(unsigned id,std::vector<unsigned>& out):identity(id),destroyed(out){}~Resource(){destroyed.push_back(identity);}};
}
bool em2::TestFencedScratchCache(){
 std::vector<unsigned> destroyed;std::vector<GpuUse> commands;uint64_t completed=0;
 FencedScratchCache<unsigned,Resource> cache(96);
 auto submit=[&](unsigned key,uint64_t bytes,uint64_t serial){
  auto value=cache.Find(key,serial);
  if(!value){if(!cache.MakeRoom(bytes,serial,completed))return false;value=std::make_shared<Resource>(key,destroyed);if(!cache.Insert(key,value,bytes,serial))return false;}
  commands.push_back({key,serial});return true;
 };
 auto intact=[&]{for(const auto& c:commands)if(c.last_submission>completed && std::find(destroyed.begin(),destroyed.end(),c.identity)!=destroyed.end())return false;return cache.used()<=96;};
 if(!submit(1,32,1) || !submit(2,64,1) || submit(3,64,2) || !destroyed.empty() || !intact())return false;
 // A high completed fence must not retire the current/open submission.
 completed=1;if(cache.MakeRoom(64,1,100) || !destroyed.empty())return false;
 auto borrowed=cache.Find(2,1);
 if(cache.MakeRoom(64,2,completed) || !destroyed.empty() || !intact())return false;
 borrowed.reset();if(!submit(3,64,2) || destroyed!=std::vector<unsigned>({1,2}) || !intact())return false;
 if(!submit(4,32,3) || submit(5,64,3) || !intact())return false;
 completed=2;if(!submit(5,64,4) || !intact() || destroyed!=std::vector<unsigned>({1,2,3}))return false;
 // Unknown submissions retain their resources rather than guessing at safety.
 FencedScratchCache<unsigned,Resource> unknown(32);auto pinned=std::make_shared<Resource>(9,destroyed);if(!unknown.Insert(9,pinned,32,1))return false;pinned.reset();
 if(!unknown.Find(9,0) || !unknown.Find(9,100))return false;
 if(unknown.MakeRoom(32,100,100) || unknown.MakeRoom(UINT64_MAX,100,100) || unknown.MakeRoom(0,100,100))return false;
 completed=4; // Shutdown, unlike eviction, requires the producer queue drained.
 if(!intact())return false;
 REXLOG_INFO("Fenced scratch cache: queued uses, current submission, retained handles, completed-only eviction, budget and unknown-fence protection passed");return true;
}

#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
namespace em2 {
// Current/open submissions and borrowed resources are never eviction candidates.
// A zero submission denotes a caller outside native submission tracking (tests).
template<class Key,class Resource> class FencedScratchCache {
 struct Entry {std::shared_ptr<Resource> resource;uint64_t bytes,submission,touched;};
 std::map<Key,Entry> entries_;
 uint64_t budget_,used_=0,clock_=0;
public:
 explicit FencedScratchCache(uint64_t budget):budget_(budget){}
 std::shared_ptr<Resource> Find(const Key& key,uint64_t submission){
  auto it=entries_.find(key);if(it==entries_.end())return {};
  it->second.submission=it->second.submission && submission?std::max(it->second.submission,submission):0;
  it->second.touched=++clock_;return it->second.resource;
 }
 bool MakeRoom(uint64_t bytes,uint64_t current,uint64_t completed){
  if(!bytes || bytes>budget_)return false;
  // Do not churn completed entries if in-flight resources still prevent the
  // request from fitting. Failed admission leaves the existing cache intact.
  if(used_>budget_-bytes){
   uint64_t reclaimable=0;
   for(const auto& pair:entries_){const auto& e=pair.second;
    if(e.submission && e.submission<current && e.submission<=completed && e.resource.use_count()==1)reclaimable+=e.bytes;
   }
   if(reclaimable<used_-(budget_-bytes))return false;
  }
  while(used_>budget_-bytes){
   auto victim=entries_.end();
   for(auto it=entries_.begin();it!=entries_.end();++it){
    const auto& e=it->second;
    if(!e.submission || e.submission>=current || e.submission>completed || e.resource.use_count()!=1)continue;
    if(victim==entries_.end() || e.touched<victim->second.touched)victim=it;
   }
   if(victim==entries_.end())return false;
   used_-=victim->second.bytes;entries_.erase(victim);
  }
  return true;
 }
 bool Insert(const Key& key,std::shared_ptr<Resource> resource,uint64_t bytes,uint64_t submission){
  if(!resource || !bytes || bytes>budget_ || used_>budget_-bytes || entries_.contains(key))return false;
  entries_.emplace(key,Entry{std::move(resource),bytes,submission,++clock_});used_+=bytes;return true;
 }
 uint64_t used()const{return used_;}
};
bool TestFencedScratchCache();
}

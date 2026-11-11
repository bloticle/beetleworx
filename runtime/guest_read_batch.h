#pragma once
#include <windows.h>
#include <psapi.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
namespace em2 {
// Only use around a bounded, read-only inspection. No guest allocation,
// protection changes, callbacks that mutate mappings, or retained pointers.
// A fresh inspection starts with an empty cache; nested inspections reuse it.
class GuestReadBatch {
    struct Region {uintptr_t begin=0,end=0;};
    inline static thread_local GuestReadBatch* active_=nullptr;
    inline static std::atomic<uint64_t> queries_{0},hits_{0},resident_{0},fallbacks_{0};
    GuestReadBatch* previous_=active_;
    std::array<Region,16> regions_{};
    unsigned next_=0;
    uint64_t queries=0,hits=0,resident=0,fallbacks=0;
public:
    GuestReadBatch(){if(!active_)active_=this;}
    ~GuestReadBatch(){if(!previous_){active_=nullptr;queries_.fetch_add(queries,std::memory_order_relaxed);hits_.fetch_add(hits,std::memory_order_relaxed);resident_.fetch_add(resident,std::memory_order_relaxed);fallbacks_.fetch_add(fallbacks,std::memory_order_relaxed);}}
    GuestReadBatch(const GuestReadBatch&)=delete;
    GuestReadBatch& operator=(const GuestReadBatch&)=delete;
    static bool Readable(const void* pointer,size_t size){
        const auto address=reinterpret_cast<uintptr_t>(pointer);
        if(!size || size>std::numeric_limits<uintptr_t>::max()-address)return false;
        const auto end=address+size;
        if(active_)for(const auto& region:active_->regions_)if(address>=region.begin && end<=region.end){++active_->hits;return true;}
        if(active_)++active_->queries;
        // AMD64 Windows base pages are 4 KiB. Inspect every page of a small
        // user-mode span without walking an enormous guest-memory region.
        // Cold/guard pages, mixed protections, failed probes and larger spans
        // retain VirtualQuery. Never fault a page in merely to inspect it.
        constexpr uintptr_t page_mask=4095;
        const auto first=address&~page_mask,last=(end-1)&~page_mask;
        const auto count=(last-first)/4096+1;
        if(address>=65536 && end<=0x00007FFFFFFF0000ull && count<=16){
            std::array<PSAPI_WORKING_SET_EX_INFORMATION,16> pages{};
            for(size_t i=0;i<count;++i)pages[i].VirtualAddress=reinterpret_cast<void*>(first+i*4096);
            const auto error=GetLastError();
            bool found=K32QueryWorkingSetEx(GetCurrentProcess(),pages.data(),DWORD(count*sizeof(pages[0])))!=0;
            SetLastError(error);
            const auto protection=pages[0].VirtualAttributes.Win32Protection;
            for(size_t i=0;i<count && found;++i){
                const auto& page=pages[i].VirtualAttributes;
                found=page.Valid && !page.Bad && protection && page.Win32Protection==protection && !(protection&(PAGE_NOACCESS|PAGE_GUARD));
            }
            if(found){
                if(active_){++active_->resident;active_->regions_[active_->next_]={first,last+4096};active_->next_=(active_->next_+1)%active_->regions_.size();}
                return true;
            }
        }
        if(active_)++active_->fallbacks;
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(pointer,&info,sizeof(info)) || info.State!=MEM_COMMIT || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
        const auto begin=reinterpret_cast<uintptr_t>(info.BaseAddress),region_end=begin+info.RegionSize;
        if(end>region_end)return false;
        if(active_){active_->regions_[active_->next_]={begin,region_end};active_->next_=(active_->next_+1)%active_->regions_.size();}
        return true;
    }
    // Use around callbacks that may allocate, free or protect guest memory.
    // Neither the callback nor subsequent reads may inherit cached mappings.
    class CallbackBoundary {
        GuestReadBatch* saved_=active_;
    public:
        CallbackBoundary(){if(saved_){saved_->regions_.fill({});saved_->next_=0;}active_=nullptr;}
        ~CallbackBoundary(){if(saved_){saved_->regions_.fill({});saved_->next_=0;}active_=saved_;}
        CallbackBoundary(const CallbackBoundary&)=delete;
        CallbackBoundary& operator=(const CallbackBoundary&)=delete;
    };
    static std::array<uint64_t,2> QueryKinds(){return {resident_.load(std::memory_order_relaxed),fallbacks_.load(std::memory_order_relaxed)};}
    static std::array<uint64_t,2> Counts(){return {queries_.load(std::memory_order_relaxed),hits_.load(std::memory_order_relaxed)};}
};
bool TestGuestReadBatch();
}

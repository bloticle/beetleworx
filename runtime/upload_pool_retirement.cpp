#include "upload_pool_retirement.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/ui/graphics_upload_buffer_pool.h>

REXCVAR_DEFINE_BOOL(em2_trim_shared_upload_cache,false,"Debug","Retire old completed shared-memory upload pages beyond an 8 MiB spare cache; private measurement candidate");
extern "C" void EM2OriginalSharedUploadCompletion(rex::graphics::d3d12::D3D12SharedMemory*);
namespace {
using Pool=rex::ui::GraphicsUploadBufferPool;
template<class Tag,auto Member>struct Access {friend auto MemberOf(Tag){return Member;}};
struct First{friend auto MemberOf(First);};template struct Access<First,&Pool::writable_first_>;
struct Last{friend auto MemberOf(Last);};template struct Access<Last,&Pool::writable_last_>;
struct Size{friend auto MemberOf(Size);};template struct Access<Size,&Pool::page_size_>;

// Called on the pool's owning GPU worker, after native Reclaim. The first
// writable page may still have in-flight writes and partially used space.
// Preserve it and its counters unconditionally. Only old, fence-completed
// spare pages can be destroyed; submitted pages are never inspected/changed.
uint64_t Trim(Pool& pool,uint64_t completed,uint64_t spare_bytes,uint64_t grace) {
    auto* previous=pool.*MemberOf(First{});
    const auto page_size=pool.*MemberOf(Size{});
    if(!previous || !page_size)return 0;
    uint64_t retained=0,released=0;
    for(auto* page=previous->next_;page;) {
        auto* next=page->next_;
        const bool old=page->last_submission_index_<=completed && completed-page->last_submission_index_>=grace;
        if(retained>=spare_bytes && old) {
            previous->next_=next;
            if(pool.*MemberOf(Last{})==page)pool.*MemberOf(Last{})=previous;
            delete page;
            released+=page_size;
        }else {retained+=page_size;previous=page;}
        page=next;
    }
    return released;
}

class Fixture final:public Pool {
public:
    int created=0,destroyed=0,flushes=0;
    Fixture():Pool(64){}
    ~Fixture(){ClearCache();}
    struct TestPage final:Page {
        Fixture& owner;
        explicit TestPage(Fixture& value):owner(value){}
        ~TestPage() override{++owner.destroyed;}
    };
    Page* CreatePageImplementation() override{++created;return new TestPage(*this);}
    void FlushPageWrites(Page*,size_t,size_t) override{++flushes;}
    Page* Write(uint64_t submission,size_t bytes,size_t& offset){return Request(submission,bytes,1,offset);}
    size_t Pending()const{size_t n=0;for(auto* p=submitted_first_;p;p=p->next_)++n;return n;}
    size_t Writable()const{size_t n=0;for(auto* p=writable_first_;p;p=p->next_)++n;return n;}
    bool TailValid()const{auto* p=writable_first_;if(!p)return !writable_last_;while(p->next_)p=p->next_;return p==writable_last_;}
    size_t Used()const{return current_page_used_;}
    size_t Flushed()const{return current_page_flushed_;}
};
}

void rex::graphics::d3d12::D3D12SharedMemory::CompletedSubmissionUpdated() {
    EM2OriginalSharedUploadCompletion(this);
    if(REXCVAR_GET(em2_trim_shared_upload_cache) && upload_buffer_pool_)
        Trim(*upload_buffer_pool_,command_processor_.GetCompletedSubmission(),8ull<<20,120);
}

bool em2::TestUploadPoolRetirement() {
    auto check=[](bool value,const char* reason){if(!value)REXLOG_ERROR("Upload cache retirement regression: {}",reason);return value;};
    Fixture pool;size_t offset=0;
    if(!check(Trim(pool,100,128,120)==0,"empty pool"))return false;
    for(uint64_t i=1;i<=12;++i)if(!pool.Write(i,64,offset))return false;
    pool.Reclaim(6);
    if(!check(pool.Writable()==7 && pool.Pending()==5,"native partial fence reclaim"))return false;
    if(!check(Trim(pool,6,128,120)==0 && pool.destroyed==0,"young spare pages retained"))return false;
    // Even with no grace, submitted pages and the active page must survive.
    if(!check(Trim(pool,6,128,0)==256 && pool.destroyed==4 && pool.Pending()==5 && pool.Writable()==3 && pool.TailValid(),"only completed excess spares retired"))return false;
    auto* active=pool.Write(12,0,offset);const auto used=pool.Used(),flushed=pool.Flushed();
    pool.Reclaim(12);
    if(!check(Trim(pool,129,128,120)==192 && pool.Writable()==5,"retirement age boundary"))return false;
    if(!check(Trim(pool,132,128,120)==128 && pool.Writable()==3 && pool.Pending()==0 && pool.TailValid(),"completed tail retirement"))return false;
    if(!check(pool.Used()==used && pool.Flushed()==flushed && pool.Write(12,0,offset)==active,"active page and partial-write counters preserved"))return false;
    const auto created=pool.created;
    if(!check(pool.Write(133,64,offset)!=nullptr && pool.created==created && offset==0 && pool.TailValid(),"native request reuses retained spare"))return false;
    pool.ClearCache();
    if(!check(pool.destroyed==pool.created && !pool.Pending() && !pool.Writable() && pool.TailValid(),"native shutdown frees each page once"))return false;
    // A partially written current page must remain appendable after trimming.
    auto* partial=pool.Write(200,16,offset);pool.FlushWrites();
    if(!check(Trim(pool,500,0,0)==0 && pool.Write(201,16,offset)==partial && offset==16 && pool.Flushed()==16,"partial active page remains appendable"))return false;
    pool.ClearCache();
    if(!check(pool.destroyed==pool.created,"partial-page cleanup"))return false;
    REXLOG_INFO("Upload cache retirement tests passed: native requests/reclaim, incomplete fences, age boundary, spare reuse, partial writes, tail links and exact destruction");
    return true;
}

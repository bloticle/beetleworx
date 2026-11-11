#include "guest_read_batch.h"
#include "online_selector.h"
#include "online.h"
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_trace_online_observations);
#include <windows.h>
#include <immintrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <set>
#include <unordered_set>
#include <vector>
PPC_EXTERN_FUNC(__imp__sub_829505C0);
PPC_EXTERN_FUNC(__restfpr_25);
PPC_EXTERN_FUNC(__restgprlr_23);
PPC_EXTERN_FUNC(__savefpr_25);
PPC_EXTERN_FUNC(__savegprlr_23);
PPC_EXTERN_FUNC(sub_82919320);
PPC_EXTERN_FUNC(sub_8294FC28);
PPC_EXTERN_FUNC(sub_82B6DDC8);
PPC_EXTERN_FUNC(sub_831C14E0);
PPC_EXTERN_FUNC(sub_831C1558);

namespace {
struct HostState{DWORD error=GetLastError();unsigned csr=_mm_getcsr();~HostState(){SetLastError(error);_mm_setcsr(csr);}};
struct Reader {
    uint8_t* base;
    bool Words(uint32_t a,uint32_t* out,size_t count){
        const auto size=count*4;if(a<4096 || uint64_t(a)+size>0x100000000ull)return false;
        if(!em2::GuestReadBatch::Readable(PPC_RAW_ADDR(a),size))return false;
        for(size_t i=0;i<count;++i)out[i]=PPC_LOAD_U32(a+uint32_t(i*4));return true;
    }
    bool Word(uint32_t a,uint32_t& out){return Words(a,&out,1);}
};
struct Targets {
    using Id=std::array<uint32_t,4>;
    struct Entry{Id id;uint32_t node,key,bucket;};
    uint32_t map=0,bucket_count=0;size_t cursor=0;bool reorder=false;std::vector<Entry> entries;
    bool Build(Reader& r,uint32_t selector){
        em2::GuestReadBatch inspection;
        entries.clear();cursor=0;reorder=false;map=selector+52;uint32_t table[2];if(!r.Words(map+16,table,2) || !table[1] || table[1]>65536)return false;bucket_count=table[1];
        std::unordered_set<uint32_t> seen;std::set<Id> ids;
        for(uint32_t bucket=0;bucket<bucket_count;++bucket){uint32_t node;if(!r.Word(table[0]+bucket*4,node))return false;
            while(node){if(seen.size()>=4096 || !seen.insert(node).second)return false;uint32_t n[6],attached=0;if(!r.Words(node,n,6) || !n[0])return false;
                uint32_t entity[5];
                // Native selector itself resolves BEntity as *(target+60)+8.
                // 825184B8 initializes its complete 16-byte ID at interface+4.
                if(!r.Word(n[0]+60,attached) || !attached || !r.Words(attached+8,entity,5) || entity[0]!=0x8208D908)return false;
                const Id id={entity[1],entity[2],entity[3],entity[4]};
                if(std::all_of(id.begin(),id.end(),[](auto v){return v==0;}) || !ids.insert(id).second)return false;
                entries.push_back({id,node,n[0],bucket});node=n[5];}
        }
        reorder=!std::is_sorted(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.id<b.id;});return true;
    }
    bool Locate(Reader& r,const Entry& e,uint32_t& bucket,uint32_t& previous){
        em2::GuestReadBatch inspection;
        uint32_t table[2];if(!r.Words(map+16,table,2) || !table[1] || table[1]>65536)return false;
        const bool rehashed=table[1]!=bucket_count;size_t visited=0;
        for(uint32_t b=rehashed?0:e.bucket;b<(rehashed?table[1]:e.bucket+1);++b){
            if(b>=table[1])return false;uint32_t node;if(!r.Word(table[0]+b*4,node))return false;previous=0;
            while(node && visited++<4096){uint32_t n[6];if(!r.Words(node,n,6))return false;
                if(node==e.node && n[0]==e.key){bucket=b;return true;}previous=node;node=n[5];}
        }return false;
    }
    void Store(PPCContext& ctx,uint8_t* base,uint32_t iterator){
        HostState host;Reader r{base};uint32_t bucket=0,previous=0,node=0;
        for(;cursor<entries.size();++cursor)if(Locate(r,entries[cursor],bucket,previous)){node=entries[cursor].node;break;}
        if(!node){uint32_t count=0;r.Word(map+20,count);bucket=count;previous=0;}
        PPC_STORE_U32(iterator,map);PPC_STORE_U32(iterator+4,bucket);PPC_STORE_U32(iterator+8,node);PPC_STORE_U32(iterator+12,previous);
        ctx.r28.u64=map;ctx.r31.u64=bucket;ctx.r30.u64=node;ctx.r10.u64=previous;
    }
    void First(PPCContext& ctx,uint8_t* base,uint32_t iterator){cursor=0;Store(ctx,base,iterator);}
    bool Next(PPCContext& ctx,uint8_t* base,uint32_t iterator){++cursor;Store(ctx,base,iterator);return true;}
};
// Retail 829505C0 body. Only begin/advance select the stable work list; native
// tests, RNG, callbacks, timestamp writes and budget accounting remain intact.
void CanonicalBody(PPCContext& ctx,uint8_t* base,Targets& targets) {
	PPC_FUNC_PROLOGUE();
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	ctx.r12.u64 = ctx.lr;
	// bl 0x82b69554
	ctx.lr = 0x829505C8;
	__savegprlr_23(ctx, base);
	// addi r12,r1,-80
	ctx.r12.s64 = ctx.r1.s64 + -80;
	// bl 0x82b69fac
	ctx.lr = 0x829505D0;
	__savefpr_25(ctx, base);
	// stwu r1,-256(r1)
	ea = -256 + ctx.r1.u32;
	PPC_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	// lfs f0,0(r4)
	ctx.fpscr.disableFlushMode();
	temp.u32 = PPC_LOAD_U32(ctx.r4.u32 + 0);
	ctx.f0.f64 = double(temp.f32);
	// mr r29,r3
	ctx.r29.u64 = ctx.r3.u64;
	// lfs f13,48(r3)
	temp.u32 = PPC_LOAD_U32(ctx.r3.u32 + 48);
	ctx.f13.f64 = double(temp.f32);
	// mr r23,r4
	ctx.r23.u64 = ctx.r4.u64;
	// fcmpu cr6,f0,f13
	ctx.cr6.compare(ctx.f0.f64, ctx.f13.f64);
	// ble cr6,0x829505f4
	if (!ctx.cr6.gt) goto loc_829505F4;
	// fmr f25,f13
	ctx.f25.f64 = ctx.f13.f64;
	// b 0x829505f8
	goto loc_829505F8;
loc_829505F4:
	// fmr f25,f0
	ctx.fpscr.disableFlushMode();
	ctx.f25.f64 = ctx.f0.f64;
loc_829505F8:
	// lis r11,-32256
	ctx.r11.s64 = -2113929216;
	// lfs f28,8288(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = PPC_LOAD_U32(ctx.r11.u32 + 8288);
	ctx.f28.f64 = double(temp.f32);
	// fcmpu cr6,f25,f28
	ctx.cr6.compare(ctx.f25.f64, ctx.f28.f64);
	// ble cr6,0x82950810
	if (!ctx.cr6.gt) goto loc_82950810;
	// addi r26,r29,88
	ctx.r26.s64 = ctx.r29.s64 + 88;
	// fmr f30,f28
	ctx.f30.f64 = ctx.f28.f64;
	// fmr f1,f28
	ctx.f1.f64 = ctx.f28.f64;
	// mr r3,r26
	ctx.r3.u64 = ctx.r26.u64;
	// bl 0x831c14e0
	ctx.lr = 0x8295061C;
	sub_831C14E0(ctx, base);
	// lis r11,-31936
	ctx.r11.s64 = -2092957696;
	// addi r25,r29,52
	ctx.r25.s64 = ctx.r29.s64 + 52;
	// addi r3,r1,96
	ctx.r3.s64 = ctx.r1.s64 + 96;
	// mr r4,r25
	ctx.r4.u64 = ctx.r25.u64;
	// li r24,1
	ctx.r24.s64 = 1;
	// lfs f31,-13748(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = PPC_LOAD_U32(ctx.r11.u32 + -13748);
	ctx.f31.f64 = double(temp.f32);
	// bl 0x82919320
	ctx.lr = 0x82950638;
	sub_82919320(ctx, base);
	targets.First(ctx,base,ctx.r1.u32+96);
	// lis r11,-32225
	ctx.r11.s64 = -2111897600;
	// lwz r10,108(r1)
	ctx.r10.u64 = PPC_LOAD_U32(ctx.r1.u32 + 108);
	// lis r9,-32256
	ctx.r9.s64 = -2113929216;
	// lis r8,-32251
	ctx.r8.s64 = -2113601536;
	// lis r27,-31938
	ctx.r27.s64 = -2093088768;
	// lfs f26,20768(r11)
	ctx.fpscr.disableFlushMode();
	temp.u32 = PPC_LOAD_U32(ctx.r11.u32 + 20768);
	ctx.f26.f64 = double(temp.f32);
	// lfs f29,16200(r9)
	temp.u32 = PPC_LOAD_U32(ctx.r9.u32 + 16200);
	ctx.f29.f64 = double(temp.f32);
	// lfs f27,24088(r8)
	temp.u32 = PPC_LOAD_U32(ctx.r8.u32 + 24088);
	ctx.f27.f64 = double(temp.f32);
	// lwz r28,96(r1)
	ctx.r28.u64 = PPC_LOAD_U32(ctx.r1.u32 + 96);
	// lwz r31,100(r1)
	ctx.r31.u64 = PPC_LOAD_U32(ctx.r1.u32 + 100);
	// lwz r30,104(r1)
	ctx.r30.u64 = PPC_LOAD_U32(ctx.r1.u32 + 104);
loc_82950664:
	// lwz r11,20(r25)
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r25.u32 + 20);
	// cmplw cr6,r28,r25
	ctx.cr6.compare<uint32_t>(ctx.r28.u32, ctx.r25.u32, ctx.xer);
	// bne cr6,0x8295068c
	if (!ctx.cr6.eq) goto loc_8295068C;
	// cmplw cr6,r31,r11
	ctx.cr6.compare<uint32_t>(ctx.r31.u32, ctx.r11.u32, ctx.xer);
	// bne cr6,0x8295068c
	if (!ctx.cr6.eq) goto loc_8295068C;
	// cmplwi cr6,r30,0
	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
	// bne cr6,0x8295068c
	if (!ctx.cr6.eq) goto loc_8295068C;
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// li r11,1
	ctx.r11.s64 = 1;
	// beq cr6,0x82950690
	if (ctx.cr6.eq) goto loc_82950690;
loc_8295068C:
	// li r11,0
	ctx.r11.s64 = 0;
loc_82950690:
	// clrlwi r11,r11,24
	ctx.r11.u64 = ctx.r11.u32 & 0xFF;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// bne cr6,0x82950804
	if (!ctx.cr6.eq) goto loc_82950804;
	// fcmpu cr6,f25,f30
	ctx.fpscr.disableFlushMode();
	ctx.cr6.compare(ctx.f25.f64, ctx.f30.f64);
	// ble cr6,0x82950804
	if (!ctx.cr6.gt) goto loc_82950804;
	// lfs f0,44(r29)
	temp.u32 = PPC_LOAD_U32(ctx.r29.u32 + 44);
	ctx.f0.f64 = double(temp.f32);
	// fcmpu cr6,f0,f28
	ctx.cr6.compare(ctx.f0.f64, ctx.f28.f64);
	// ble cr6,0x829507b4
	if (!ctx.cr6.gt) goto loc_829507B4;
	// lwz r11,12(r30)
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r30.u32 + 12);
	// cmpwi cr6,r11,0
	ctx.cr6.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
	// bne cr6,0x829507b4
	if (!ctx.cr6.eq) goto loc_829507B4;
	// bl 0x82b6ddc8
	ctx.lr = 0x829506C0;
	sub_82B6DDC8(ctx, base);
	// extsw r11,r3
	ctx.r11.s64 = ctx.r3.s32;
	// lfs f0,8(r30)
	ctx.fpscr.disableFlushMode();
	temp.u32 = PPC_LOAD_U32(ctx.r30.u32 + 8);
	ctx.f0.f64 = double(temp.f32);
	// std r11,80(r1)
	PPC_STORE_U64(ctx.r1.u32 + 80, ctx.r11.u64);
	// lfd f13,80(r1)
	ctx.f13.u64 = PPC_LOAD_U64(ctx.r1.u32 + 80);
	// fcfid f12,f13
	ctx.f12.f64 = double(ctx.f13.s64);
	// lfs f11,44(r29)
	temp.u32 = PPC_LOAD_U32(ctx.r29.u32 + 44);
	ctx.f11.f64 = double(temp.f32);
	// frsp f10,f12
	ctx.f10.f64 = double(float(ctx.f12.f64));
	// fsubs f9,f31,f0
	ctx.f9.f64 = double(float(ctx.f31.f64 - ctx.f0.f64));
	// fmsubs f8,f10,f27,f29
	ctx.f8.f64 = double(float(ctx.f10.f64 * ctx.f27.f64 - ctx.f29.f64));
	// fmadds f7,f8,f26,f29
	ctx.f7.f64 = double(float(ctx.f8.f64 * ctx.f26.f64 + ctx.f29.f64));
	// fmuls f6,f7,f11
	ctx.f6.f64 = double(float(ctx.f7.f64 * ctx.f11.f64));
	// fcmpu cr6,f9,f6
	ctx.cr6.compare(ctx.f9.f64, ctx.f6.f64);
	// ble cr6,0x829507b4
	if (!ctx.cr6.gt) goto loc_829507B4;
	// lwz r11,0(r30)
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r30.u32 + 0);
	// lwz r11,60(r11)
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r11.u32 + 60);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82950764
	if (ctx.cr6.eq) goto loc_82950764;
	// addi r9,r11,8
	ctx.r9.s64 = ctx.r11.s64 + 8;
	// cmplwi cr6,r9,0
	ctx.cr6.compare<uint32_t>(ctx.r9.u32, 0, ctx.xer);
	// beq cr6,0x82950764
	if (ctx.cr6.eq) goto loc_82950764;
	// lwz r10,88(r9)
	ctx.r10.u64 = PPC_LOAD_U32(ctx.r9.u32 + 88);
	// cmplwi cr6,r10,0
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	// beq cr6,0x82950728
	if (ctx.cr6.eq) goto loc_82950728;
	// lbz r11,-16123(r27)
	ctx.r11.u64 = PPC_LOAD_U8(ctx.r27.u32 + -16123);
	// lbzx r11,r11,r10
	ctx.r11.u64 = PPC_LOAD_U8(ctx.r11.u32 + ctx.r10.u32);
	// b 0x8295072c
	goto loc_8295072C;
loc_82950728:
	// li r11,255
	ctx.r11.s64 = 255;
loc_8295072C:
	// clrlwi r11,r11,24
	ctx.r11.u64 = ctx.r11.u32 & 0xFF;
	// cmplwi cr6,r11,255
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 255, ctx.xer);
	// beq cr6,0x82950764
	if (ctx.cr6.eq) goto loc_82950764;
	// lwz r10,36(r9)
	ctx.r10.u64 = PPC_LOAD_U32(ctx.r9.u32 + 36);
	// rlwinm r9,r11,2,0,29
	ctx.r9.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 2) & 0xFFFFFFFC;
	// lwzx r11,r9,r10
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r9.u32 + ctx.r10.u32);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82950764
	if (ctx.cr6.eq) goto loc_82950764;
	// addi r11,r11,-8
	ctx.r11.s64 = ctx.r11.s64 + -8;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82950764
	if (ctx.cr6.eq) goto loc_82950764;
	// lwz r11,108(r11)
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r11.u32 + 108);
	// cmpwi cr6,r11,2
	ctx.cr6.compare<int32_t>(ctx.r11.s32, 2, ctx.xer);
	// beq cr6,0x829507b4
	if (ctx.cr6.eq) goto loc_829507B4;
loc_82950764:
	// clrlwi r11,r24,24
	ctx.r11.u64 = ctx.r24.u32 & 0xFF;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x82950788
	if (ctx.cr6.eq) goto loc_82950788;
	// lwz r11,0(r29)
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r29.u32 + 0);
	// mr r3,r29
	ctx.r3.u64 = ctx.r29.u64;
	// lwz r10,32(r11)
	ctx.r10.u64 = PPC_LOAD_U32(ctx.r11.u32 + 32);
	// mtctr r10
	ctx.ctr.u64 = ctx.r10.u64;
	// bctrl 
	ctx.lr = 0x82950784;
	PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// li r24,0
	ctx.r24.s64 = 0;
loc_82950788:
	// mr r3,r29
	ctx.r3.u64 = ctx.r29.u64;
	// ld r4,96(r1)
	ctx.r4.u64 = PPC_LOAD_U64(ctx.r1.u32 + 96);
	// ld r5,104(r1)
	ctx.r5.u64 = PPC_LOAD_U64(ctx.r1.u32 + 104);
	// fmr f1,f31
	ctx.fpscr.disableFlushMode();
	ctx.f1.f64 = ctx.f31.f64;
	// bl 0x8294fc28
	ctx.lr = 0x8295079C;
	sub_8294FC28(ctx, base);
	// mr r3,r26
	ctx.r3.u64 = ctx.r26.u64;
	// bl 0x831c1558
	ctx.lr = 0x829507A4;
	sub_831C1558(ctx, base);
	// mr r3,r26
	ctx.r3.u64 = ctx.r26.u64;
	// fadds f30,f1,f30
	ctx.fpscr.disableFlushMode();
	ctx.f30.f64 = double(float(ctx.f1.f64 + ctx.f30.f64));
	// fmr f1,f28
	ctx.f1.f64 = ctx.f28.f64;
	// bl 0x831c14e0
	ctx.lr = 0x829507B4;
	sub_831C14E0(ctx, base);
loc_829507B4:
	if(targets.Next(ctx,base,ctx.r1.u32+96)) goto loc_82950664;
	// mr r10,r30
	ctx.r10.u64 = ctx.r30.u64;
	// lwz r30,20(r30)
	ctx.r30.u64 = PPC_LOAD_U32(ctx.r30.u32 + 20);
	// stw r10,108(r1)
	PPC_STORE_U32(ctx.r1.u32 + 108, ctx.r10.u32);
	// cmplwi cr6,r30,0
	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
	// stw r30,104(r1)
	PPC_STORE_U32(ctx.r1.u32 + 104, ctx.r30.u32);
	// bne cr6,0x82950664
	if (!ctx.cr6.eq) goto loc_82950664;
	// lwz r11,20(r28)
	ctx.r11.u64 = PPC_LOAD_U32(ctx.r28.u32 + 20);
loc_829507D0:
	// addi r31,r31,1
	ctx.r31.s64 = ctx.r31.s64 + 1;
	// li r10,0
	ctx.r10.s64 = 0;
	// stw r31,100(r1)
	PPC_STORE_U32(ctx.r1.u32 + 100, ctx.r31.u32);
	// cmplw cr6,r31,r11
	ctx.cr6.compare<uint32_t>(ctx.r31.u32, ctx.r11.u32, ctx.xer);
	// stw r10,108(r1)
	PPC_STORE_U32(ctx.r1.u32 + 108, ctx.r10.u32);
	// bge cr6,0x82950664
	if (!ctx.cr6.lt) goto loc_82950664;
	// lwz r9,16(r28)
	ctx.r9.u64 = PPC_LOAD_U32(ctx.r28.u32 + 16);
	// rlwinm r8,r31,2,0,29
	ctx.r8.u64 = __builtin_rotateleft64(ctx.r31.u32 | (ctx.r31.u64 << 32), 2) & 0xFFFFFFFC;
	// lwzx r30,r8,r9
	ctx.r30.u64 = PPC_LOAD_U32(ctx.r8.u32 + ctx.r9.u32);
	// cmplwi cr6,r30,0
	ctx.cr6.compare<uint32_t>(ctx.r30.u32, 0, ctx.xer);
	// stw r30,104(r1)
	PPC_STORE_U32(ctx.r1.u32 + 104, ctx.r30.u32);
	// beq cr6,0x829507d0
	if (ctx.cr6.eq) goto loc_829507D0;
	// b 0x82950664
	goto loc_82950664;
loc_82950804:
	// lfs f0,0(r23)
	ctx.fpscr.disableFlushMode();
	temp.u32 = PPC_LOAD_U32(ctx.r23.u32 + 0);
	ctx.f0.f64 = double(temp.f32);
	// fsubs f13,f0,f30
	ctx.f13.f64 = double(float(ctx.f0.f64 - ctx.f30.f64));
	// stfs f13,0(r23)
	temp.f32 = float(ctx.f13.f64);
	PPC_STORE_U32(ctx.r23.u32 + 0, temp.u32);
loc_82950810:
	// addi r1,r1,256
	ctx.r1.s64 = ctx.r1.s64 + 256;
	// addi r12,r1,-80
	ctx.r12.s64 = ctx.r1.s64 + -80;
	// bl 0x82b69ff8
	ctx.lr = 0x8295081C;
	__restfpr_25(ctx, base);
	// b 0x82b695a4
	__restgprlr_23(ctx, base);
	return;
}


}
void em2::net::RunCanonicalPerception(PPCContext& ctx,uint8_t* base){
    Targets targets;bool valid=false,has_work=true;
    {
        HostState host;Reader reader{base};uint32_t budget_bits=0,limit_bits=0;
        // Preserve the retail zero/negative/NaN budget fast path without a
        // registry snapshot or sort. The original still owns all side effects.
        if(reader.Word(ctx.r4.u32,budget_bits) && reader.Word(ctx.r3.u32+48,limit_bits)){
            const auto budget=std::bit_cast<float>(budget_bits),limit=std::bit_cast<float>(limit_bits);
            has_work=(budget>limit?limit:budget)>0;
        }
        if(has_work)valid=targets.Build(reader,ctx.r3.u32);
    }
    if(!has_work){__imp__sub_829505C0(ctx,base);return;}
    if(REXCVAR_GET(em2_trace_online_observations) && GameplayInputGate().committed_tick()<4){
        HostState host;std::string ids;for(size_t i=0;i<std::min<size_t>(targets.entries.size(),64);++i){const auto& id=targets.entries[i].id;ids+=fmt::format("{:08X}{:08X}{:08X}{:08X} ",id[0],id[1],id[2],id[3]);}
        REXLOG_INFO("Canonical perception order: tick {}, selector {:08X}, valid {}, reordered {}, targets {}, ids {}",GameplayInputGate().committed_tick(),ctx.r3.u32,valid,targets.reorder,targets.entries.size(),ids);
    }
    if(!valid || !targets.reorder){__imp__sub_829505C0(ctx,base);return;}
    CanonicalBody(ctx,base,targets);
}
bool em2::net::TestCanonicalPerception(){
    std::array<uint8_t,65536> memory{};auto* base=memory.data();HostState host;
    const uint32_t selector=0x1000,map=selector+52;
    // Same first word, distinct final words: all 128 ID bits participate.
    for(unsigned i=0;i<3;++i){const auto entity=0x2008+i*0x200;PPC_STORE_U32(entity,0x8208D908);PPC_STORE_U32(entity+4,0x77A20);PPC_STORE_U32(entity+16,i+1);}
    PPC_STORE_U32(map+16,0x6000);PPC_STORE_U32(map+20,3);
    // Different address/bucket order must yield full object IDs1,2,3.
    for(unsigned bucket=0;bucket<3;++bucket){const auto i=(bucket+2)%3,node=0x7000+i*32,key=0x5000+i*0x100;
        PPC_STORE_U32(0x6000+bucket*4,node);PPC_STORE_U32(node,key);PPC_STORE_U32(key+60,0x2000+i*0x200);}
    Reader reader{base};
    Targets targets;if(!targets.Build(reader,selector) || !targets.reorder)return false;PPCContext ctx{};
    targets.First(ctx,base,0x8000);if(PPC_LOAD_U32(0x8008)!=0x7000)return false;
    // Remove the next scheduled node; iteration must not visit a freed entry.
    PPC_STORE_U32(0x6000+2*4,0);targets.Next(ctx,base,0x8000);if(PPC_LOAD_U32(0x8008)!=0x7040)return false;
    targets.Next(ctx,base,0x8000);if(PPC_LOAD_U32(0x8008) || PPC_LOAD_U32(0x8004)!=3)return false;
    // Reject missing IDs, duplicate IDs and cycles instead of address tie breaks.
    PPC_STORE_U32(0x2008+4,0);PPC_STORE_U32(0x2008+16,0);Targets rejected;if(rejected.Build(reader,selector))return false;PPC_STORE_U32(0x2008+4,0x77A20);PPC_STORE_U32(0x2008+16,1);
    PPC_STORE_U32(0x5000+60,0x2400);Targets duplicate;if(duplicate.Build(reader,selector))return false;PPC_STORE_U32(0x5000+60,0x2000);
    PPC_STORE_U32(0x7040+20,0x7040);Targets cyclic;if(cyclic.Build(reader,selector))return false;
    REXLOG_INFO("Canonical perception worklist passed native entity/target layouts, full 128-bit non-address ID ordering, removed entries, end iterator, missing identity and cycle rejection; replica ID stability/native callbacks require acceptance");return true;
}

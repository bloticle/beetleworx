#include <windows.h>
#include <cstring>
#include <cstdint>
namespace em2 {bool CopyMappedPack(void*,const void*,size_t);}
namespace {
int CopyFailure(EXCEPTION_POINTERS* exception,const void* input,void* output,size_t length) {
    const auto* record=exception->ExceptionRecord;
    if(record->NumberParameters<2)return EXCEPTION_CONTINUE_SEARCH;
    const auto address=uintptr_t(record->ExceptionInformation[1]);
    const bool source=address>=uintptr_t(input) && address-uintptr_t(input)<length;
    const bool destination=address>=uintptr_t(output) && address-uintptr_t(output)<length;
    if((record->ExceptionCode==EXCEPTION_IN_PAGE_ERROR && source) ||
       (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && (source || destination)))return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}
}
__declspec(noinline) bool em2::CopyMappedPack(void* output,const void* input,size_t length) {
    __try {std::memmove(output,input,length);return true;}
    __except(CopyFailure(GetExceptionInformation(),input,output,length)){return false;}
}

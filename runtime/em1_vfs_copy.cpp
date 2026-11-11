#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace em2::em1 {
namespace {
int CopyFault(EXCEPTION_POINTERS* info, const void* source, void* destination, size_t length) {
  const auto* record = info->ExceptionRecord;
  if (record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
  const auto address = uintptr_t(record->ExceptionInformation[1]);
  const bool in_source = address >= uintptr_t(source) && address - uintptr_t(source) < length;
  const bool in_destination = address >= uintptr_t(destination) && address - uintptr_t(destination) < length;
  if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && (in_source || in_destination)) ||
      (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && in_source)) return EXCEPTION_EXECUTE_HANDLER;
  return EXCEPTION_CONTINUE_SEARCH;
}
}
// Separate /EHa, non-LTO boundary: ordinary C++ ownership code remains /EHsc.
__declspec(noinline) bool CopyRvzBytes(void* destination, const void* source, size_t length) {
  __try { std::memmove(destination, source, length); return true; }
  __except(CopyFault(GetExceptionInformation(), source, destination, length)) { return false; }
}
}

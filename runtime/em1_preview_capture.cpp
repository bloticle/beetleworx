#include "em1_preview_capture.h"
#include <rex/ui/presenter.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

REXCVAR_DEFINE_STRING(em1_preview_capture_file,"","Debug",
    "Explicit absolute new BMP path for one unmodified SDR EM1 guest-output capture after readiness");

namespace {
std::atomic_bool ready{false};
std::atomic_bool registered_ready{false};
std::atomic_bool attempted{false};
void Need(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct File {
 HANDLE handle=INVALID_HANDLE_VALUE;bool complete=false;
 ~File(){if(handle!=INVALID_HANDLE_VALUE){
  // Remove only an incomplete file created and exclusively owned by this call.
  if(!complete){FILE_DISPOSITION_INFO discard{TRUE};SetFileInformationByHandle(handle,FileDispositionInfo,&discard,sizeof(discard));}
  CloseHandle(handle);
 }}
};
void Write(HANDLE file,const uint8_t* bytes,size_t size) {
 while(size){const auto count=DWORD((std::min)(size,size_t(std::numeric_limits<DWORD>::max())));DWORD written=0;
  Need(WriteFile(file,bytes,count,&written,nullptr) && written && written<=count,"Could not write guest-output BMP");
  bytes+=written;size-=written;
 }
}
void Save(const rex::ui::RawImage& image,const std::string& configured) {
 const auto path=std::filesystem::u8path(configured);
 Need(path.is_absolute() && path.has_filename(),"Guest capture requires an explicit absolute BMP filename");
 auto extension=path.extension().wstring();for(auto& c:extension)if(c>=L'A' && c<=L'Z')c+=L'a'-L'A';
 Need(extension==L".bmp" && std::filesystem::is_directory(path.parent_path()),"Guest capture needs .bmp extension and an existing parent directory");
 Need(image.width && image.height && image.width<=16384 && image.height<=16384,"Guest capture returned invalid dimensions");
 const uint64_t row=uint64_t(image.width)*4,pixels=row*image.height;
 Need(image.stride>=row && image.stride<=256*1024*1024 && image.data.size()<=256*1024*1024 &&
      uint64_t(image.height-1)*image.stride+row<=image.data.size() && pixels<=256*1024*1024,"Guest capture returned invalid or oversized RGBX32 storage");
 // BITMAPV4HEADER uses explicit R/G/B masks to preserve R8 G8 B8 X8 bytes
 // verbatim. Negative height retains top-down order; X is not interpreted as
 // alpha. No channel conversion, filtering, HDR decoding or scaling occurs.
 std::array<uint8_t,122> header{};
 auto Word=[&](size_t at,uint32_t value,unsigned size=4){for(unsigned i=0;i<size;++i)header[at+i]=uint8_t(value>>(i*8));};
 header[0]='B';header[1]='M';Word(2,uint32_t(header.size()+pixels));Word(10,uint32_t(header.size()));
 Word(14,108);Word(18,image.width);Word(22,uint32_t(-int32_t(image.height)));Word(26,1,2);Word(28,32,2);
 Word(30,3);Word(34,uint32_t(pixels));Word(54,0x000000ff);Word(58,0x0000ff00);Word(62,0x00ff0000);Word(70,0x73524742);
 File file;file.handle=CreateFileW(path.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
 Need(file.handle!=INVALID_HANDLE_VALUE,"Guest capture refuses an existing or inaccessible output file");
 Write(file.handle,header.data(),header.size());
 for(uint32_t y=0;y<image.height;++y)Write(file.handle,image.data.data()+size_t(y)*image.stride,size_t(row));
 Need(FlushFileBuffers(file.handle),"Could not flush guest-output BMP");file.complete=true;
}
} // namespace

void em2::em1::NotifyArchitecturePreviewCaptureReady() noexcept {ready.store(true,std::memory_order_release);}
bool em2::em1::ArchitecturePreviewGraphicsReady() noexcept {return ready.load(std::memory_order_acquire);}
void em2::em1::NotifyRegisteredEm1GraphicsReady(bool value) noexcept {registered_ready.store(value,std::memory_order_release);}
bool em2::em1::Em1DrawDiagnosticsReady() noexcept {return ready.load(std::memory_order_acquire)||registered_ready.load(std::memory_order_acquire);}
bool em2::em1::ArchitecturePreviewCapturePending() noexcept {
 try{return ready.load(std::memory_order_acquire) && !attempted.load(std::memory_order_acquire) && !REXCVAR_GET(em1_preview_capture_file).empty();}catch(...){return false;}
}

void em2::em1::TickArchitecturePreviewCapture(rex::ui::Presenter& presenter) noexcept {
 static bool waiting=false;static std::chrono::steady_clock::time_point due;
 if(attempted.load(std::memory_order_acquire) || !ready.load(std::memory_order_acquire))return;
 try {
  const auto path=REXCVAR_GET(em1_preview_capture_file);if(path.empty())return;
  const auto now=std::chrono::steady_clock::now();
  if(!waiting){waiting=true;due=now+std::chrono::seconds(2);return;}if(now<due)return;
  attempted=true;rex::ui::RawImage image;
  // Presenter declines unsupported HDR output; never reinterpret FP16 as bytes.
  if(!presenter.CaptureGuestOutput(image)){
   REXLOG_WARN("[em1] guest preview capture unavailable (including unsupported HDR readback); no image written");return;
  }
  Save(image,path);
  REXLOG_INFO("[em1] unmodified guest preview captured: '{}' {}x{} RGBX32 top-down BMP",path,image.width,image.height);
 }catch(const std::exception& exception){attempted=true;try{REXLOG_WARN("[em1] guest preview capture failed: {}",exception.what());}catch(...){}
 }catch(...){attempted=true;try{REXLOG_WARN("[em1] guest preview capture failed unexpectedly");}catch(...){}
 }
}

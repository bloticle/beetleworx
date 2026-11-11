#include "em1_single_gsa_pak.h"
#include "em1_level_archives.h"
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <cstring>
#include <stdexcept>
namespace em2::em1 {bool CopyRvzBytes(void* out,const void* in,size_t size){std::memcpy(out,in,size);return true;}}
namespace {
void Check(bool value,const std::string& error){if(!value)throw std::runtime_error(error);}
}
extern "C" __declspec(dllexport) const char* run(const wchar_t* fixture){
  static std::string result;
  rex::InitLogging(nullptr,spdlog::level::off);
  struct Log {~Log(){rex::ShutdownLogging();}} log;
  try {
    using rex::X_STATUS;using namespace rex::filesystem;using namespace em2::em1;
    VirtualFileSystem fs;
    auto device=std::make_unique<HostPathDevice>("\\Device\\Harddisk0\\Partition1",fixture,true);
    Check(device->Initialize() && fs.RegisterDevice(std::move(device)),"Fixture device");
    Check(fs.RegisterSymbolicLink("d:","\\Device\\Harddisk0\\Partition1"),"Fixture alias");
    const std::vector<uint8_t> scene(70000,0x5a);
    const NativeScenePakEntry entry{"em1/runtime/probe.bin",scene};
    const std::vector<NativeScenePakRequest> batch{{"em1_level_batch_s0",{entry}},{"em1_level_batch_s1",{entry}}};
    std::string error;
    Check(MountNativeScenePaks(fs,batch,error),error);
    auto* first=fs.ResolvePath("d:\\packfiles\\em1_level_batch_s0.pak");Check(first,"First file absent");
    Check(MountSingleGsaPak(fs,"em1_courtyard_followup",entry.name,scene,error),error);
    Check(fs.ResolvePath("d:\\packfiles\\em1_level_batch_s1.pak"),"Batch file disappeared on second mount");
    File* opened=nullptr;FileAction action{};
    Check(fs.OpenFile(nullptr,"d:\\packfiles\\em1_level_batch_s0.pak",FileDisposition::kOpen,FileAccess::kGenericRead,false,true,&opened,&action)==X_STATUS_SUCCESS && opened,"Old archive open failed");
    std::vector<uint8_t> bytes(first->size());size_t read=0;auto status=opened->ReadSync(bytes.data(),bytes.size(),0,&read);opened->Destroy();
    Check(status==X_STATUS_SUCCESS && read==bytes.size() && bytes==*BuildNativeScenePak(std::span(&entry,1)),"Old archive changed");
    Check(!MountNativeScenePaks(fs,batch,error),"Existing archive shadowed");
    auto duplicate=batch;duplicate[0].basename=duplicate[1].basename="em1_level_duplicate";
    Check(!MountNativeScenePaks(fs,duplicate,error),"Duplicate names accepted");
    Check(!fs.ResolvePath("d:\\packfiles\\em1_level_duplicate.pak"),"Failed batch partly mounted");
    auto invalid=batch;invalid[0].basename="em1_level_../escape";
    Check(!MountNativeScenePaks(fs,invalid,error),"Unsafe name accepted");
    VirtualFileSystem absent;
    for(int i=0;i<70;++i){Check(!MountNativeScenePaks(absent,batch,error),"Missing base accepted");Check(error=="Original pack directory/probe is unavailable","Failed mounts leaked slots: "+error);}
    VirtualFileSystem rollback_fs;
    auto other=std::make_unique<HostPathDevice>("\\Device\\Different",fixture,true);
    Check(other->Initialize() && rollback_fs.RegisterDevice(std::move(other)),"Rollback fixture");
    Check(rollback_fs.RegisterSymbolicLink("d:","\\Device\\Different"),"Rollback alias");
    auto* unchanged=rollback_fs.ResolvePath("d:\\packfiles\\globalscripts.pak");
    std::string logical_before,physical_before;
    const bool had_logical=rollback_fs.FindSymbolicLink("d:\\packfiles",logical_before);
    const bool had_physical=rollback_fs.FindSymbolicLink("\\Device\\Harddisk0\\Partition1\\packfiles",physical_before);
    for(int i=0;i<70;++i){
      Check(!MountNativeScenePaks(rollback_fs,batch,error),"Unsupported physical root accepted");
      Check(error=="VFS did not resolve a level archive","Rollback did not release device/slot: "+error);
      Check(rollback_fs.ResolvePath("d:\\packfiles\\globalscripts.pak")==unchanged,"Rollback damaged original mapping");
      std::string target;Check(rollback_fs.FindSymbolicLink("d:\\packfiles",target)==had_logical && target==logical_before,"Rollback changed logical resolution");
      target.clear();Check(rollback_fs.FindSymbolicLink("\\Device\\Harddisk0\\Partition1\\packfiles",target)==had_physical && target==physical_before,"Rollback changed physical resolution");
    }
    const NativeScenePakRequest final{"em1_level_after_failures",{entry}};
    Check(MountNativeScenePaks(fs,std::span(&final,1),error),error);
    auto* last=fs.ResolvePath("d:\\packfiles\\em1_level_after_failures.pak");
    opened=nullptr;Check(last->Open(FileAccess::kGenericWrite,&opened)==X_STATUS_ACCESS_DENIED && !opened,"Archive writable");
    result="PASS: batch reads; repeated overlays; original ownership; retained archive bytes; collisions; invalid names; failed-slot recovery; post-publication rollback; read-only";
  }catch(const std::exception& e){result=std::string("FAIL: ")+e.what();}
  return result.c_str();
}

extern "C" __declspec(dllexport) const char* stage_level(const wchar_t* fixture,const wchar_t* image,const char* level){
  static std::string result;
  try {
    using namespace rex::filesystem;using namespace em2::em1;using rex::X_STATUS;
    VirtualFileSystem fs;
    auto device=std::make_unique<HostPathDevice>("\\Device\\Harddisk0\\Partition1",fixture,true);
    Check(device->Initialize() && fs.RegisterDevice(std::move(device)),"Level fixture device");
    Check(fs.RegisterSymbolicLink("d:","\\Device\\Harddisk0\\Partition1"),"Level fixture alias");
    std::string error;std::shared_ptr<RvzSource> source=RvzSource::Open(image,error);Check(bool(source),error);
    LevelContents contents;Check(ReadLevelContents(source,level,contents,error),error);
    std::vector<LevelSceneArchive> archives;Check(MountLevelSceneContents(fs,contents,archives,error),error);
    Check(archives.size()==contents.entry_scenes.size(),"Not all scenes mounted");
    for(const auto& archive:archives){
      const auto path="d:\\packfiles\\"+archive.archive_basename+".pak";
      File* opened=nullptr;FileAction action{};
      Check(fs.OpenFile(nullptr,path,FileDisposition::kOpen,FileAccess::kGenericRead,false,true,&opened,&action)==X_STATUS_SUCCESS && opened,"Actual level PAK not openable");
      std::vector<uint8_t> observed(archive.bytes->size());size_t read=0;
      const auto status=opened->ReadSync(observed.data(),observed.size(),0,&read);opened->Destroy();
      Check(status==X_STATUS_SUCCESS && read==observed.size() && observed==*archive.bytes,"Actual level archive bytes changed");
    }
    result="PASS: "+std::string(level)+" staged "+std::to_string(archives.size())+" full scenes through SDK VFS";
  }catch(const std::exception& e){result=std::string("FAIL: ")+e.what();}
  return result.c_str();
}

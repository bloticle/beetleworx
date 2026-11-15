#pragma once
#include "native_component_schema.h"
#include <memory>
#include <functional>
namespace em2 {
// Trusted local reflected-field component only. No network byte/pointer import.
// Captures audited direct POD members and existing POD property boxes, not hidden controller/AI state,
// general arrays/strings/reference boxes, animation graphs, events or entity lifetimes.
// Also captures audited movement/hover histories and hkaPose arrays with exact
// allocation identities; these cannot restore changed graph or object lifetimes.
class NativeComponentFieldCheckpoint {
public:
    NativeComponentFieldCheckpoint();
    ~NativeComponentFieldCheckpoint();
    NativeComponentFieldCheckpoint(const NativeComponentFieldCheckpoint&)=delete;
    NativeComponentFieldCheckpoint& operator=(const NativeComponentFieldCheckpoint&)=delete;
    bool Capture(uint8_t*,const NativeEntityOwnership&,std::span<const NativeComponentSchema>,uint64_t epoch,std::string&);
    bool Restore(uint8_t*,uint64_t epoch,std::string&)const;
    bool Matches(uint8_t*,uint64_t epoch,std::string&)const;
    std::string DescribeEntity(uint32_t entity)const;
    size_t fields()const;
    size_t bytes()const;
    size_t unsupported()const;
    // Private probe: temporarily mutate health, a matrix and a POD box and restore
    // fields at the same completed boundary, before simulation resumes.
    bool TestRoundTrip(uint8_t*,uint64_t epoch,std::string&,const std::function<bool(std::string&)>& coordinated_restore={})const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}

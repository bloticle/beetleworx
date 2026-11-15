#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <memory>
#include <string>
struct PPCContext;
namespace em2 {
// Local component of a future complete world checkpoint. Captures rigid-body
// motion, retained stored dynamic motion, supported contact streams and native
// simulation clocks. Motion headers and adjacent owned arrays are preserved.
// Audited AABB/simple-shape/game-trigger phantom poses and ordered overlaps are
// included; their native listeners/engine owners remain the same objects.
// This does not capture constraints' runtime, actions, sleeping island structure,
// arbitrary JPS callback state, script roots or entity lifetime changes.
// The audited island flag byte37 is restored only with matching membership,
// activation, track counts and solver layout; island topology is never copied.
struct NativePhysicsMotionSample {
    uint32_t body=0;std::array<uint8_t,320> bytes{};
    // Read-only diagnostic: body+128..223 includes owned structures. Never
    // flatten these bytes into checkpoint restoration without layout audits.
    std::array<uint8_t,96> metadata{};
    uint32_t island=0;std::array<uint8_t,96> island_metadata{};
    bool operator==(const NativePhysicsMotionSample&)const=default;
};
class NativeRigidBodyCheckpoint {
public:
    NativeRigidBodyCheckpoint();
    ~NativeRigidBodyCheckpoint();
    NativeRigidBodyCheckpoint(const NativeRigidBodyCheckpoint&)=delete;
    NativeRigidBodyCheckpoint& operator=(const NativeRigidBodyCheckpoint&)=delete;
    // Completed solver boundary, original native owner only. Epoch is issued
    // by the world owner and must change on unload/replacement.
    bool Capture(PPCContext&,uint8_t*,uint32_t world,uint64_t epoch,std::string&);
    // Same native objects, types and shapes only. Preflights complete retained
    // body/constraint/action/phantom inventories before changing any motion.
    // The full world owner must restore the other components and provide its
    // transaction backup; an exception during native writes is a failed restore.
    bool Restore(PPCContext&,uint8_t*,uint32_t world,uint64_t epoch,std::string&)const;
    // Compare captured motion/phantoms/clocks at a completed owner boundary.
    bool Matches(PPCContext&,uint8_t*,uint32_t world,uint64_t epoch,std::string&)const;
    // Native references require explicit owner-thread release before teardown.
    bool Release(PPCContext&,uint8_t*);
    bool SampleMotion(uint8_t*,std::vector<NativePhysicsMotionSample>&,std::string&)const;
    unsigned bodies()const;
    unsigned contacts()const;
    unsigned stored_motions()const;
    unsigned phantoms()const;
    unsigned overlaps()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}

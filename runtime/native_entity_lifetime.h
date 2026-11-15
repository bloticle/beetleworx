#pragma once
#include <cstdint>
namespace em2 {
// Process-local observation fence for audited native registry insertion,
// removal, deferred teardown requests and nonempty queue processing. Never
// serialize as network identity.
// False during a native mutation or after revision exhaustion. This does not
// restore lifetimes or cover arbitrary component/property changes.
bool SampleNativeEntityLifetimeRevision(uint64_t&) noexcept;
bool TestNativeEntityLifetime();
}

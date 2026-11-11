#pragma once
namespace em2 {
#if __has_include("../generated/tests/instructions.cpp")
inline constexpr bool HasLoweringTests = true;
bool TestLowering();
#else
inline constexpr bool HasLoweringTests = false;
inline bool TestLowering() { return false; }
#endif
}

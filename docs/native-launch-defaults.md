# Native jump-action defaults

New jump actions explicitly initialize their two direction vectors. The
original 176-byte constructor leaves those fields untouched, allowing values
from an earlier allocation to become launch or facing inputs.

Initialization runs before the original constructor. Native construction,
subclass setup and later admission calculations can assign their own values
normally. The change applies automatically and needs no setting.

The constructor-boundary regression covers five dirty-memory patterns, later
native assignments, exact PPC register and host FP/error-state preservation,
single invocation and exception propagation: 40 checks in total. Its callback
is controlled test code; it does not simulate an entire NPC or contact world.

A private two-copy replay confirmed zero initial vectors on both copies and
1,696 consecutive matching recorded world observations. Its detailed trace
still contains other first-use storage and component-byte differences. This
is a targeted initialization fix, not proof of complete world synchronization.
The startup Lua disagreement and broader multiplayer acceptance remain open.

Source-built D3D12 presenter for the EM2 port.

Baseline source:
https://github.com/rexglue/rexglue-sdk/blob/v0.1.0/src/ui/d3d12/d3d12_presenter.cpp

The corresponding upstream header matches the installed header after newline
normalization. prepare-presenter-source.py verifies the pinned SDK archive and
header, and recovers its exact ten DXBC shaders into the build directory.
prepare-renderer-library.py removes only the old presenter object from the local
UI archive; the installed SDK is never edited. No class layout change is allowed
without rebuilding every SDK caller that constructs the presenter.

This baseline retains SDR behavior. It permits subsequent coordinated HDR color,
buffer, display and shader changes in source; it does not itself enable HDR.
Original source copyright and license notices are retained. See SDK-LICENSE.

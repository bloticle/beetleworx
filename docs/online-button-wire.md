# Native button transport

NativeActionV3 extends the existing Movement/Jump command with a fixed
104-byte button payload: a 32-byte definition hash, an eight-byte source tick
and a 64-byte action bitmap. The bitmap identifies the original ordinal in
the default or active input context, preserving native order. At most 256
outputs may be set. A maximum sixteen-peer frame with both Jump edges fits
the existing 8 KiB packet and 256 KiB queued-byte limits without increasing
either limit. V1 and V2 command bytes remain unchanged.

The SHA-256 schema includes the supported native action names, kinds, keys,
ordinals, repeat configuration, active context, trigger threshold and retail
listener capability. It excludes guest addresses, interned UID pointers,
allocation padding and mutable button/timer state. Receiver metadata matching
therefore resolves its own local descriptors, even with different allocation
addresses. Unknown ordinals, non-retail UI bits and schema mismatches reject
the whole batch. The boolean format rejects nonzero or negative-zero float
arguments instead of losing future analog or aiming data.

The Room validates source ticks against the negotiated input delay. Once a
native button stream starts, its schema and actor incarnation remain fixed;
source ticks cannot repeat, skip or wrap, and the stream cannot downgrade to
an older command type. Button and Jump validation commit together only after
the complete command succeeds. Host input and client authoritative-frame
validation both enforce these rules.

Tests cover all truncation lengths, the old V2 golden bytes, independently
packed schema hash bytes, relocation, unknown/UI bits, local transactional
retry, malicious client input and host schema substitution. A full 64-frame
window carries 256 button decisions and both Jump edges for each of sixteen
real loopback connections. A separate source fixture carries an actual native
Oswald button result through the codec and authoritative echo.

This is transport and metadata matching. It does not dispatch these actions
into the live game. Existing Movement/Jump adapters explicitly reject V3 until
receiver ownership, ordered native callbacks and aiming are integrated. It
does not enable the public online menu or establish sixteen playable actors.

# Retail button receivers

Receiver preflight owns separate snapshots for the two admitted retail human
controllers. It validates their world, actor and load incarnations, entity,
controller, listener and native Movement definition. The original native
context selector confirms the active input context using a private context
copy. This does not borrow or replace the local physical producer's lease.

The receiver checks the actual event headers, listener and action lists,
definition names and immutable action bytes before mapping a packet. Native
latches and repeat timers may evolve without changing those definitions.
Actor reuse, changed definitions or unreadable metadata retire the receiver;
restoring bytes alone cannot revive it. Explicit reset and capture are needed.
Invalid packets leave both the output and valid receiver state available for
a correct retry. Foreign threads cannot prepare or reset an armed receiver.

`EnableRetailButtonDelivery` explicitly pins both receivers before the first
gameplay tick. The Movement adapter continues to reject V3 unless this receiver
capability is installed. Preflight checks both complete commands, native Jump
edge correspondence and source histories before button replay. Replay requires
both players' exact commands to be staged and both native Movement deliveries
to have completed in the same admitted tick. Merely matching metadata does not
authorize a callback.

Replay invokes the original derived character handler and its original base
handler with the complete local event and action records. A scoped permit
admits only that action, actor and call path. The records retain authored order.
Jump records use this same path, rather than being replayed a second time by
the separate Jump adapter. Ordinary non-Movement character callbacks remain
quarantined between ticks and after a partial-frame failure. Callback exceptions
clear the permit and restore caller registers, scratch, floating-point state
and last-error state; the failed frame cannot be retried.

The allocated-image fixture exercises original derived/base handlers and a
bounded task observer for both characters. It checks complete descriptors,
missing tick ownership, later-slot schema rejection, duplicate delivery,
neutral frames, nested foreign input and a task exception before the second
actor. It does not establish live paint, interaction, animation or damage.

The live Room uses Movement/Jump V2 by default. Experimental V3 is available
with `em2_room_native_buttons=true` on both peers, or `-Gameplay -NativeButtons`
in the private pair launcher. This is a development test option, not an online
menu or a negotiated public lobby capability.

Readiness captures both receiver definitions and a neutral native button
baseline. Each sealed local hardware sample then feeds the original native
button interpreter. Its decisions travel through SubmitLocal and the normal
authoritative echo before the retail delivery adapter may consume them. The
outgoing source clock is ahead of the receiver by the negotiated input delay;
outgoing metadata checks never advance the receiver's history.

The initial delayed frames remain the Room's exact neutral V1 commands. At the
first genuine input frame, both slots must switch to V3 with the correct source
ticks. Early V3, late legacy commands, mixed phases, altered startup commands,
and cutover overflow reject. V3 Jump events use button delivery exclusively;
the older Jump adapter remains responsible for the neutral startup prefix.

Two private neutral-input pairs matched 1,655 and 1,696 recorded V5
character-physics ticks while V3 commands flowed. The final pair ran without
Lua-owner tracing. No gameplay controls or screenshots were automated.
This establishes that transition and recorded-field comparison, not complete
world determinism or live paint, aiming, interaction and damage. Another pair
diverged in native random calls during bootstrap before V3 activated; that
intermittent initialization issue remains unresolved. Aim, tool targeting and
lifecycle handling also remain unfinished. The public online menu is disabled.

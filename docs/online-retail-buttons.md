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

These functions are read-only preflight. Their returned descriptors are not
permission to invoke arbitrary callbacks. Native delivery still needs an
admitted tick, ordered replay, suppression of duplicate ordinary input and
complete frame ownership. Aim and tool targeting remain separate unfinished
work. V3 gameplay is still rejected by the live Movement/Jump adapters.

# Native button action production

The native action producer evaluates digital buttons and analog trigger
buttons through the original SDK history updater, condition checker and
action interpreter. Tap, press, held, repeat and release decisions use the
original routines. It does not replace them with button-to-action guesses.

`NativeButtonScope::TriggersOnly` retains the trigger-only inventory.
`DigitalAndTriggers` additionally discovers the selected logical user's
face, shoulder, D-pad and stick-button definitions. Both retain default and
active context order. Unknown native keys and unsupported combinations,
conditions, curves or action kinds are reported as exclusions. Exclusions
must not be treated as complete control coverage. Default UI descriptors are
inventory/math records, not permission to dispatch pause or menu callbacks.

Private history copies use a source-tick clock at the authored 30 Hz. Only
audited native button-duration and repeat callsites can read that clock.
Unrelated engine time queries retain their normal path. Caller context,
scratch, live devices, action definitions and host FP/error state are
preserved. A failed evaluation does not commit source history or time.
Arming also requires settled neutral native button histories. Co-op joining
can leave Oswald's Start record held or just released; let the original updater
settle both current and previous state before establishing a fresh baseline.
Do not clear that live history or manufacture a release to admit the producer.

The raw producer API requires the actual native device index. The Room
overload accepts only a sealed sample from the existing physical sampler,
validated against its Room, current binding, controls generation and source
tick. This supports physical controller0 controlling canonical Oswald/device1
without relabelling arbitrary raw input or polling the device a second time.

The regression fixtures exercise native short taps, quarter-second holds,
timed repeats, releases, mixed B/LT input and stale binding/scope rejection.
A connected loopback source fixture tests a non-neutral controller0 press
through canonical Oswald/device1, including unissued/wrong-Room/stale tokens
and the unchanged strict API's physical/native mismatch rejection.

This is action production, not network action delivery. The live Room adapter
still admits Movement, Jump and native right-stick Look only. Delivering
additional gameplay actions requires receiver-owned descriptors, ordered
native dispatch, aiming and context/lifecycle handling. No public online menu
or gameplay capability is enabled by these helpers.

# Failure Taxonomy

**The single most useful thing this project produced.**

A hardware agent's output channel is character-level. The target machine is free to reinterpret those
characters. That gap produced every expensive bug in the project, and the fix was not a better retry —
it was teaching the harness to **classify the failure before acting on it**.

## Three failures, two identical symptoms

"My text is not on screen" has three distinct causes with three different fixes. Acting on the wrong
one makes things worse, sometimes much worse.

| Discriminator | Cause | Fix |
|---|---|---|
| `queued < len` in the type report | the device never put it on the wire | transport / HID problem — fix the sending side |
| the screen did **not change** at all after the action | the keystrokes went somewhere else — **focus** | click into the target field, then retype |
| the screen **did** change, but the text is wrong | the characters were **reinterpreted** on the target | change the input *channel*, not the retry count |

The middle and bottom rows are the dangerous pair, because they look identical on screen and their
fixes are opposites. A model (or a developer) that conflates them will toggle an input method to fix
a focus problem, or retype into a field that is not focused, and burn an entire task doing it.

**Both discriminators are now produced by the harness, not inferred by the model:**

- `queued=ALL N/N` comes from the HID layer, which counted what it actually sent.
- `screen since last screenshot: unchanged / changed` comes from a frame fingerprint.
- An action summary is tagged `[NO SCREEN CHANGE]` when the batch changed nothing.

They appear in the status line at the top of every request.

## Why "the screen did not change" is the key signal

It is the only discriminator that separates *the keystrokes never landed* from *they landed and were
transformed*. And it is exactly the case a model cannot reason its way to: from inside the model's
view, "I typed `Vicrl-7391-A` and the field says `Vicrl--A`" and "I typed it and the field is empty"
are both simply *wrong*, with no visible difference in kind.

Measured example of what this catches: an operator clicked the taskbar's language indicator while the
agent was running. Focus left Notepad. The agent's next `type` reported `queued=ALL 12/12` and the
editor still read `0 characters`. That is not an input-method problem and no amount of toggling would
have fixed it — the keystrokes went to the desktop.

## The channels

Once you know the characters were reinterpreted, the question becomes *which channel* to use. The
device's answer is to offer more than one, and to prefer the ones that are not character-level at all:

| Channel | Mechanism | Measured on this target |
|---|---|---|
| main number row | default | ❌ digits consumed by the IME |
| numeric keypad | `digits_numpad` | ❌ also consumed |
| **avoid the GUI** | `run "cmd /c echo ... > file"` — no editor, no IME | ✅ |
| **clipboard hop** | put the value on the clipboard, then `ctrl+v` | not yet measured |
| **verify the mode first** | probe, then type | the right first move |

The generalisation matters more than the specific rows: **when an output channel can be rewritten by
the environment, add a second channel and a way to tell which one worked.** A retry is not a second
channel.

## Verification must be separated from the thing it verifies

This is the trap that cost the most time, and it is subtle enough to be worth stating as a rule:

> A probe sent in the same step as the action it is probing measures the *old* state.

The input-method toggle takes effect asynchronously on the target. Three toggles were sent, each
immediately followed by a probe **in the same action batch**, and all three probes failed — so the
conclusion was "toggling does not work on this machine". Then the very next multi-word type, with no
toggle at all, came through with its letters, spaces and punctuation intact, which only happens in
English mode. **The toggles had worked every time.** The probe was simply transmitted before the
switch landed.

A false negative is worse than no measurement: it sends you to a different channel that was never
needed. Two consequences:

- The probe is now required to be **its own step** (a full model round trip, ~4 s — far more than
  enough for the switch to settle).
- The firmware additionally waits 150 ms after sending a toggle key, so any other action in the same
  batch cannot race it either.

## Two more rules that came out of the same work

**Do not repair text character by character.** Patching a wrong string leaves the mechanism that
corrupted it armed, so the next attempt breaks again. Fix the *mode*, or clear and retype.

**Know what each cleanup key destroys.** While a candidate window is open, the composition has not
entered the document yet — so `escape` discards it while touching nothing. `enter` commits it, which
means it must then be undone. And `clear_first` is `ctrl+a`: the **entire field**, which on a document
that already contains earlier work is destructive. The harness therefore also offers
`replace_chars: N` — `shift+left` × N, an exact and local replacement, where N is readable from the
application's own character count.

See also: [Input Method Troubles](Input-Method-Troubles).

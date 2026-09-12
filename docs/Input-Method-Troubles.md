# Input Method Troubles

The longest-running and most expensive bug in this project's history. It is written up because the resolution is counterintuitive, and because it is a *class* of problem rather than a Windows quirk: **an output channel that the environment is allowed to rewrite.**

## The symptom

With a Chinese IME active, typing the ASCII string `Vicrl-7391-A` into Notepad produces `Vicrl--A`.

Everything about that result is informative:

| Evidence | What it establishes |
|---|---|
| `last type: "Vicrl-7391-A" — device queued ALL 12/12 key events` | the device put every keystroke on the wire |
| the application's own status bar reads `8 个字符` | 12 keys, 8 characters — exactly 4 lost |
| the surviving 8 are `Vicrl`, `-`, `-`, `A` | **letters and punctuation survive; only the digits are consumed** |

Digits on the **main number row** are candidate-selection keys while a composition is open. That part is well known. What took a further experiment to establish is that the **numeric keypad is consumed too** — measured with Num Lock confirmed on from the host's LED report, all 12 keys queued, and the digits still gone. The natural workaround ("type digits on the keypad, IMEs only hijack the number row") is **false on this IME**, and the code keeps the option only because other IMEs may differ.

The same evidence explains a much more surprising observation: a **50-word English story typed while the mode was still Chinese came through perfectly**, spaces and punctuation included. Only digits are at risk.

## Why it consumed an entire task

The model did not simply fail. It failed *in an escalating loop*, and the loop was caused by the prompt, not by the model.

The rules at the time said:

> press `shift` to toggle Chinese/English; if that does not work, try `win+space` or `ctrl+space`

In Microsoft Pinyin, **`shift` and `ctrl+space` flip the same state.** Told to try one and then the other, the model toggled twice and landed exactly where it started — then retyped the string in Chinese mode, and failed again.

That is the generalisable failure: **an unobservable parity, presented as a list of alternatives.** Every attempt is 50/50, none of them is checkable, and the model has no way to know it is oscillating.

Two further details matter:

- **`shift` is conditionally effective.** It only toggles when a text caret is present, and an open composition can swallow it instead of switching. `ctrl+space` works without a caret. So the fallback was more reliable than the primary.
- **Escape is not a mode fix.** It discards the pending composition; the very next letter opens a new one. Used alone it changes nothing about Chinese/English — but it *is* required first, because a pending composition consumes the toggle.

## The resolution

Three ideas, in order of how much they mattered.

**1. Make the unobservable parity measurable.** The device cannot read the input mode, but it *can* read whether a digit survived:

```
press "ctrl+space"          →  (next step, never in the same batch)
type "a1" with clear_first  →  read the application's own character count
   "2 个字符" → mode is right
   "1 个字符" → still Chinese; toggle once more, re-probe
```

Two failed probes with the probe as its own step means the machine needs a different channel — stop toggling. The status line also reports `IME toggles sent: N`, so "you have already tried this twice" is a fact rather than a memory.

**2. Separate the probe from the toggle in time.** This is the trap described in [Failure Taxonomy](Failure-Taxonomy.md): three same-batch probes reported a working toggle as broken. The probe must be its own step.

**3. Provide channels that are not character-level.** When the task does not require typing into a GUI at all, do not:

```
run "cmd /c echo <value> > C:\p.txt"     →  no editor, no IME
run "notepad C:\p.txt"                   →  reading needs no typing
run "cmd /c echo <value>|clip"  + ctrl+v →  the payload never traverses the keyboard
```

The clipboard path is the most promising general answer — `ctrl+v` is a single atomic event that no IME can reinterpret — but it has not been measured end to end yet, and the command that populates the clipboard is itself typed, so it inherits the same open question about the Run dialog.

## The honest limits

- **A task that genuinely requires typing digits into a GUI text field on a machine whose IME eats them is blocked.** The workarounds cover "put this value in a file" and "paste this value", not "type this number into that box".
- **Deleting the Chinese keyboard is not a prerequisite.** It is a legitimate deployment choice, but it is not required, and treating it as the answer would make the whole device conditional on a prepared machine.
- **The IME state is readable by eye.** Microsoft Pinyin draws a 中/英 badge near the caret, and `win+space` opens a switcher listing every installed input method with the active one highlighted — a large, readable overlay, and on many machines the only way to see the state at all. The agent is told to read it rather than guess.

## What is still open

1. **Does `ctrl+space` cleanly switch the mode on this machine, with a properly separated probe?** Every previous attempt was contaminated by a same-batch probe or by a second toggle.
2. **Does the Run dialog preserve digits?** If it does, the clipboard hop becomes a complete solution, because the payload then never touches an IME-active text control. The test is self-diagnosing: if the digits are eaten, `cmd /c echo |clip` puts `ECHO is on.` on the clipboard instead of the value.

Both are one short run each.

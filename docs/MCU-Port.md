# The MCU Port — Victrl on an ESP32-P4

This is the implementation in the repository. The prototype ran on a PC; this runs on a microcontroller with **no operating system**, and the whole harness came with it.

## What is on the chip

| | |
|---|---|
| Board | ESP32-P4 — dual-core RISC-V, 32 MB PSRAM, 16 MB flash |
| Runtime | ESP-IDF 6.0.1 + FreeRTOS. No Linux, no Python, no OS. |
| WiFi | ESP32-C6 over SDIO (ESP-Hosted) |
| Eyes | MS2109 USB capture card, MJPEG 1920×1080 |
| Hands | TinyUSB composite HID — keyboard (6KRO, 1 ms) + absolute mouse (0…32767) |
| Storage | microSD (FAT32): sessions, plans, trajectories, task logs |
| UI | on-device WebUI served by the board itself |

And everything that makes a stateless model behave like an agent:

- the sense → think → act state machine
- the trajectory, the plan, and the conversation-window store
- the **status line** — cursor position, held buttons, lock-key state, screen-change detection, input-method toggle count, and the exact string the last `type` put on the wire
- verification and failure classification
- the MJPEG decode → region-of-interest crop/scale → re-encode → base64 pipeline
- adaptive reasoning effort, and per-step latency instrumentation

## What the port actually cost

The interesting part is not that it fits. It is *what broke* when the safety nets disappeared. In Python on Linux, a 6 MB allocation that is freed a moment later is unremarkable; a byte-wise string truncation is invisible; a background thread is free. None of that is true here.

### The hardware decoder cannot scale or crop

The ESP32-P4's JPEG decoder emits the picture at its native resolution. Full stop. Two consequences:

1. The decode buffer must be sized for the **source** (padded to the driver's 16-byte alignment), not for a downscaled target. Sizing it for the target made every decode fail with `JPEG_ERR_NO_MEM` — and the caller quietly fell back to the full frame, so the feature silently never worked.
2. Worse, once the buffer was large enough: passing the **downscaled** width/height to the encoder while feeding it full-size pixels makes it read only the **top-left crop** of the image. The model then sees a picture missing its right and bottom, and every normalised coordinate is offset. That is a silent, plausible-looking wrong answer — the worst kind.

Scaling therefore has to happen in software, after a full-resolution decode. Which raised the next question honestly: is that worth it? Measured, yes — the crop+resample costs ~197 ms, but the smaller payload saves more than that in base64 alone (25 ms vs 150 ms) and cuts upload 4.7×.

### Codec engines were being rebuilt every step

The first version created and destroyed the decoder and encoder engines, and allocated a fresh 6.2 MB PSRAM decode buffer, **on every step**:

```
jpeg_new_decoder_engine()              per step
jpeg_alloc_decoder_mem(1920*1080*3)    per step, 6.2 MB
jpeg_decoder_process()                 per step
jpeg_del_decoder_engine()              per step
free(6.2MB)                            per step
... then the same again for the encoder
```

The capture card delivers one fixed geometry for the entire run, so all of it was invariant. It was invisible because the whole thing was inside a single opaque `jpeg=180ms` timing field — which is why the pipeline is now instrumented as `setup / decode / resample / encode`. **You cannot optimise what you have merged into one number.**

### A silent truncation can end a task

A fixed-size buffer plus `strncpy`/`snprintf("%.Ns")` truncates by **bytes**, and the task goal was Chinese. Cutting a three-byte character in half produces invalid UTF-8, and a JSON request body must be valid UTF-8 — so the API rejected the entire request with a 400 while the harness reported it as a generic HTTP failure. Every step of that task failed identically, and the evidence that mattered was in one place: the stored goal ended in a replacement character.

The fix is two-layered on purpose: character-safe truncation at every site, plus a single `utf8_sanitize()` at the one point every request passes through, so the invariant is *enforced* rather than *trusted*.

### The same bug, one layer up

After that was fixed the requests worked — and the model still finished after step 1, because the goal was being truncated to 255 bytes at the HTTP boundary, in a `char task[256]`. The fix had been applied to two layers and the third still cut the user's instruction in half. **A silently shortened goal is the most damaging failure this project has had**, because the agent confidently completes a shorter task than it was given. Over-long input is now an explicit error.

### Stacks and static buffers matter again

The main task's default 3.5 KB stack was not enough for session handling and produced a `Stack protection fault`. Component data structures that would be heap-allocated by reflex in Python are stack or static allocations here, and their sizes feed back into how much of the 32 MB is actually usable.

### What became possible *because* it is a device

Three capabilities have no equivalent in the prototype, and they are the reason the port was worth doing beyond the size reduction:

- **Region-of-interest zoom.** The model can ask to be shown a sub-region, and the harness inverts the mapping exactly. Small targets become legible; upload drops 4.7×.
- **A status line the device asserts.** The board is the only party that knows where the pointer is, whether the screen changed, and what lock keys are set. It says so every step instead of asking the model to infer it.
- **Failure classification.** Distinguishing "never arrived" from "was reinterpreted" — see [Failure Taxonomy](Failure-Taxonomy.md).

### Numbers

| | Measured |
|---|---|
| Wall clock per step | 3.8 – 6.8 s |
| API share | 64 – 91 % |
| Request body, full frame / zoomed | 406 KB / 87–89 KB |
| Decode buffer | 6.2 MB, cached across steps |
| Output throughput | ~116 tokens/s |

Full detail and the complete decision record: [`docs/技术文档.md`](技术文档.md).

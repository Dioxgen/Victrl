# Victrl — Hardware AI Agent (ESP32-P4)

> **English** | [中文](技术文档.md)

A hardware-level AI computer operator built on the ESP32-P4. It grabs the screen through a USB HDMI capture card, sends it to a multimodal model for analysis, and executes the action instructions the model returns over USB HID (keyboard plus absolute mouse) on the target computer.

**Nothing needs to be installed on the target computer** — a pure-hardware vision-and-control loop.

---

## Hardware platform

| Part | Model | Purpose |
|------|------|------|
| Main controller | ESP32-P4 | USB Host (UVC capture), USB Device (HID output), inference scheduling |
| Co-processor | ESP32-C6 | WiFi 6 + BLE 5.3, talks to the P4 over SDIO |
| Capture card | MS2109 | HDMI to USB, outputs an MJPEG video stream |
| Display | NV3007 | 142×428 SPI LCD, landscape status display |
| Buttons | GPIO0/1 | start / pause / abort |
| Storage | SD card | FatFS, holds configuration, logs and prompts |

---

## Architecture

```
ESP32-P4 (main controller)
├── UVC capture (MS2109 → MJPEG frames)
├── JPEG compression (hardware codec, trades quality for size)
├── LLM client (HTTPS POST, DeepSeek Responses API)
├── USB HID keyboard + absolute mouse (TinyUSB)
├── Web server (HTTP/1.1 + Dashboard)
├── LCD display (NV3007 142×428 SPI)
├── Buttons (GPIO interrupt + debounce)
├── Storage (FatFS SDMMC, SD card)
└── WiFi (ESP-Hosted SDIO → C6)

ESP32-C6 (co-processor)
├── WiFi STA (lwIP)
└── BLE HID (not implemented)
```

---

## FreeRTOS tasks

| Task | Priority | Stack | Function |
|------|--------|-----|------|
| agent_loop | Medium | 16KB | main loop: capture → infer → execute → wait |
| httpd | Low | 6KB | web server (internal esp_http_server task) |
| btn_monitor | High | 2KB | GPIO interrupt / debounce / short-vs-long press detection |
| lcd_refresh | Low | 3KB | LCD status refresh |
| usb_lib | High | 4KB | USB Host event handling (inside the UVC component) |

---

## State machine

```
IDLE ← long press GPIO1 (abort) ← any state
  │
  ├─ short press GPIO0 / WebUI start → RUNNING
  │     ├─ short press GPIO1 → PAUSED
  │     │     ├─ short press GPIO1 → RUNNING (resume)
  │     │     └─ WebUI step → STEP → run 1 step → PAUSED
  │     ├─ WebUI stop / long press GPIO1 → STOPPING → IDLE
  │     └─ done=true → IDLE
  │
  └─ WebUI start → RUNNING
```

---

## Main loop

1. **Capture** — grab one MJPEG frame over UVC (the MS2109 streams continuously)
2. **Compress** — re-encode at lower quality with the hardware JPEG encoder (quality is configurable)
3. **Encode** — Base64 encode
4. **Request** — HTTPS POST to the DeepSeek Responses API (`/responses`)
5. **Parse** — pull out `output_text`, then parse the action array with cJSON
6. **Execute** — run the HID actions one at a time (click/move/drag/scroll/press/type/wait)
7. **Sleep** — `sleep_before_next` (minimum 1s)
8. **Repeat** — back to step 1

---

## Supported actions

| Action | Description |
|------|------|
| `click` | with box_2d = move to the target first, then click; without box_2d = click in place |
| `move` | move the cursor to the centre of box_2d |
| `drag` | drag from_box → to_box, 8-step interpolation |
| `scroll` | wheel delta_y (horizontal is not supported, see below) |
| `press` / `hotkey` | key combination (mod+key format); the two are equivalent |
| `type` | type ASCII text; `clear_first: true` presses Ctrl+A first |
| `run` | **atomic launch**: does Win+R → types `command` → Enter by itself, and self-checks whether the screen changed |
| `wait` | wait wait_seconds seconds |
| `release` | release the specified mouse button |

> `run` is the preferred action for launching a program — one call replaces the four round trips the model would otherwise hand-assemble, and it verifies internally that the command really ran (if nothing on screen changes, it presses Enter once more).

---

## Feature overview

| Feature | Status |
|------|------|
| UVC capture (MS2109 MJPEG) | Done |
| Hardware JPEG compression (configurable quality) | Done |
| Base64 encoding (mbedtls) | Done |
| LLM API (DeepSeek Responses API) | Done |
| JSON response parsing (fault-tolerant, auto-detects the coordinate format) | Done |
| USB HID keyboard (6KRO) | Done |
| USB HID absolute mouse (0~32767) | Done |
| Click / drag / wheel / double-click | Done |
| Model reasoning effort (reasoning_effort: low/high/max) | Done |
| State machine (IDLE→RUNNING→PAUSED→STEP→STOPPING) | Done |
| SD card storage (config / profile / plan / logs / screenshots) | Done |
| Multiple profile management | Done |
| Short-term memory (10 entries + compression) | Done |
| System prompt rotation (full / condensed) | Done |
| HTTP server + Dashboard WebUI | Done |
| NV3007 142×428 SPI LCD status display | Done |
| GPIO buttons (short press start / pause / resume, long press stop) | Done |
| WiFi (ESP-Hosted SDIO → C6) | Done |
| NTP time sync (Beijing time) | Done |
| Mouse self-test | Done |
| Keyboard self-test | Done |
| BLE HID (C6 co-processor) | **Not implemented** (the P4 has no SOC_BT_SUPPORTED) |

---

## Project structure

```
Victrl/
├── main/                  # application entry point
├── components/            # component library
│   ├── agent_core/        # state machine + main loop + action execution
│   ├── cloud_client/      # LLM API client + Base64 + JSON parsing
│   ├── hid_device/        # USB HID keyboard + absolute mouse
│   ├── jpeg_utils/        # hardware JPEG encode/decode
│   ├── storage_manager/   # config / profile / plan / log management
│   ├── web_server/        # HTTP server + WebUI API
│   ├── wifi_manager/      # WiFi (ESP-Hosted → C6)
│   ├── display_task/      # LCD status display
│   ├── button_driver/     # GPIO button driver
│   ├── uvc_capture_card_driver/  # MS2109 UVC capture
│   ├── sdmmc_driver/      # SD card (FatFS)
│   └── NV3007_driver/     # SPI LCD driver
├── sdcard/                # SD card file templates
│   ├── config.json        # runtime configuration
│   ├── sysprompt/         # system prompts
│   ├── web/               # WebUI HTML
│   └── profiles/          # device profiles
├── partitions.csv         # partition table
├── sdkconfig.defaults     # default configuration
└── README_CN.md           # this file
```

---

## Quick start

### 1. Prepare the SD card

Copy everything under `sdcard/` to the root of the SD card, then edit `config.json`:

```json
{
  "uvc": {
    "capture_width": 1920,    // UVC capture resolution
    "capture_height": 1080,
    "output_width": 1920,     // target computer screen resolution (used for HID coordinate mapping)
    "output_height": 1080
  },
  "api": {
    "endpoint": "https://api.deepseek.com/responses",
    "key": "your DeepSeek API key",
    "model_name": "deepseek-flash",
    "timeout": 120,
    "max_retries": 3,
    "max_output_tokens": 16384,
    "enable_thinking": true,
    "reasoning_effort": "high",         // low | high | max
    "upload_max_dim": 1360,             // downsample size for the longest edge of an uploaded frame; 0 = no downsampling
    "save_frames": false                // write each step's screenshot to the SD card (about 150-250KB blocking write)
  },
  "agent": {
    "max_actions": 100,
    "history_max_len": 10,
    "system_prompt_interval": 0         // 0 = no rotation, always use the full prompt (good for the prefix cache)
  },
  "wifi": {
    "ssid": "your WiFi SSID",
    "password": "your WiFi password"
  }
}
```

> `endpoint` is used **verbatim as the request URL**, so the `/responses` suffix is required. If your gateway returns 404, try `https://api.deepseek.com/v1/responses`. `reasoning_effort` only takes effect when `enable_thinking: true`; setting that to false forces it down to `low`. `upload_max_dim` defaults to 1360: the model side scales anything larger than about 1300×1300 back down, and each image is billed against a cap of about 1024 tokens, so uploading a bigger image only costs bandwidth and buys no accuracy. `system_prompt_interval` defaults to 0 (no rotation). Rotating in a condensed prompt saves very few tokens and breaks the Responses API prefix cache, so it is not worth it.

### 2. Build and flash

```bash
idf.py build flash monitor
```

### 3. Usage

- **WebUI**: open `http://<device IP>` in a browser. Two tabs:
- **Dashboard** — task control, system / network / time status, the previous step's latency breakdown, the plan, and the run log
- **SD card files** — browse / edit / create / delete files on the SD card directly in the browser
- **Buttons**: short press GPIO0 to start; short press GPIO1 to pause / resume, long press to stop
- **Automatic**: on boot it connects WiFi and initialises every peripheral; NTP syncs asynchronously in the background

### WebUI file management

No more pulling the SD card in and out. Any file under `/sdcard` can be edited in the browser:

| Endpoint | Description |
|------|------|
| `GET /api/fs?path=/sdcard/...` | directory listing (JSON, directories first, with sizes) |
| `GET /api/fs/read?path=...` | read a file (`text/plain`, 128KB limit) |
| `POST /api/fs/write` | write `{path, content}` |
| `POST /api/fs/delete` | delete a file or empty directory with `{path}` |
| `POST /api/fs/mkdir` | create a directory with `{path}` |
| `POST /api/reboot` | reboot the device (needed after editing `config.json`) |

Security constraint: every path must resolve inside `/sdcard`; anything containing `..` or a prefix of the form `/sdcardX` is rejected outright. **The WebUI itself has no authentication**, so use it only on a trusted LAN.

> Changes to `config.json`, the system prompt and even `web/index.html` can all be saved directly; `config.json` and the prompt only take effect after a reboot or a fresh task, while `index.html` just needs a page refresh. Note: **if you break `web/index.html` while editing it, the page itself will not open** — copy it to `index.html.bak` before you start.

### Key data on the dashboard

The Dashboard shows: agent state and step count, consecutive failures, wait mode, the current profile, internal RAM / PSRAM free and largest allocatable block, SD card usage, reset reason, WiFi status / IP / MAC / RSSI, device time and NTP sync state (including the server list, how long ago the last sync was, and uptime), plus **a bar chart breaking down the previous step's latency** (equivalent to the `timing(ms)` line on the serial port, showing how much each of capture / JPEG / base64 / serialisation / TCP+TLS / HTTP / parse / SD write / execute / sleep took).

### NTP

Startup **no longer blocks waiting for it** (it used to stall app_main for up to 30s). It now syncs asynchronously in the background, and the state is visible on the Dashboard. The server order is `ntp.aliyun.com` → `cn.pool.ntp.org` → `pool.ntp.org`.

> **Important**: `CONFIG_LWIP_SNTP_MAX_SERVERS` was `1`, so the second server set in code was **silently dropped** by lwIP — only `pool.ntp.org` was ever tried, and that host is often unreachable from mainland China; this is exactly why the first boot log contained `NTP sync timeout after 30s`. It is now `3`, and `CONFIG_LWIP_SNTP_STARTUP_DELAY` is off (it delays the first request by a random amount of up to 5s). Both settings are also recorded in `sdkconfig.defaults`.

---

## Performance instrumentation

Every step prints one line of stage timings (to the serial port and to the task log file):

```
step 7 timing(ms): total=14210 capture=35 prep=520 (jpeg=310 b64=90 json=120)
                   connect=1840 http=6100 parse=6 sdlog=42 exec=830 sleep=1000
```

| Field | Meaning | What to look at |
|------|------|--------|
| `total` | total wall clock for this step | the only final metric |
| `capture` | waiting for a UVC frame + copying it out | consistently large means the capture pipeline is not keeping up |
| `prep` | prompt + JPEG re-encode + base64 + JSON serialisation | if `jpeg` dominates, tune `upload_max_dim` |
| `connect` | TCP + TLS handshake | **0 means the long-lived connection was reused**; a constant 1-2s means keep-alive is not working |
| `http` | the whole `perform()` call | subtract `connect` to get server-side inference time |
| `parse` | parsing the response JSON | should be single-digit ms |
| `sdlog` | writing the task log to disk | large means the SD card is slow |
| `exec` | executing the HID actions | high on steps that type a lot |
| `sleep` | `sleep_before_next` | floored at 1s |

> `connect = 0` is the sign that the long-lived connection is in effect. If it stays non-zero, the server is closing idle connections early; the client then rebuilds the connection automatically on the next request.

### Input method (IME) — the first suspect when typing fails

On a target machine with a Chinese IME, Latin letters typed in Chinese mode go into the **composition buffer** instead of the target control. Ordered by actual harm:

| Symptom | Cause |
|---|---|
| **Digits disappear** | while composing, **the number row selects candidates**. `Vicrl-7391-A` becoming `Vicrl-A` is exactly this — **letters and punctuation survive, digits do not**. This is the most frequent case in practice |
| **Spaces disappear** | in every Chinese IME, Space is a **candidate-selection / word-segmentation key** and produces no literal space. `Hello World!` becomes `HelloWorld!` |
| **Letters disappear** | they are consumed as part of pinyin parsing, or eaten by a letter candidate-selection key |
| **`press "enter"` seems to do nothing** | while composing, **the first Enter only commits the pinyin**; a **second Enter** is needed to press the dialog's default button |

#### How to tell it is the IME and not the device

The status line carries a line of objective evidence:

```
last type: "Vicrl-7391-A" - device queued ALL 12/12 key events, so any
missing/different text on screen was caused by the target machine
```

**As long as it says `ALL N/N`, the device really did send every keystroke**; if the text on screen is wrong, the target machine dropped it. The matching line in the serial log, `typed 12/12 chars (skipped 0): "Vicrl-7391-A"`, is the same evidence. Once you see this conclusion, **do not retry the same thing** — a retry is guaranteed to fail again.

#### The key point: the Chinese/English toggle is a **parity you cannot observe**

In practice the agent made this step **worse the more it tried to fix it**, not because it was not smart enough, but because the prompt at the time taught it wrong:

```
2. press "shift" —— MS 拼音中英切换；无效则试 win+space、ctrl+space   (MS Pinyin Chinese/English toggle; if that has no effect, try win+space or ctrl+space)
```

**In Microsoft Pinyin, `Shift` and `Ctrl+Space` toggle the same state.** The model pressed shift (→ English) and then ctrl+space (→ Chinese); **two toggles are no toggle at all**, and then it typed the string again in Chinese mode. Listing two equivalent operations as "alternatives" is teaching the model to cancel out its own parity.

**But the two keys differ enormously in reliability** (from real Windows 11 experience, now written into the prompt):

| Toggle key | Precondition | Reliability |
|---|---|---|
| `Shift` | **requires a text caret** | low — it does nothing without focus, **and a pending composition box swallows the Shift outright**, so it never gets a chance to toggle |
| `Ctrl+Space` | **ignores focus** | high (about 99%) |

> This explains directly why "shift does not work" was observed in practice: at the time of that failure **the composition box was open** (the log shows `'1 A'` hanging on the line). So `Shift` is **doubly unreliable** in this scenario — it requires a caret, and it gets swallowed by an uncommitted composition. **The first choice should be `Ctrl+Space`.** My original prompt had the priority backwards.

**The role of `escape` also needs stating clearly**: it does not change the Chinese/English mode, but it **must be pressed first** — while a composition is pending, the toggle key gets swallowed, so the order is `escape` → `ctrl+space`.

#### The probe turns an "invisible parity" into an "observable state"

This is the core of the whole solution. **The device cannot read the IME mode, but it can read whether the digits survived**:

1. `press "escape"` then `press "ctrl+space"` (they can be in the same batch)
2. type the 2-character probe `a1` **into the very control you intend to use** (`clear_first: true`)
3. read **the character count the application itself reports** (Notepad's status bar says "2 个字符")
- `a1` → the mode is correct
- `a` alone → still Chinese
4. only if the probe comes through intact, clear it and type the real string
5. if the probe fails, **toggle exactly once more** and probe again; **if both attempts fail, this machine needs a different channel** — stop toggling

The status line reports `IME toggles sent: N`. **N ≥ 2 is hard evidence that toggling is not working**, and the model no longer has to guess.

> **Design note**: the parity is dangerous because it is **invisible** — every step could be right or wrong, so the model can only guess. Add a probe and the same parity becomes a **measurable state**, and the loop converges on its own. That is far more useful than "hand it a few more toggle keys".

#### But the probe lies too: it must be its own step

A real run (the 22:54 round) hit this trap, and it **cost a lot**:

```
#4 action: press "ctrl+space" type "a1" clear_first     ← toggle and probe in the same batch
#5 action: press "escape" press "ctrl+space" type "a1" clear_first
#6 action: press "shift" type "a1" clear_first
#7 action: type "<50 词纯字母故事>"                       ← no toggle, and it succeeded
```

**The story in step 7 kept even its spaces.** And in Chinese mode Space is a candidate-selection key — if the mode were still Chinese the story would inevitably have been destroyed. **So the `shift` in step 6 did in fact toggle successfully**; the probe in the same batch was simply sent **before** the toggle took effect, and therefore produced a **false negative**.

| Conclusion | |
|---|---|
| Mode switching takes effect **asynchronously on the host side** | keys that follow the toggle inside the same batch may still be interpreted under the **old mode** |
| A false negative is worse than "no probe" | it sends you off to change a channel that **never needed changing**, burning two round trips every time |
| **Fix** | the probe **must be its own step** (one model round trip ≈4s, enough for the toggle to land); the prompt now says "after toggling, **stop right there and send nothing, not even the probe**" |
| Firmware safeguard | after sending the toggle keys, force `vTaskDelay(150ms)`, which covers the case where "there are other actions in the same batch" |

> This also shows that **the probe is a double-edged sword**: it turns an unobservable state into a measurable one, but if the **timing of the measurement** is wrong it reports something correct as wrong. The probe's **timing** matters as much as the probe itself.

#### A side finding: only digits are eaten; letters, spaces and punctuation are safe

The 50-word story from step 7 (still in Chinese mode) came through with **every letter, space and punctuation mark intact**. So when **the agent decides the content itself** (a story, a sample message, a placeholder name, a test string), **content without digits sidesteps this failure mode entirely** — far cheaper than wrestling with the IME. But **values the task specifies cannot be changed**; those are the goal itself.

#### Cleaning up text that was already typed: what each key destroys

**While the candidate window is open, the composition string has not entered the document yet.** That line decides everything:

| Action | Scope | Risk |
|---|---|---|
| `escape` | **discards only the composition string, never touches the document** | with no composition string it passes through to the application → **closes the dialog, cancels the rename** |
| `enter` | **commits** the composition string (it really does go into the document) | once written it has to be deleted again, adding one more destructive step |
| `backspace` | while composing it edits the **pinyin buffer**, deleting one letter at a time and inserting nothing | only after a commit does it delete real text. **Do not rely on it, and do not use it to count keystrokes** |
| `ctrl+z` | undoes the last edit | one key, but **the scope is not yours to control**: applications group edits differently and it may undo earlier good content as well. After using it you must **look**; if you undid too much, `ctrl+y` restores it |
| `"clear_first": true` | `ctrl+a` = **the whole field** | correct on a blank document; **if the field already holds text typed earlier in this task it silently destroys it** |
| `"replace_chars": N` | `shift+left` × N = **exactly the last N characters** | precise and local. N can be computed from the application's own character count (now − before typing) |

> **This is why neither "backspace" nor "enter + ctrl+z" is the first choice**: `escape` is the only cleanup action that **does not modify the document at all**. `enter` really writes the wrong text in, so it must then be undone, and the scope of the undo is not yours to control. `backspace` cannot reach the document at all while composing. **Two instruction defects have been fixed** (both stem from the table above):
>
> 1. The old prompt said "**always press escape first**" — with no composition string, escape passes through to the application, and in dialog / rename scenarios it is destructive. It now says "press escape first **only when the candidate window is visible**".
> 2. The old prompt had only `clear_first`, and that is **the whole field**. In a genuinely multi-step task (type line 1, then line 2, then `clear_first` after line 2 fails) it wipes line 1 as well. `replace_chars` has now been added — an incremental, local replacement.

#### Measured channel results (2026-09-11)

| Channel | Method | Result |
|---|---|---|
| Main keyboard number row | default `type` | ❌ `Vicrl-7391-A` → `Vicrl--A`, **only the 4 digits disappeared**; the letters and both hyphens all survived |
| **Numeric keypad digits** | `"digits_numpad": true` | ❌ **eaten just the same**. `locks=num=on`, `queued=12/12` and the `keypad` channel marker were all present, and the result was still only 8 characters |

> **The numeric-keypad hypothesis has been falsified; do not try it again.** The hope was that "the IME only hijacks the main number row and lets the keypad through", but Microsoft Pinyin treats keypad digits as candidate-selection keys **too**. `digits_numpad` is kept because other IMEs (Sogou, QQ and so on) may differ, but on Microsoft Pinyin this is not an intermittent failure, so retrying is pointless. That this could be confirmed at all comes down to the `type` action summary now recording the **channel** (`type "..." clear_first keypad`) and the status line's `locks: num=on caps=off`. Before that, a log line reading `type "Vicrl-7391-A" clear_first` **could not distinguish** "the keypad was eaten too" from "the keypad was never tried" — **an experiment that does not record which variant it ran is not an experiment.**

**One case has not yet been tested cleanly**: whether digits land after `escape` + `ctrl+space` (once). The earlier attempt put escape + shift + retype in the same batch and then pressed ctrl+space again (a second flip of the same state), so that "does not work" conclusion is contaminated. **This is the most valuable next experiment** — if it works, the whole problem reduces to "`escape` → toggle once → verify with a probe", which would fix typing in **any** application, not just this one string.

#### Two failures that look alike, opposite fixes

This is the step where debugging most easily goes wrong:

| Symptom | Discriminator | Real cause | Fix |
|---|---|---|---|
| The text never appeared | `screen ... NO - identical` / `[NO SCREEN CHANGE]` in the action summary | **focus is not on the target control** (another window or the operator took it) | `click` once inside the target control and retype. **Toggling the IME cannot fix this** |
| The text appeared but is wrong | the screen changed, or the application's character count changed | **the characters were reinterpreted by the target** (IME / keyboard layout) | work down the channel ladder above |

> This was hit in practice: the operator clicked the taskbar language indicator to switch IMEs by hand, focus left Notepad, and the result was `queued=12/12` while the edit area read `0 个字符`. This is **not** the IME eating characters, but the prompt at the time conflated the two, so the agent went off toggling the IME over what was a focus problem.

#### Workarounds that do not depend on the input mode

The ordering is deliberate: **item 1 fixes "typing in any application"; the rest only fix this one string.**

1. **Get the mode right first, and verify it**: send a single `press "shift"` on its own, then **look**, then read the application's character count with the `a1` probe. If `a1` arrives intact the mode is right and everything after it is fine. `press "win+space"` opens the IME switcher overlay, listing every installed IME with the current one highlighted — that is a **large, readable** overlay, and on many machines it is the only way to see the state at all.
2. **Do not type into the GUI at all.** If the string only needs to land in a file, use `run`: `{"action_type":"run","command":"cmd /c echo Vicrl-7391-A > C:\\p.txt"}` — one action, no editor. To look at it, open it **read-only** with `run "notepad C:\\p.txt"`.
3. **Go through the clipboard.** Put the value on the clipboard and paste it — `ctrl+v` is a single atomic event that no IME can alter: `run "cmd /c echo Vicrl-7391-A|clip"` → `press "ctrl+v"`
4. **Report the block honestly**: finish with `done:false`, stating the IME behaviour, which channels were tried, and what the operator can change.

> **Still to test**: whether the Run dialog (`explorer.exe`'s Win+R) preserves digits. The clipboard workaround depends on it doing so, and this **has not been tested yet**. The discriminator is clean: if it is eaten too, the command becomes `cmd /c echo |clip` and what gets pasted is `ECHO is on.` — that output is the answer in itself. **What the operator can change** (not the only solution, but the least effort): **add an English keyboard** to the machine (`Settings → Time & language → Language & region → Add a language → English (United States)`). That gives `win+space` an **observable switcher overlay**, turning the toggle from an "invisible parity" into a "state you can read off". Microsoft Pinyin does not need to be removed.

### Typing and the `run` action

**Typing is the control channel, not a performance metric.** A single dropped character costs a whole model round trip plus a string of recovery actions, far more than the tens of milliseconds saved. So typing is **deliberately humanised**, with the rhythm controlled by the device:

```json
"hid": {
  "type_hold_ms": 15,          // how long a key is held down
  "type_gap_ms": 35,           // base gap between keys
  "type_jitter_ms": 25,        // random jitter on each gap (avoids a mechanical rhythm)
  "type_word_pause_ms": 120,   // extra pause after a space (simulates moving to the next word)
  "run_dialog_ms": 500,        // from Win+R until the dialog accepts input
  "run_settle_ms": 150         // from the end of typing until Enter
}
```

**If characters are still being dropped, raise `type_gap_ms` first.**

#### HID endpoint polling interval (important)

The endpoint polling interval in the descriptor was once **10ms**, meaning the host could take at most one keyboard report every 10ms. Typing at the time was 8ms down + 4ms up — **both the press and the release fell between two polls and the host never saw them**, so characters were dropped periodically (`notepad` became `ntpd`, losing positions 2, 4 and 6). It is now **1ms**.

The return value of `tud_hid_keyboard_report()` used to be ignored as well — when the endpoint was busy the report was **silently dropped**. Every send in `hid_reports.c` now calls `hid_wait_ready()` first and waits for the endpoint, retries once on failure, and reports it with `ESP_LOGW`.

#### `run` — one action to launch a program

`run` does Win+R → type → Enter by itself, with the internal delays set correctly:

```json
{"action_type":"run","command":"notepad","wait_after":1.5}
{"action_type":"run","command":"cmd /c echo hi > C:\\test.txt"}
{"action_type":"run","command":"powershell -NoProfile -Command ..."}
```

In field logs the model repeatedly got stuck in "close the file-not-found dialog → reopen Win+R → select all → retype", each step a full round trip. `run` turns that whole sequence into one atomic action.

`run` **also self-checks**: after pressing Enter it waits 700ms and takes a frame fingerprint; if the screen **has not changed at all** it presses Enter once more (a bounded retry, exactly one). That covers two real failures at once — the Run dialog's autocomplete drop-down swallowing the first Enter, and the first Enter under an IME composition being used only to commit. If the second press still changes nothing it logs `ESP_LOGE` saying the command may not have run.

`type` gained `clear_first` (press Ctrl+A first), the standard way to correct a wrong field — far more reliable than having the model assemble "click → select all → retype" itself.

### How to verify mouse accuracy

There is no need to guess whether this is an offset problem. The status line already reports the cursor's actual position; just compare:

1. have the model `move` to the centre of some `box_2d`, say the normalised `(0.500, 0.500)`
2. check `cursor=(...)` on the status line

If `cursor` matches the request → **the mapping chain is fine and the accuracy problem is the model's visual localisation**. If it deviates systematically → check that `output_width/height` in `config.json` really is the target computer's resolution (it determines the `pixel_to_abs` mapping). At DEBUG log level you also see `move px(...) -> abs(...) [screen WxH]`, and the three values can be checked one by one.

> A note: if the target machine has "enhance pointer precision" (mouse acceleration) enabled, it has no effect on an **absolute-coordinate** device, so that is not a suspect.

### Adaptive reasoning effort (the biggest latency lever)

Measured across steps 1-5, `http` accounted for **62–85%** of per-step wall clock (2653–6191ms), while `reasoning_tokens` were 30–50% of the output. In other words, **making the model think hard on every step is the largest avoidable cost**.

```json
"api": {
  "reasoning_effort": "low",            // normal steps
  "reasoning_effort_escalated": "high", // when stuck
  "effort_escalate_after": 1
}
```

**"Stuck" is an objective determination**, and the harness already knows:

- the previous action **produced no screen change at all** (identical frame fingerprint)
- the same action type repeated ≥ 4 times in the last 15

Either one increments a counter; after `effort_escalate_after` consecutive hits the effort escalates to `high`, and **the moment there is visible progress it drops straight back to `low`**. The serial log marks the setting currently in use:

```
Step 6: ... | effort=high (escalated) | stuck=2
```

The WebUI Dashboard also shows the current setting and the number of stuck steps. To turn adaptivity off, set the two values equal.

### Output tokens are the bill: give text fields a budget

Measured output speed is about **116 tok/s**, so a step's wall clock is essentially "output tokens ÷ 116". And by default the model writes verbosely — and that text **gets truncated when it goes into the trajectory**, so the extra words are paid for and then thrown away.

The system prompt now states explicit budgets (`observation` ≤240 characters, `self_evaluation` ≤200 characters, `plan_update.summary` one sentence). Measured effect:

| | Before | After |
|---|---|---|
| Step 1 output tokens | 539 | **224 (-58%)** |
| Step 1 API time | 4639ms | **3476ms (-25%)** |
| Step 2 output tokens | 568 | **324 (-43%)** |
| Step 2 API time | 3854ms | **3289ms (-15%)** |

The same prompt addition also states that `plan_update` should **not restate the goal, the status line or the previous plan**.

**The other half is ambiguity in the task itself.** In the same test round, step 3 took **13643ms** because `reasoning_tokens` spiked to **2326** (88% of the 2635 output) — that step's instruction contradicted itself on "whether steps 3/4/5 can be merged", and the model weighed it back and forth for a dozen rounds inside its reasoning. Note that `effort` was still `low` at that point: **the effort setting cannot contain reasoning inflation caused by ambiguity**. Writing the instruction clearly is far more effective than turning a dial.

### The JPEG pipeline: read the breakdown first, then decide about PPA

The ESP32-P4 has a **PPA (Pixel Processing Accelerator)**, and `ppa_do_scale_rotate_mirror()` does **crop + scale + rotate + mirror** in one call (`in.block_offset_x/y` + `block_w/h` are the crop, `scale_x/y` is the scale). The documentation states outright that *"the size of the entire picture has no influence on the performance"* — **PPA's runtime is proportional only to the size of the cropped block**, not to the source image. In theory that maps exactly onto our ROI operation.

**But on the path we actually measured it is useless**, for a reason that is right there in the code:

```c
resample_region() has a fast path
    if (dw == cw) { row-by-row memcpy }
```

An ROI request uses `scale=1`, so `cw == dw == 960` and **it takes exactly this memcpy path** (540 rows × 2880 bytes ≈ 1.5MB ≈ 2ms). The part PPA wants to replace was never the bottleneck.

So where does `jpeg=179–207ms` actually go? **The old `compress_ms` was one opaque total**, so this version splits it into four parts:

| Field | Meaning |
|---|---|
| `setup` | codec engine / buffer creation (**cached, so ≈0 in steady state**) |
| `dec` | hardware full-resolution decode |
| `rs` | software crop + box scaling (**the only part PPA could replace**) |
| `enc` | hardware re-encode |

Both the `Timing(ms)` line in the task log and the WebUI latency bar chart show these four parts.

**And a more substantial cost was fixed along the way**: every step used to rebuild the entire codec chain —

```c
jpeg_new_decoder_engine()              // every step
jpeg_alloc_decoder_mem(1920*1080*3)    // every step, 6.2MB
jpeg_decoder_process()
jpeg_del_decoder_engine()              // every step
free(6.2MB)                            // every step
... and the same again for the encoder ...
```

The capture card's resolution is constant for the whole run, so **all of this can be amortised**. The decoder engine, the decode buffer (6.2MB), the encoder engine and the scratch buffer used for cropping (grow-only) are now cached in `jpeg_utils.c` and rebuilt only **when the frame geometry changes**. The codec memory is guarded by one mutex (there is only one caller today, but adding a second preview path would corrupt the state).

> **Where PPA actually earns its place**: the two paths `scale=2/4` (whole-frame downsampling) and `upload_max_dim>0` use per-pixel box averaging with no memcpy fast path, and that is where the software bottleneck lives. In other words, **PPA's value is "making downsampling cheap"** — cheap enough that `upload_max_dim` is worth turning on, which brings the full-frame step's uplink from ~407KB down to ~180KB with **no change at all in what the model sees** (the server scales to ~1300×731 anyway). Both preconditions are confirmed: flash encryption is **off** (so PPA's "SRM is unavailable with external PSRAM + flash encryption" restriction does not apply), and the L2 cache line is 64B (PPA's output-buffer alignment requirement). Separately, `scale_x/scale_y` precision **is truncated to 1/16**, so PPA is not suitable for exact pixel-level scaling, but it is more than good enough for "shrink it to a size the model can see".

### Uploaded frames: `upload_max_dim = 0` means leave them alone

**This is the currently recommended setting.** The MJPEG the MS2109 outputs is already a complete JPEG and the model side scales it anyway, so decoding and re-encoding it is pure waste.

| Value | Behaviour | Cost |
|---|---|---|
| `0` | **pass the MJPEG straight through**, no decode, no re-encode | cheapest; about 150–190KB uploaded per step |
| `>0` | decode → software box downsample → re-encode | full-resolution decode + ~6MB PSRAM + software scaling, in exchange for the upload dropping to about 60KB |

> ⚠️ **Two traps with `>0` (both fixed)**: the ESP32-P4's hardware decoder **does not scale** and always outputs the source resolution. The old code allocated the decode buffer at the *downsampled* size, so every decode failed with `JPEG_ERR_NO_MEM (258)` and silently fell back to the original frame — `max_dim` never took effect. More dangerous still: even with a large enough buffer, passing the downsampled `width/height` to the encoder while feeding it full-size RGB makes the encoder read only a **crop from the top-left corner**; the model then sees a frame missing its bottom-right and every normalised coordinate is shifted. The decode buffer is now allocated at the source size (with 16-byte alignment) and the downsampling happens in software.

### Device profile

A profile is a short document **about this machine**, and it is **resent in full** on every step's request, so the longer it gets, the higher the token cost and the noise per step. The default file is `sdcard/profiles/win11_laptop.md`, and the repository ships a starting version (OS / resolution / IME and toggle keys / taskbar / common shortcuts).

**The correct use of `profile_updates`**: write to it only when you learn a machine-level fact that "will still hold next week under a different task". **The overwhelming majority of steps should carry an empty array `[]`.** Rule 11 in the system prompt gives clear positive and negative examples.

Things that do not belong in a profile (all of these really appeared in logs):

```
- This machine's Notepad opens with an empty '无标题' document; status bar shows
  line/column and character count, useful for verifying typed text.
- Notepad status bar shows caret line/column and character count — quick way to
  verify exactly typed content.
- Notepad status bar shows character count and caret line/column (e.g. '行 1, 列 7
  12 个字符'), useful to verify typed text length.
```

These three are **three phrasings of the same fact**, and all of them are things "you only learned by opening Notepad during this task" — they are not machine records. They belong in `plan_update` / history, not in the profile.

The harness now has three lines of defence:

| Defence | Behaviour |
|---|---|
| Prompt rule 11 | lists positive and negative examples explicitly, and requires an empty array by default |
| **Similarity deduplication** | compares against every existing line after normalisation; an exact match or ≥70% word-containment skips the write and logs it (aimed squarely at the rephrasings above) |
| **Size cap** | refuses to append once the file exceeds 4096 bytes, and warns |

> If an earlier run left a misnamed `.md` file on your machine, just delete it from the WebUI file page.

## Sessions (conversation windows)

Each **session** is a separate context window: its own task goal, plan, and per-round **trajectory**.

```
/sdcard/sessions/<id>/
    meta.json        title, goal, creation time, step count
    plan.json        the plan and milestones for this task
    trajectory.txt   the per-round trajectory — the model's memory
```

- **Start with a task** → creates a new conversation window
- **Click a row on the sessions page** → switches the current context (allowed only in IDLE, so the goal/plan cannot be swapped out from under a running task)
- **Continue** → loads that session and starts running, counting steps on from where it left off
- The right-hand side of the sessions page lets you **edit the trajectory** directly, for example deleting a few steps that went off track; the change takes effect from the next step after saving

### Why the trajectory holds no historical screenshots

This is the most important difference between this project and "generic multi-turn agent history". You are right that **the prefix cache does make historical tokens nearly free** (a hit costs $0.006/M against $0.30/M for a miss, a factor of 50; measured at step 20, full history cost about the same as the current scheme, because the output is what dominates).

**But the cache does not solve byte counts.** One base64 frame is about 160KB, so 20 steps of history is **3.2MB, and it has to be retransmitted on every step**:

| | Current | With historical screenshots |
|---|---|---|
| Request body per step | ~160–200KB | 3.2MB (step 20) → 8MB (step 50) |
| Device-side JSON construction | ~40ms | ~0.8s |
| WiFi uplink | ~0.2–0.5s | seconds, growing linearly |
| Decision value | — | **zero: the screen changed long ago** |

So the design is: **keep the entire text trajectory** (nothing is trimmed until 24KB, and then only the earliest steps; the cost is negligible) and **keep not one historical screenshot**. Each step's request contains only the current frame.

### What the trajectory looks like

```
#4 [18:31:58] action: click(left,0.450,0.450) type "Hello World!" clear_first
   saw: Notepad is open (title '无标题') ... status bar '行 1, 列 1  0 个字符'
   result: Partially worked: text typed but the space was dropped (11 chars)
   plan: Notepad is open and empty. Click into the editor and type ...
```

Two key differences from before the change:

- **It carries parameters**: `click(left,0.450,0.450)` rather than just `[click]`. The model can now compare the coordinates it sent against the cursor's actual position on the status line and judge for itself whether it clicked off target.
- **It carries "what was seen" and "how it turned out"**: previously there was only a `plan_update.summary` line, and the model could not look back at the evidence behind its own earlier decisions.

### Division of labour with the SD card task log

| | Trajectory (enters context) | Task log (does not enter context) |
|---|---|---|
| Location | `sessions/<id>/trajectory.txt` | `log/<id>/<id>.log` + per-frame JPEG |
| Content | per-round action parameters / observation / result / plan | the complete raw API response |
| Purpose | the model's decisions | humans debugging problems |

This is what chapter 2 of the book calls "**isolation over compression**": bulky intermediate information never enters the main context at all.

### On the compiler optimisation level

Currently `sdkconfig` has:

```
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y          # -Og + assertions
CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_SIZE=y   # -Os
```

mbedTLS, cJSON and base64 are all CPU-intensive and are several times slower under -Og than -O2. Switching to `CONFIG_COMPILER_OPTIMIZATION_PERF` (`idf.py menuconfig` → Compiler options → Optimization Level → Performance) plus `CONFIG_MBEDTLS_COMPILER_OPTIMIZATION_PERF` is two switches, and **the recommendation is to baseline with the timing data above first, then flip these switches once and compare**, so that "the effect of the optimisation level" and "the effect of the code changes" do not get mixed together.

---

## View (ROI scaling) and screen preview

### Why ROI is not just about "saving bandwidth"

The key is the server's scaling rule: **an image with fewer than about 544×544 pixels is scaled up**, and a larger one is scaled down to a total of roughly 1300×1300. So sending the full screen and sending a crop yields **different effective resolutions**:

| What is sent | Source pixels | After server processing | A 12px control as the model sees it |
|---|---|---|---|
| Full screen 1920×1080 | 2.07M | scaled to ~1.69M (≈1734×975) | **~10.8px (cannot be read reliably)** |
| Crop 480×270 | 130K | **scaled up** to ~296K (≈725×408) | **~18px** |
| Crop 256×144 | 37K | scaled up to ~296K | **~34px** |

The token cap is fixed at ~1024, but **the magnification you buy goes from 1.0× to 1.5–2.8×**, and the enlargement is free on the server side — the device only has to crop, not interpolate. This addresses the "imprecise mouse" problem confirmed earlier head-on: the mapping chain is fine, the model simply localises poorly on small text.

### How the model uses it

An optional field in the response applies to the **next** screenshot:

```json
"next_view": {"roi": [0.30, 0.25, 0.55, 0.60]}   // ymin,xmin,ymax,xmax, normalised to the full screen
"next_view": {"scale": 2}                         // full screen, scaled down by half
"next_view": {}                                   // back to the full screen
```

The prompt explicitly requires the model to request a zoom **before clicking a small target, before reading small text, and after a click that hit nothing**.

**`next_view` is sticky**: not sending it keeps the current view, and `{}` is the explicit return to the full screen. It was first implemented as **one-shot** (restarting from FULL every step), so on any step where the model did not repeat it the view silently fell back to the full frame — a state change the model never asked for, and in practice it noticed, was confused for a long while, and wrote a long stretch of reasoning to explain it. The prompt's own sentence, "to operate outside the ROI, send `{}` to return to the full screen", **presupposes stickiness**; the code and the prompt disagreed at the time and are now unified on sticky.

Sticky is also the cheaper choice — measured data (1920×1080 full frame vs ROI 0.25–0.75):

| | Full frame | ROI |
|---|---|---|
| `prep` | 318–358 ms | **259 ms** |
| of which crop / re-encode | 0 | 197 |
| of which base64 | **142–157** | **25** |
| Uplink `Req` | 406 KB | **87 KB** |

Crop + re-encode costs 197ms, but because the payload is 4.7× smaller, the base64 item alone saves 120–130ms back, so **the net cost is almost nothing and you get 320KB of uplink for free**. So there is no cost to worry about in "sticking" to a view.

### Coordinate mapping: the one place that can go wrong

**`box_2d` is always normalised against the image the model saw.** The first field of the status line states the current view:

```
view: FULL | step 7/100 | ...
view: ROI y0.300-0.550 x0.250-0.600 scale=1 — box_2d is normalised WITHIN this view | step 8/100 | ...
```

The harness does the inverse, and **one function** does it (`box_to_pixel` in `action_executor.c`):

```c
full_x = roi_x0 + view_x * (roi_x1 - roi_x0);
```

Two deliberate design decisions:

- **`scale` needs no coordinate handling at all** — normalised coordinates are resolution-independent, and only an ROI moves the origin. Hold that line and a systematic offset has exactly one possible source.
- **Coordinates in the trajectory are recorded in global coordinates**, with a separate `look=` marking which region was being viewed. That keeps the history readable across view switches:
  ```
  #5 [18:32:05] look=ROI(y0.30-0.55 x0.25-0.60 s1) | action: click(left,0.450,0.420)
  ```

### Screen preview `/api/snapshot`

| Request | Returns |
|---|---|
| `GET /api/snapshot` | **the exact image the model received** (already cropped / scaled per `next_view`) |
| `GET /api/snapshot?live=1` | grab a frame straight from the capture card (live picture) |

The WebUI Dashboard has a preview panel showing the dimensions (for example `模型看到的那张 · 725×408`). **This is the most direct way to verify ROI** — without it, whether the crop region is right is pure guesswork.

### Cost

The hardware decoder **can neither crop nor scale**, so every non-full-screen view requires:

```
decode the whole frame (6.2MB RGB, ~150-250ms) → crop + software resample → encode the small image (~50ms)
```

But **this item cannot be read in isolation**. The same-step comparison above (see the table): ROI's `prep` totals 259ms, actually **less** than the full frame's 318–358ms, because the base64 item alone drops from ~150ms to 25ms. The 197ms of cropping is cancelled out by a larger gain.

So the net balance is: **ROI costs less device time than the full frame while saving 4.7× of uplink**, and it buys 1.5–2.8× effective resolution on top. At full frame with scale=1 the copy-free fast path is taken and this cost is not paid at all.

> **Two things are worth watching when measuring**: whether the preview image's dimensions are what you expect (the `?last` one), and whether the `cursor` reading on the status line agrees with the coordinates the model sent. If it deviates systematically, the problem is in the `box_to_pixel` mapping — that is the only place coordinates are changed. **This has been measured once**: the model sent `box_2d [0,0,0,0]` under `view: ROI y0.250-0.750 x0.250-0.750`, and the status line came back `cursor=(0.250, 0.250) px=(479,269)`. Expected (0.250,0.250), **hit exactly**; the 1-pixel difference in `px` is an unavoidable consequence of quantising HID absolute coordinates (`(480*32767)/1920 = 8191.75`, truncated to 8191, read back as `8191*1920/32767 = 479.99`).

---

## need_screen v2 and the status line

### The status line

The start of every user message carries a `**Status:**` line generated by **code**, not by the model:

```
step 7/100 | cursor=(0.523, 0.411) px=(1004, 444) | held buttons: none |
screen since last screenshot: NO - identical to the screenshot you last saw |
consecutive actions without a new screenshot: 0/3 | recent actions: click x6 of last 15
```

Three pieces of information the hardware holds and the model cannot obtain on its own:

- **`cursor`** — `hid_device` knows the pointer position exactly. The pointer is often hard or impossible to make out in a screenshot, so after a bare `move` with no click the model would otherwise have no idea where it left the cursor.
- **`screen ... NO - identical`** — an objective answer to "did my last action actually do anything". The prompt asks the model to self-assess, but a pixel-level answer is more reliable than its guess; "no change" is the clearest possible signal that a click hit nothing. `recent actions: click x6` is the counter's way of saying "you are stuck".
- **`held buttons`** — the `{pressed_buttons}` in the prompt used to be hard-coded to `[]`; the real state is now reported.

### Image reuse (lossless)

Every step fingerprints the captured frame with FNV-1a. If the fingerprint exactly matches the **previous frame sent to the model**, the JPEG bytes are identical, so the base64 encoded on the previous step is reused directly — skipping compression and base64 (about 400ms). This is **provably lossless**: same fingerprint ⇒ same JPEG ⇒ same base64, so there is no approximation risk of "using a stale image" whatsoever. The log shows:

```
Screen unchanged since last query (182344 bytes); reusing encoded payload
```

### The new meaning of `need_screen: false`

**The harness always captures a fresh frame** — deciding the next action from an old screenshot is an approximation, the screen may already have changed, and it is not worth risking that to save ~35ms.

So the meaning of `need_screen: false` narrows to a much safer request: **"I am only waiting; do not ask me again until the screen moves."** All of these must hold:

1. every action in the batch is a `wait` (any single click/press/type/drag and it does not hold)
2. the model sent `need_screen: false`
3. the frame fingerprint proves the screen **still** matches the last one it saw
4. the consecutive-skip count is < `agent.max_blind_rounds`

When they hold, the API call is skipped outright, and after `wait_settle_ms` the comparison is made again; the moment the screen changes, asking resumes. `agent.allow_wait_skip` defaults to **false**.

> **Why it is off by default**: this path depends on the premise that the frame fingerprint does not change while the picture is static. The MS2109's MJPEG output is **expected** to be byte-identical frame to frame on a still image, but that has to be confirmed on real hardware. Before enabling it, check whether `Screen unchanged since last query` appears consistently in the log — if it never appears on a static picture, the encoder is jittering and this should not be enabled.

### New observations on the action space

The model no longer has to guess the cursor position, and no longer needs `need_screen` to express "I want to chain actions" — the harness no longer overwrites `need_screen` (the old code forced it to 1 after click/press/type/drag, in direct contradiction of the prompt's "chain actions aggressively").

---

## Notes

- **MS2109 capture card**: must be connected to the USB Host port (OTG1, GPIO18/19)
- **USB HID output**: connected to the USB Device port (OTG0, GPIO26/27)
- **SD card**: must be FAT32, mounted at `/sdcard`
- **C6 co-processor**: requires the ESP-Hosted firmware to be flashed in advance
- **Memory**: SPIRAM is used heavily (frame buffers, Base64 encoding, HTTP responses)
- **RTC time**: synced over NTP, timezone UTC+8
- **Device profile**: the default profile is `win11_laptop`, and you must place `sdcard/profiles/win11_laptop.md` yourself; when it is missing the model receives only a one-line placeholder and operating accuracy drops noticeably
- **Wheel**: only vertical scrolling is supported (`delta_y`); the report descriptor declares no horizontal scrolling field, so `delta_x` is ignored and a warning is logged

### One character takes down the whole task: JSON must be valid UTF-8

A trap hit for real, worth recording on its own. The symptom: under a certain Chinese task goal, every step was

```
Step 1 | Req: 0B | Resp: 0B | API: 19470ms | Exec: 0ms | parse=0
Response: no response
```

Three times in a row, identical, and then `fail_count` tripped the abort. It looked like a network or authentication failure.

**Root cause**: to silence `-Werror=format-truncation`, fixed-length copies had earlier been changed uniformly to `snprintf(dst, n, "%.*s", n - 1, src)`. But **`%.*s` takes its precision in bytes, not characters** — a Chinese character is 3 bytes, so a truncation point landing mid-character produces an **invalid UTF-8 byte sequence**.

And JSON in an HTTP request body **must be valid UTF-8**; if the server cannot decode it, it returns 400 outright. So:

| Observation | What it actually means |
|---|---|
| `http=891~2542ms`, `connect=406ms` | rejected about 500ms after connecting — a **fast failure**, not a timeout |
| all 3 retries failed, 19.5s total | 2000+4000+8000 backoff + ~1.3s each; the numbers match exactly |
| `Req: 0B`, `parse=0` | parsing failed so there is no metadata to read; this is a **result, not the cause** |
| `prep=243 (b64=122 json=99)` | the request was constructed fine — so the problem is in the **content**, not the flow |

That task goal was 500+ bytes, while `SESSION_GOAL_LEN` was 192 and `ctx->task_goal` 256, so **both cut it mid-character**. The earlier "open Notepad, type Hello World!" was only about 40 bytes and never reached the boundary, which is why it stayed hidden.

**The fix (two layers)**:

1. `components/storage_manager/utf8_util.h` provides `utf8_copy()`, which truncates on **character boundaries**. Every fixed-length copy of user- or model-facing text now uses it.
2. In `cloud_client.c`, after `cJSON_PrintUnformatted()` the **entire request body** gets one pass of `utf8_sanitize()`, replacing any illegal bytes that slipped through with `?`. This is the single exit point for all requests, so the invariant is **enforced** rather than "trusting that upstream got it right".

**The same trap, a second time**: with both of the above fixed, requests were normal (`Req: 404138B`), but the model went `done: true` right after step 1, writing in its own reasoning:

> the goal text ends with "?" which suggests the task description may be truncated

The cause was further upstream: `handle_api_start()` in `web_server.c` had

```c
char task[256] = {0};
strncpy(task, t->valuestring, sizeof(task) - 1);   // cuts by byte, and silently
```

The task goal was cut to 255 bytes **as it entered the HTTP layer**, and the 512 limit I had raised it to in `agent_start()`/session was never reached. The later steps of a multi-step Chinese instruction were chopped off, and the agent **confidently completed the shorter task** — far more dangerous than a crash, because there is no error signal at all.

So the rule now is: **when a user's task goal is too long, error out; never truncate.**

```c
if (strlen(t->valuestring) >= sizeof(task)) {
    return send_error(req, 400, "task too long: N bytes, max 1023 (split it into steps)");
}
```

`SESSION_GOAL_LEN` is now 1024 (about 340 Chinese characters, enough for a multi-step instruction), and `agent_core.c` carries `_Static_assert(sizeof(agent_ctx_t.task_goal) == SESSION_GOAL_LEN)` — that assertion **really did stop a build** during this change (when the two are out of sync there is bound to be a silent truncation somewhere).

**Debugging advice**: when every step fails and fails fast, look at the **serial log** first (the task log has no HTTP detail) for `HTTP error ... status=400` and `Request body: N bytes`. After the fix there is one extra line, `Request body had N invalid UTF-8 byte(s)`.

**The general lesson**: anywhere a `char buf[N]` plus `strncpy/snprintf("%.Ns")` truncates user text, the meaning is silently changed. There can be several truncation points (HTTP boundary → session → agent context), and fixing one layer is not enough — prefer `utf8_copy` at every layer, and do one hard validation at the outermost exit.

### Abort behaviour

Emergency stop (WebUI Emergency / long press GPIO1), 3 consecutive capture failures, and 3 consecutive LLM failures all **release every HID key and return to IDLE** rather than terminating the agent task — so after an emergency stop the device can be started again from the WebUI or GPIO0, with no power cycle.

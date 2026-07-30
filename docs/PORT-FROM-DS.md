# Porting DS back to the Cardputer

DS (`../DS`) started as DOLL-OS ported to a Freenove FNK0104 ESP32-S3 display
board, with telnet replacing the physical keyboard. It has since grown well past
its parent. This document is the plan for bringing that growth back to the
M5Cardputer, skipping anything that genuinely needs PSRAM.

Read `../DS/docs/PORTING.md` first — it documents the outbound trip and explains
*why* most of the divergence exists. This is the return leg.

## Status: all phases complete

Every phase below is implemented and compiles clean (no warnings from sketch
files at `--warnings all`). **Nothing here has run on hardware yet** — the
numbers are build output, not observed behaviour, and the whole thing is
untested on a real device.

| Phase | Flash | Δ | Static RAM | Δ |
|---|---|---|---|---|
| baseline | 1 437 371 | — | 106 180 | — |
| 2 · `outLine()` seam | 1 438 135 | +764 | 106 180 | 0 |
| 3.1–3.4 · fs cmds, reboot/uptime/status, battery, wifi | 1 449 943 | +11 808 | 106 204 | +24 |
| 3.5 · `.dapp` runtime | 1 461 571 | +11 628 | 109 420 | +3 216 |
| 3.6 · Fn+K inline prompt | 1 461 939 | +368 | 109 420 | 0 |
| 4 · FTP server | 1 481 875 | +19 936 | 112 644 | +3 224 |
| 5 · `edit` | 1 493 179 | +11 304 | 113 108 | +464 |
| resync · `.dapp` strings, `RAND`, `IFEQ` | 1 498 795 | +5 616 | 113 108 | 0 |

Totals: **+61 424 bytes flash** (1.50 MB of the 6.375 MB slot, 22%) and
**+6 928 bytes static RAM**, leaving 214 572 bytes free. The editor's 18 KB
buffer is allocated on entry and freed on exit, so it doesn't appear above; so
are the `.dapp` string variables (8 × 128 B worst case, on `appExecute`'s frame).

Deviations from the plan as written, all recorded in place below: the inline
command chord is **Fn+K** not Ctrl+K; the editor's "go to line" is **^L** not
`^_`; there is **no boot-time Wi-Fi join at all**; and Phase 1's `sketch.yaml`
had to be written after all, because Phase 4 cannot work without it.

**Resync, 2026-07-30.** DS kept moving after Phase 3.5 copied `AppRunner.ino`
across, so the `.dapp` runtime had drifted behind its parent. Brought level:
string variables (`SETSTR` / `APPEND` / `INPUT`, backed by a new `DappStringVar`
in `global.h`), `RAND`, and the string comparisons `IFEQ` / `IFNE`. Caps are
reduced the way Phase 5 reduced the editor's — 8 string variables of 128
characters against DS's 8 × 512 — and the step budget takes DS's 4000 in place of
1200. What is *not* from DS is **Fn+Q**: DS can always drop the telnet session to
escape a running app, and this can't, so `appDelay()` and the new `INPUT` prompt
both sample the same local abort chord `ssh` and `telnet` already use. That is
what makes the higher step budget safe to take.

## The hardware delta that drives everything

| | DOLL-OS (M5Cardputer) | DS (FNK0104AB) |
|---|---|---|
| MCU | M5Stamp-S3 — ESP32-S3FN8 (board ID `m5stack_stamp_s3`) | ESP32-S3-WROOM-1 N16R8 |
| PSRAM | **none** | 8 MB octal |
| Flash | 8 MB | 16 MB |
| Panel | 240×135 ST7789 (M5GFX) | 320×240 ILI9341 (TFT_eSPI) |
| Input | built-in keyboard | telnet + DS-Slave BLE bridge |
| SD | SPI (CS 12 / MOSI 14 / MISO 39 / SCK 40) | SDMMC 4-bit |
| Audio | NS4168 I2S amp, mono, no codec | ES8311 codec over I2C |
| USB | native OTG | serial bridge only |

Two consequences dominate the whole plan:

1. **No PSRAM.** ~390 KB of usable internal heap, shared with WiFi and mbedTLS.
   DS routes its general heap to PSRAM at boot (`enablePsramHeap()`); the
   Cardputer has nowhere to route to.
2. **The build is silently on a 4 MB partition scheme.** DOLL-OS has no
   `sketch.yaml`, so it takes each board menu's first entry — for
   `m5stack_stamp_s3` that's `PartitionScheme=default` (1.25 MB app slot), even
   though the board's own `build.partitions` is `default_8MB`. Half the part is
   stranded. Fixed in Phase 1 with a custom table (`partitions.csv`, already
   written) that spans the full 8 MB.

## Verdict table

### Excluded — needs PSRAM

| Feature | Why |
|---|---|
| `radio` (`Radio.ino`, `es8311.*`) | `ESP32-audioI2S` hard-requires PSRAM — `Audio.cpp:333` logs `"audioI2S requires PSRAM!"` and its buffers are `ps_malloc`. Independently blocked anyway: the Cardputer has no ES8311 codec and no I2C audio bus. |
| `gb` (`Gameboy.ino`, `src/`) | See note below — technically not blocked, but not a port. |

**The `gb` caveat, stated honestly:** the vendored gnuboy core *already* has a
no-PSRAM fallback (`src/emulator/gnuboy/gnuboy.c` — bank-paging from SD with a
28 KB reserve) and its comment says that path was hardware-verified against a
Cardputer. So "requires PSRAM" is not strictly true of the emulator. What blocks
it is everything around it: audio goes through `src/AudioOut.cpp`, which is
hardcoded to the ES8311 pins and calls into `Radio.ino`; input comes from
DS-Slave gamepad mode, which the Cardputer doesn't have; and the panel is
240×135, *shorter* than the Game Boy's 144 lines, so even 1× doesn't fit without
a new scaler. That is a project, not a port. Excluded here, flagged as a
possible standalone follow-up.

### Port — logic only, no design work

These carry over by swapping output calls and the SD library. DS's copies are
also mildly better than DOLL-OS's originals (e.g. `Calc.ino` dropped the VLA
`String[]`/`uint16_t[]` help arrays for direct `outLine` calls — worth taking on
its own, given the fragmentation notes in the git log).

| Feature | Files | Work |
|---|---|---|
| `mkdir` `rm` `del` `cp` `mv` | `Storage.ino` | `SD_MMC` → `SD`, `outLine` shim. ~250 new lines, all self-contained. |
| `reboot` `uptime` `status` | `CommandProcessor.ino` | Trivial. |
| `battery` | `SysInfo.ino` | Replace the ADC divider read with `M5.Power.getBatteryLevel()`. Also fills the two `TODO` stubs at the bottom of `hardware.ino`. |
| `apps` / `run` (.dapp runtime) | `AppRunner.ino`, `docs/DAPP.md` | Ports whole. Needs `readBatteryPercent()`, `wifiIsConnected()`, and an `appDelay()` that pumps the Cardputer loop (`M5Cardputer.update()` / `statusManagement()` / `drawTerminalHistory()`) instead of DS's `ftpService()`/`radioService()`/`drawDisplayFrame()`. |
| WiFi robustness | `WiFiManager.ino` | DOLL-OS's `wifi.ino` already has scan/connect/save. Take three specific things: `WiFi.setAutoReconnect(false)` + the bounded `maintainInternetConnection()` tick (the comment there explains a real bug — a background association spin that makes `wifi scan` and `wifi connect` fail outright), a boot-time `connectToInternet()`, and `initStorage()`'s LittleFS reformat-on-corruption recovery. |
| `calc` cleanup | `Calc.ino` | Drop the VLA help arrays. |

### Port — real work

| Feature | Effort | Notes |
|---|---|---|
| FTP server | Small–medium | `SimpleFTPServer` is already installed. Backend is selected in the *library's* `FtpServerKey.h` (`STORAGE_SD_MMC` for DS, needs `STORAGE_SD` here) — a hand edit to a globally shared library. Copy it sketch-local instead; see Phase 1. |
| `edit` | Medium–large | Judgment call — see below. |

### Explicitly out of scope

**The telnet server.** DS uses telnet as its *only* input path; the Cardputer has
a keyboard and doesn't need a second one. Dropped by decision, not by
constraint. What goes with it: `TelnetServer.ino` entirely, and therefore
`processLineEditByte()` / `LineEditState` / `LineInputResult` (DS's byte-level
line editor), `telnetNegotiateServerMode()`, `telnetReadFilteredByte()`, the
`activeInputPrompt`/`activeInputText`/`activeInputMasked` mirror trio, and the
socket half of `outLine()`'s fan-out. `readKeyboard()` stays the sole input path
and `outLine()` stays sprite-only.

Note this is the *server*. The outbound `telnet` **client** command is
unaffected and already present.

One piece survives on its own: DS's `^K` inline-command prompt
(`RemoteSession::runInlineCommandPrompt()`) — run one shell command mid-ssh
without ending the session. It only needs *an* input path, not telnet's. See
Phase 3.

**The DS-Slave link.** `KeyboardSerial.ino` + `SlaveLink.ino` bridge a BLE HID
keyboard/gamepad over UART — the same "second input source" idea, same answer.
If a BLE gamepad ever becomes interesting, the Grove port (G1/G2) is the wiring.

### The `edit` judgment call

DS's editor stores the file as a flat 128 KB slab plus a 4000-entry line index,
both in PSRAM, with 64 undo records whose delete payloads are also PSRAM
(`Edit.ino:50-51,88`). Strictly read, that makes it a PSRAM feature and it
should be excluded.

But the PSRAM dependency is in the *caps*, not the design — the flat-slab
architecture exists specifically to avoid per-line `String` allocations
scattered across the heap, which is exactly what you'd want on a machine with no
PSRAM and a fragmentation problem. Scaled to 16 KB / 512 lines / 16 undo steps,
it's about 20–26 KB and fits.

**Decided: `edit` stays in, at reduced caps.** It remains the largest single
chunk of work in the plan, which is why it's sequenced last.

Other Cardputer-specific editor changes: the viewport is **38×9** at the 6×8
font (vs DS's 52×21) — cramped, but the text area is exactly the terminal
sprite, so this is what the hardware gives; and `editDecodeByte()`'s byte/CSI
decoder is replaced outright by a `Keyboard_Class::KeysState` → `EditKey` mapper
(with the telnet server cut, nothing feeds bytes to it). The Cardputer keyboard
has a real `ctrl`, so nano's `^O`/`^X`/`^K`/`^W`/`^G` chords all work as-is;
arrows use the existing `Fn+;` `Fn+.` `Fn+,` `Fn+/` convention from
`hardware.ino`. The one chord that could not survive is nano's `^_` for "go to
line" — there is no way to type it on this keyboard, so it became `^L`.

### Already present, no port needed

`ssh`, `telnet` (client), `motoko`, `ip`, `ping`, `dice`, `usb`, `ls`/`cd`/
`pwd`/`cat`, command history, `free`. The DS copies differ substantially on
paper, but the diffs are almost entirely the interface swap (sprite+keyboard →
telnet), which is exactly what we're *not* taking. Worth a read-through for
incidental fixes, not a merge.

---

## Phased plan

### Phase 0 — headroom first

Nothing else is safe until this is done. Per the git log, memory is already the
sore spot; this plan adds an FTP server, an app runner, and an editor on top.

1. **Measure.** `free details` already reports fragmentation and per-pool caps.
   Record a baseline: boot, after WiFi, after an ssh session, after `usb`.
   `recordHeapCheckpoint()` is already wired for this.
2. **Budget.** Current known consumers:
   - three sprites: status 240×14×2 (6.7 KB) + terminal 240×97×2 (46.6 KB) +
     command bar 240×24×2 (11.5 KB) ≈ **65 KB**
   - `historyRows`: 120 × 98 B ≈ **11.8 KB** static
   - libssh/mbedTLS handshake task (the dominant transient)

   Incoming: FTP server + 2 clients + transfer buffer ≈ 10 KB; editor
   ≈ 20–26 KB.
3. **Reclaim.** Options in rough order of value:
   - Merge the three sprites into one 240×135 frame sprite the way DS does
     (`frameSprite` + `pushDisplayFrame`). Same total pixels, but one allocation
     instead of three — better for fragmentation, not for total bytes.
   - Or go the other way: drop the status/command-bar sprites and draw those two
     strips directly to `M5Cardputer.Display`, saving ~18 KB outright.
   - Make every new subsystem lazily initialized and explicitly stoppable
     (`ftp on`/`ftp off` already is). DS's
     `Radio.ino` documents why this matters — a constructor that grabbed DMA
     buffers before `setup()` starved the WiFi stack into an OOM crash.
   - Take the `Calc.ino` VLA cleanup.

**Exit criterion:** a documented free-heap floor with ssh connected, and a
number you're willing to spend against.

### Phase 1 — build configuration

DOLL-OS has no `sketch.yaml`, so every board menu falls to its first entry.
Three of those defaults are wrong for this project.

Target board is the **M5Stamp-S3**, 8 MB flash, no PSRAM — board ID
`m5stack_stamp_s3` (underscores; `m5stack_stamps3` is not a thing), verified
against the installed core at `esp32:esp32@3.3.10`.

1. Add `sketch.yaml` with:

   ```yaml
   default_profile: stamps3
   profiles:
     stamps3:
       fqbn: esp32:esp32:m5stack_stamp_s3:FlashSize=8M,PartitionScheme=custom,PSRAM=disabled,USBMode=default,CDCOnBoot=cdc
       platforms:
         - platform: esp32:esp32 (3.3.10)
   ```

   The three that matter, and what each fixes:
   - **`PartitionScheme=custom`** — makes the core pick up this sketch's own
     `partitions.csv` (step 2). The menu default is `PartitionScheme=default`,
     a **1.25 MB** app slot on a 4 MB layout — it strands half the part and is
     the thing most likely to bite as the app grows.
   - **`PSRAM=disabled`** — the menu default is also `disabled` here, but pin it
     explicitly so nothing silently defines `BOARD_HAS_PSRAM`. `SysInfo.ino`'s
     detailed heap dump is already `#if defined(BOARD_HAS_PSRAM)`-guarded, so it
     compiles out cleanly.
   - **`USBMode=default`** — this is the confusingly-named TinyUSB option
     (`build.usb_mode=0`). The menu default is `USBMode=hwcdc`
     (`build.usb_mode=1`), under which `usb_msc.ino:6` fires its `#error` and the
     whole sketch refuses to build. If `usb` currently works for you, you set
     this by hand in Tools once; pinning it means a fresh checkout doesn't have
     to know that.

2. **`partitions.csv` — already written**, at the repo root next to
   `DOLL-OS.ino`. It's inert until step 1's `PartitionScheme=custom` selects it,
   so it changes nothing about the current build.

   | Partition | Offset | Size | Purpose |
   |---|---|---|---|
   | `nvs` | `0x9000` | 20 KB | core NVS |
   | `otadata` | `0xe000` | 8 KB | slot-select for `boot_app0.bin` |
   | `app0` (`ota_0`) | `0x10000` | **6.375 MB** (6 684 672 B) | the sketch |
   | `spiffs` | `0x670000` | 1.5 MB | LittleFS — `/wifi.cfg`, `/apps/*.dapp` |
   | `coredump` | `0x7F0000` | 64 KB | crash postmortem |

   Exactly 8 MB, no dead space. It keeps `default_8MB`'s shape but drops the
   `app1` OTA slot — this project is flashed over USB, not over the air, so that
   was 3.19 MB doing nothing — and gives every reclaimed byte to the app. At
   6.375 MB the app slot is ~5× the stock menu default and ~2× `default_8MB`;
   flash stops being something to think about at all.

   Three things about it worth knowing:
   - The app partition is `ota_0` subtype rather than `factory`, so
     `boot_app0.bin`'s slot-select still applies even with no `app1` present.
     Same trick as DS's table — copy it, don't "fix" it to `factory`.
   - The LittleFS partition is *labelled* `spiffs` because that's the label
     `LittleFS.begin()` looks for by default. Contents are LittleFS. Its offset
     and size are byte-identical to `default_8MB`'s, so if you ever pass through
     that scheme the contents survive.
   - **Coming from the menu default it still moves** (`default` puts spiffs at
     `0x290000`), so the first boot after switching formats it and `/wifi.cfg`
     is lost. Re-run `wifi connect` + `wifi save` once. Phase 3's
     reformat-on-corruption work makes this failure mode self-healing rather
     than a silent fallback to the `config.h` defaults.

   One sharp edge: with `PartitionScheme=custom` the board sets
   `upload.maximum_size=16777216`, so the IDE reports your sketch against a
   **16 MB** ceiling and won't warn when it overruns the real slot. Ignore the
   percentage and watch the absolute byte count against 6 684 672 — though at
   this size you'd have to try.
3. Pin libraries in `sketch.yaml` the way DS does. Copy `SimpleFTPServer`
   sketch-local (`DOLL-OS/libraries/SimpleFTPServer`) so its
   `FtpServerKey.h` → `STORAGE_SD` edit doesn't collide with DS's
   `STORAGE_SD_MMC` in your shared sketchbook. DS hit exactly this class of
   problem with `TFT_eSPI`; its `docs/PORTING.md` "Building this sketch" section
   is the precedent.

**Exit criterion:** current DOLL-OS builds unchanged under the new profile,
still boots, and `wifi save` survives a reboot (proving LittleFS landed on the
new partition). Record the sketch's absolute byte size — it's the baseline every
later phase is spending against, and the IDE's percentage is meaningless here.

### Phase 2 — the `outLine()` seam

This is the highest-leverage step in the whole plan, and it survives the telnet
server being cut — its value was never the socket fan-out, it's that every DS
file to be ported calls `outLine(text)` / `outLine(text, C_CYAN)`. DOLL-OS calls
`addWrappedHistoryLine(text)` / `(text, CYAN)` — same shape, different name, and
a `uint16_t` pixel color instead of an ANSI SGR integer.

Build the shim once and every subsequent port becomes a copy-paste:

1. Add the `C_*` ANSI constants to `global.h` (copy from DS's `global.h:184-193`).
2. Port `ansiCodeToPixelColor()` from `Display.ino:535` — maps `C_*` → M5GFX
   `uint16_t`.
3. Add `Output.ino`: `outLine(const String&)`, `outLine(const String&, int)`,
   `outClearScreen()`, `shellPrompt()`, `echoCommandLine()`. These are thin
   wrappers over `addWrappedHistoryLine` / the history reset that
   `commandProcessor` does inline today — no socket half, so no `printPrompt()`.
4. Optionally sweep existing DOLL-OS call sites onto `outLine`. Not required —
   the two can coexist — but it keeps the ported and native files reading the
   same way, which is the point of the "keep it diffable" policy below.

`shellPrompt()` is worth taking even though DOLL-OS's prompt is currently a bare
`"> "`: it makes the prompt path-aware (`/sd/apps > `), which matters a lot once
`mkdir`/`cp`/`mv` exist. `drawCommandBar()` already has a two-argument
`(prompt, text)` overload to render it.

**Exit criterion:** `outLine` exists, colors round-trip, nothing looks different.

### Phase 3 — the drop-in ports

In dependency order. Each is independently shippable.

1. **`Storage.ino` filesystem commands.** Copy DS's `mkdir`/`rm`/`del`/`cp`/`mv`
   and their helpers (`pathIsProtectedRoot`, `parentPathOf`, `baseNameOf`,
   `childPathOf`, `ensureMountedForPath`, `removeResolvedPath`,
   `copyResolvedFile`). `routePath()` returns `&SD` instead of `&SD_MMC`; nothing
   else changes — `resolvePath`/`RoutedPath` are already byte-identical between
   the two repos.
2. **`reboot` / `uptime` / `status`.** Straight copy into `CommandProcessor.ino`.
3. **`battery` + the `hardware.ino` stubs.** `readBatteryPercent()` /
   `readBatteryVoltage()` backed by `M5.Power`; `statusManagement()` already
   reads it inline, so this consolidates two call sites onto one.
4. **WiFi robustness.** The `setAutoReconnect(false)` +
   `maintainInternetConnection()` pair, and LittleFS reformat-on-corruption.
   Add `maintainInternetConnection()` to `loop()`.

   **DS's boot-time `connectToInternet()` is deliberately not ported.** DS is
   headless and its entire UI is a telnet socket, so it has nothing to show
   until the network is up. Here the screen and keyboard are a complete UI
   without one, and a handheld that reaches for an AP every time it powers on
   isn't the intent. The radio stays off until `wifi connect` asks for it.

   Removing the boot call is only half of it, and the half that doesn't work
   alone: `maintainInternetConnection()` runs every loop pass and would see
   "not connected" and join anyway about ten seconds later. So the tick is gated
   on `wifiUserWantsConnection`, which nothing sets at boot and which flips true
   only after a `wifi connect` *succeeds* — arming on the attempt instead would
   leave a typo'd SSID retrying forever. The tick therefore re-establishes a
   link that dropped, and never initiates one.

   Two supporting pieces: `WiFi.persistent(false)`, so the core stops mirroring
   credentials into its own NVS copy (DOLL-OS's live in `/wifi.cfg`, and the NVS
   copy is the one piece of state that could still put the radio on a network
   unasked via a later bare `WiFi.begin()`); and two new subcommands,
   `wifi disconnect` (drops the link and disarms the tick) and `wifi forget`
   (clears the radio's stored network, for units flashed before
   `persistent(false)` — it leaves `/wifi.cfg` alone).
5. **`apps` / `run`.** Copy `AppRunner.ino` + `docs/DAPP.md` wholesale; rewrite
   `appDelay()` against the Cardputer loop. `$battery`/`$ip`/`$heap`/`$wifi`
   builtins all resolve once step 3 lands.
6. **`^K` inline command prompt.** Port
   `RemoteSession::runInlineCommandPrompt()` — run one shell command mid-ssh
   (e.g. `ip`, `ls`) without ending the session, then resume raw forwarding.
   DS's version alternates its two byte sources; here it collapses to a
   `readKeyboard(cmdBuffer)` loop with `drawCommandBar("cmd> ", cmdBuffer)`,
   which is smaller than the original. It composes with the `RemoteSession` base
   class already in `global.h`, and needs a dedicated buffer so a half-typed
   inline command can't tangle with `currentCommand`.

**Exit criterion:** `help` lists `apps battery calc cat cd clear cp del dice
free help ip ls mkdir motoko mv ping pwd reboot rm run ssh status telnet uptime
usb wifi`.

### Phase 4 — FTP server

Small, mostly mechanical, and it's what makes `apps`/`run` and `edit` practical
(getting files onto the SD card without pulling it).

1. Sketch-local `SimpleFTPServer` with `DEFAULT_STORAGE_TYPE_ESP32
   STORAGE_SD` (Phase 1 set this up).
2. Copy `FtpServer.ino` verbatim — it's already non-blocking and already gated
   behind `ftp on`/`ftp off`.
3. Add `ftpService()` to `loop()` and `FTP_USER`/`FTP_PASS` to `config.h`.

Note that unlike DS, the Cardputer *has* working USB MSC, so FTP is a
convenience rather than the only way in. That makes it a fine candidate to
defer if Phase 0's budget comes back tight.

**Exit criterion:** `ftp://<ip>/` browsable from Explorer with the shell still
responsive during a transfer.

### Phase 5 — `edit`

Confirmed in scope at reduced caps (see the judgment call above). Order within
the phase:

1. Port `Edit.ino` with caps reduced: `EDIT_BUF_CAP` 16 KB, `EDIT_MAX_LINES`
   512, `EDIT_UNDO_MAX` 16. Allocate with plain `heap_caps_malloc(…,
   MALLOC_CAP_8BIT)` — DS's `psramOrInternalCalloc()` already falls back to
   internal RAM, so it can come across unchanged and just always take the
   fallback path.
2. Retarget `editRenderPanel()` from `frameSprite`/TFT_eSPI to the M5GFX
   sprites. It turned out to map cleanly onto the three the shell already owns —
   status bar → title, terminal → text, command bar → hint — so the editor
   allocates **no** framebuffer of its own. On a machine with ~215 KB free that
   would otherwise have been the largest single thing in this phase.
3. Replace `editDecodeByte()` (DS's ESC/CSI byte decoder) with a
   `KeysState` → `EditKey` mapper. With no telnet client to feed it, the byte
   decoder has no remaining caller and should be dropped outright rather than
   carried dead — that's ~100 lines of `Edit.ino` gone.
4. Enforce a hard file-size check on load: refuse anything over
   `EDIT_BUF_CAP` with a clear message rather than truncating.

The save path (temp file + rename, never a truncating open on the target) and
the CRLF→LF normalization should come across untouched — both are correctness
properties, not memory ones.

**Exit criterion:** `edit /apps/hello.dapp`, type, `^O`, `^X`, and `run hello`
executes what you wrote.

---

## Risks and open questions

- **Flash — no longer a risk at all.** For scale, DS's *entire* build (gnuboy,
  radio, editor and all) reports ~8% of its 16 MB layout, against
  `partitions.csv`'s 6.375 MB slot here. FTP + editor + app runner on top of
  libssh will not come close. The only residual annoyance is that
  `PartitionScheme=custom` disables the IDE's overrun warning — see Phase 1.
- **Internal RAM under concurrency.** ssh (mbedTLS) + an FTP transfer + the
  editor buffer all live in the same ~390 KB. Cutting the telnet server helps
  here — that was the one addition with no natural off switch. Of what's left,
  realistically these won't all be hot at once, but nothing currently *prevents*
  it. Consider refusing `edit` while an FTP transfer is in flight, and refusing
  `ftp on` during an ssh session.
- **`gb` is closer than it looks.** If the emulator ever becomes interesting,
  the blockers are a Cardputer `AudioOut` (NS4168, no codec), an input mapper
  off the built-in keyboard, and a 144→135 vertical scaler. The gnuboy core
  itself needs nothing.
- **DS-Slave.** Left out entirely. If a BLE gamepad becomes interesting later,
  `KeyboardSerial.ino` + `SlaveLink.ino` port to the Grove port (G1/G2) — but
  note DS's hard-won lesson there: bit-banged UART needs `IRAM_ATTR` on both the
  write loop and its wait helper, or a flash write on the other core corrupts
  bit timing mid-byte.
- **Divergence.** After this, DS and DOLL-OS share a large body of near-identical
  code across two repos with no shared library. That's already true today and
  this makes it more so. Worth deciding whether the ported files should be kept
  diffable (same function names, same ordering) as a deliberate policy — this
  plan assumes yes, which is why Phase 2's `outLine()` shim adopts DS's names
  rather than keeping DOLL-OS's.

# DOLL-OS API Guide

A reference for writing new applications (commands, modal tools, remote-session
clients) against DOLL-OS's internal C++ APIs. Companion to
[`architecture.md`](architecture.md), which explains the runtime shape; this
document is about the functions you actually call.

Everything below lives directly in the sketch's `.ino`/`.h` files — there is no
package boundary, so "the API" just means "functions and globals declared
somewhere in this sketch that other files already call." Signatures are quoted
verbatim from source as of this writing.

## 1. Mental model

DOLL-OS is one cooperative loop (`DOLL-OS.ino`). There's no task system, no
event bus, no plugin registry. Three ways to add an "application":

```mermaid
flowchart TD
    A[New feature] --> B{Needs the screen/keyboard\nfor more than one tick?}
    B -->|No, one-shot| C[Simple command handler\nvoid handler(parts, partCount)]
    B -->|Yes| D{Talking to a byte-stream\ntransport (socket)?}
    D -->|No -- local line-based\nQ&A, e.g. MQTT chat| E[Modal blocking-loop app\nlike motoko.ino]
    D -->|Yes -- ssh/telnet-style\nraw pty/character mode| F[RemoteSession subclass\nlike ssh.ino / telnet.ino]
```

All three ultimately write to the same terminal history and read from the same
keyboard — there's no separate UI stack per feature.

## 2. Global state (`global.h`)

Declared once, used everywhere. The ones you'll actually touch:

| Symbol | Type | Meaning |
|---|---|---|
| `currentCommand` | `String` | top-level shell's live input buffer |
| `commandCursorPos` / `commandScrollOffset` | `int` | cursor/scroll state `drawCommandBar` and `readKeyboard` maintain together |
| `historyLines[HISTORY_MAX_LINES]` / `historyColors[...]` / `historyCount` | terminal history rows and their per-row color |
| `scrollOffset` | `int` | how far back the terminal view is scrolled |
| `cwd` | `String` | current working directory in the unified storage namespace |
| `SD_MOUNT` | `const String` | `"/sd"` — where the SD card appears in that namespace |
| `sdCardMounted` | `bool` | set by `initStorage()` |
| `RemoteSession` | class | base class for raw byte-stream modal sessions (§8) |
| `RoutedPath`, `AnsiFilterState`, `TerminalStreamState` | structs | declared here rather than in their owning `.ino` — see the note in §11 about why |

Anything you add that a hoisted function signature needs to reference (a struct
used as a parameter type, for instance) has to go in `global.h` too, for the
same reason.

## 3. Terminal output API (`terminal.ino`)

The terminal is a scrolling list of already-wrapped rows, not a
grid-addressable screen. This is the API almost every command handler uses to
produce output.

```cpp
void addWrappedHistoryLine(const String& line);                 // white text
void addWrappedHistoryLine(const String& line, uint16_t color); // colored
```
Wraps `line` at the terminal sprite's pixel width and appends one or more rows
to history. This is what you want for "print a line of output" in a command
handler.

```cpp
void addHistoryRow(const String& row, uint16_t color);
```
Lower-level: appends exactly one row, no wrapping. Used internally by
`addWrappedHistoryLine`; you'd only call it directly if you've already done
your own wrapping.

```cpp
void updateLastHistoryRow(const String& row, uint16_t color);
```
Overwrites the most recent row in place instead of appending — for live-updating
a line that hasn't been newline-terminated yet (e.g. a streaming prompt).

```cpp
void scrollHistory(int delta);
void drawTerminalHistory();
```
`scrollHistory` moves the view by `delta` rows (clamped). `drawTerminalHistory`
repaints the terminal sprite from `historyLines`/`historyColors`/`scrollOffset`
— it already runs once per main-loop tick, but call it manually right before a
blocking operation (Wi-Fi scan, ping, ssh connect...) so the user sees your
"Connecting..." line before the freeze, not after.

**Colors** are the standard M5GFX/LovyanGFX 16-bit constants already in scope:
`WHITE`, `BLACK`, `RED`, `GREEN`, `BLUE`, `YELLOW`, `CYAN`, `MAGENTA`, `PINK`.

## 4. Command bar / input drawing (`terminal.ino`)

```cpp
void drawCommandBar(const String& text);                    // prompt fixed to "> "
void drawCommandBar(const String& prompt, const String& text);
```
Draws the bottom input bar. `prompt` is a fixed, non-editable label pinned to
the left (`"> "`, `"password> "`, `"channel> "`, `"$ "`, ...). `text` is the
live-editable buffer — `commandCursorPos`/`commandScrollOffset` index into
*that* string, so always pass your raw buffer, never `prompt + text`
concatenated (that shifts the cursor math).

Every modal app in the codebase (motoko, ssh, telnet) draws its own prompt
through this same function rather than inventing new UI.

## 5. Keyboard input API (`hardware.ino`)

```cpp
bool readKeyboard(String& text);
```
Polls once, edits `text` in place (printable chars, backspace, cursor-move,
history-recall chords), returns `true` when Enter was just pressed. This is
the line-editor used by the top-level shell *and* by any modal app that wants
buffered, editable local input (motoko's channel/message prompts, ssh's
password prompt). Built-in chords while it owns input: `Fn+;`/`Fn+.` scroll
history, `Fn+,`/`Fn+/` move the cursor, `Ctrl+;`/`Ctrl+.` recall previous/next
sent commands.

```cpp
bool readRawKeyBytes(String& outBytes, bool& escapePressed, bool& backspacePressed);
```
The other input mode: no local buffering or editing, every keystroke becomes
raw bytes immediately (like a real terminal feeding a pty). `Fn+Q` sets
`escapePressed` instead of producing bytes — that's the universal "disconnect"
chord for raw sessions. Ctrl+letter becomes the matching control byte
(`Ctrl+C` → `0x03`). You won't normally call this yourself — it's what
`RemoteSession::run()` calls on your behalf (§8).

```cpp
bool keysContainChar(const Keyboard_Class::KeysState& keys, char target);
```
Helper for checking whether a given char is in this tick's `keys.word`.

> **Known stub:** `batteryPercentCheck()`/`batteryVoltCheck()` in
> `hardware.ino` have empty bodies but non-`void` return types (undefined
> return value) — they're unimplemented TODOs. Don't call them; the real
> battery read is inlined in `statusManagement()` (`terminal.ino`).

## 6. Writing a simple command

A command is a function plus one line in a dispatch table.

**Handler signature**, always:
```cpp
void handleFooCommand(const String parts[], int partCount);
```
`parts[0]` is the command name itself; `parts[1..3]` are up to three
space-delimited arguments (`splitCommand` in `CommandProcessor.ino` caps
tokenization at 4 slots total — a command needing more arguments has to do its
own parsing on `parts[1]` onward, or the one `splitCommand(entered, parts, 4)`
call site in `commandProcessor()` needs to change).

**Register it** in `commandTable[]` (`CommandProcessor.ino`), kept
alphabetical by convention, and add it to `helpCommandHandler`'s text:
```cpp
static const CommandEntry commandTable[] = {
    { "cd",     handleCdCommand },
    { "dice",   handleDiceCommand },
    { "foo",    handleFooCommand },   // <- add here, alphabetically
    ...
};
```
Lookup is a linear scan (`parts[0] == commandTable[i].name`), exact match, no
aliases. `clear` is the one exception — it's handled inline in
`commandProcessor()` because it mutates terminal history directly rather than
going through the table.

**Minimal worked example** (`dice.ino` is the reference "hello world" — small,
stateless, colored output):
```cpp
void handleDiceCommand(const String parts[], int partCount) {
    int diceSides = 6, diceNumber = 1;
    if (partCount == 1) { diceHelp(); return; }
    if (parts[1].length() != 0) diceSides  = parts[1].toInt();
    if (parts[2].length() != 0) diceNumber = parts[2].toInt();
    diceRoll(diceSides, diceNumber);
}
```

For subcommands (multi-mode commands like `wifi`/`ip`), branch on `parts[1]`
inside the handler — see `handleWifiCommand`/`handleIpCommand` for the
established pattern (`wifi`, `wifi scan`, `wifi connect <ssid> <pass>`, ...).
This is the natural extension point the architecture doc calls out.

## 7. Building a modal (blocking) application

For a feature that needs the screen and keyboard for an extended, multi-step
local interaction — not a raw byte-stream transport, but line-based prompts
against some backend (MQTT pub/sub is the existing example) — follow
`motoko.ino`'s shape:

1. Module-level state: an input-mode enum, your own `String` input buffer
   (never reuse `currentCommand` — that belongs to the top-level shell), and
   whatever session state your feature needs.
2. `void fooDrawInputRow()` — draws your prompt via `drawCommandBar(prompt, buffer)`.
3. `void runFooBlocking()` — the modal loop:
   ```cpp
   void runFooBlocking() {
       while (true) {
           M5Cardputer.update();
           delay(10);
           // ... poll your backend/transport, non-blocking ...
           bool enterPressed = readKeyboard(fooInputBuffer);
           if (enterPressed) {
               if (fooInputBuffer == "/quit") break;   // local exit convention
               fooHandleEnteredLine();
               fooInputBuffer = "";
           }
           drawTerminalHistory();
           fooDrawInputRow();
       }
   }
   ```
4. `void handleFooCommand(parts, partCount)` — validate preconditions (e.g.
   `WiFi.status() == WL_CONNECTED`), print an intro line, draw once
   (`drawTerminalHistory(); fooDrawInputRow();`) since the main loop isn't
   ticking during the blocking call, run `runFooBlocking()`, then **restore
   the shell UI on the way out**:
   ```cpp
   drawTerminalHistory();
   drawCommandBar(currentCommand);
   addWrappedHistoryLine("foo: exited");
   ```

## 8. RemoteSession framework — raw byte-stream apps (`global.h` + `RemoteSession.ino`)

For anything that's really a character-oriented remote transport — a socket
where every keystroke must go out immediately (arrow keys, Ctrl+C,
backspace-before-Enter, full-screen remote programs) — don't hand-roll the
loop. Subclass `RemoteSession` (declared in `global.h`, base loop implemented
in `RemoteSession.ino`); `ssh.ino`'s `SshShellSession` and `telnet.ino`'s
`TelnetSession` are the two existing implementations.

```cpp
class RemoteSession {
public:
    void run();   // owns the poll/pump/redraw loop + local Fn+Q escape chord
protected:
    virtual void pumpIncoming() = 0;                   // drain transport -> terminal, non-blocking
    virtual bool isClosed() = 0;                        // has the remote gone away
    virtual void sendBytes(const String& bytes) = 0;    // forward raw keystroke bytes
    virtual void drawInputRow() = 0;                     // your prompt/status row
    virtual void onClosed() {}                           // one-shot, first time isClosed() is true
    virtual String backspaceBytes() { return "\x7f"; }   // DEL by default; override per transport
};
```

Usage is always: construct a subclass instance, call `.run()`, it blocks until
the remote closes or the user hits `Fn+Q`. `backspaceBytes()` matters because
transports disagree — a real pty (ssh) wants DEL (`0x7F`); classic
telnet/BBS line editors want ASCII backspace (`0x08`) — see `telnet.ino`'s
override.

### Streaming terminal API (`terminal.ino`)

Byte streams arrive character-by-character and need to show up live (a
`login:` prompt with no trailing newline shouldn't stay invisible). Use one
`TerminalStreamState` instance per independent stream (ssh keeps separate ones
for stdout/stderr so they can interleave without corrupting each other):

```cpp
void terminalStreamReset(TerminalStreamState& st);              // call at session start, and defensively at end
void terminalStreamPutChar(TerminalStreamState& st, char ch, uint16_t color);  // one printable char
void terminalStreamNewline(TerminalStreamState& st);            // on '\n'
void terminalStreamCarriageReturn(TerminalStreamState& st);     // on bare '\r' (line-redraw-in-place idiom)
void terminalStreamEraseToEnd(TerminalStreamState& st);         // on CSI 'K' / erase-in-line
void terminalStreamBackspace(TerminalStreamState& st);          // when the remote's own echo sends BS/DEL
```

### ANSI/UTF-8 filter (`ansi.ino`)

Feed raw incoming bytes through this before handing them to the streaming API
above — it consumes cursor-motion escape junk, interprets SGR color codes
against a per-stream `AnsiFilterState`, and collapses multi-byte UTF-8 into a
single `?` placeholder (the display font is ASCII-only):

```cpp
bool ansiFilterByte(AnsiFilterState& st, uint8_t ch, uint16_t defaultColor,
                     uint16_t& color, char& outCh, bool& colorChanged,
                     bool& isBackspace, bool& eraseToEndOfLine);
```
Returns `true` when `outCh`/`color` are a real character to display; otherwise
check `isBackspace` / `eraseToEndOfLine` and call the matching
`terminalStream*` function, or drop the byte if none apply. See
`telnetProcessByte()` in `telnet.ino` for the canonical wiring of filter →
streaming API.

### Heavy/blocking transports need their own task

`ssh.ino` runs the entire connect-through-teardown sequence
(`sshConnectAndRun`) on a dedicated FreeRTOS task with a 40 KB stack
(`xTaskCreatePinnedToCore`), because `mbedtls`'s key exchange overflows the
default ~8 KB loop-task stack and reboots the board. `handleSshCommand` blocks
on a `volatile bool` flag until that task finishes, so the loop task's own
`M5Cardputer.update()`/drawing calls never run concurrently with the session.
Any new feature pulling in a similarly heavy crypto/TLS/parsing library should
follow this same shape rather than calling straight from a command handler.

## 9. Storage API (`storage.ino`)

DOLL-OS presents one unified path namespace over two physical filesystems:
LittleFS at the root, and the SD card mounted at `/sd`.

```cpp
void initStorage();   // called once from setup(); mounts LittleFS + SD
```

```cpp
String resolvePath(const String& cwd, const String& inputPath);
```
Pure string math: collapses `inputPath` (relative or absolute) against `cwd`
into a clean absolute path in the unified namespace, resolving `.`/`..`. Knows
nothing about which filesystem owns the result.

```cpp
RoutedPath routePath(const String& resolvedPath);
// struct RoutedPath { fs::FS* fs; String realPath; bool isSd; };
```
Maps an absolute unified path onto the physical filesystem that owns it —
anything at/under `/sd` routes to `SD` with the mount prefix stripped,
everything else routes to `LittleFS`.

```cpp
bool directoryExists(const String& resolvedPath);
void listDirectory(fs::FS& fs, const String& path, bool showSdMount);
```

**Pattern for any new command that touches files:** always go
`resolvePath(cwd, userInput)` → `routePath(resolved)` → check
`r.isSd && !sdCardMounted` → then use the standard Arduino `FS`/`File` API
(`r.fs->open(r.realPath, mode)`, `.read()`, `.write()`, `.close()`) — this is
exactly what `handleLsCommand`/`handleCdCommand`/`handlePwdCommand` already
do, and it's what keeps a path like `/sd/foo.txt` transparently landing on the
SD card instead of LittleFS.

## 10. Networking APIs

### Wi-Fi (`wifi.ino`)
```cpp
void scanWifiNetworks();                                          // blocking scan, prints results
void showWifiStatus();                                            // prints current connection info
void connectWifiNetwork(const String& ssid, const String& password); // blocking, ~15s timeout
bool saveWifiCredentials(const String& ssid, const String& password); // writes /wifi.cfg on LittleFS
bool loadWifiCredentials(String& ssid, String& password);
```
All of these just wrap the standard `WiFi` library and print through
`addWrappedHistoryLine`; call them directly from a new command if you need
connectivity, or just check `WiFi.status() == WL_CONNECTED` yourself the way
every existing networked command does before doing anything blocking.

### IP tools (`ip.ino`)
```cpp
void ipShowInfo();                        // local IP/gateway/subnet/MAC/DNS
void ipScanNetwork();                     // blocking ping sweep of the /24
void ipArpScan();                         // blocking ARP scan via esp32ARP
void ipComputeRange(byte net[4], byte bcast[4]);   // subnet math helper
String formatMacAddr(const uint8_t* addr);          // "AA:BB:CC:DD:EE:FF"
```

### Ping (`ping.ino`)
No standalone helper beyond the command handler — it calls the `ESP32Ping`
library's global `Ping` object directly (`Ping.ping(address, count)`,
`Ping.averageTime()`). Do the same from your own code if you need a reachability
check.

## 11. Command-line parsing & history (`CommandProcessor.ino`)

```cpp
int splitCommand(const String& input, String parts[], int maxParts);
```
Space-delimited tokenizer, trims leading/trailing whitespace, stops at
`maxParts` tokens. Tokens wrapped in `"..."` or `'...'` are treated as a
single argument (quotes stripped, spaces inside preserved) — e.g.
`ssh "my host" user` yields `my host` as one token. An unterminated quote
just consumes the rest of the input as that token. The shell calls this
with `maxParts = 8`.

```cpp
void addCommandHistory(const String& cmd);
void recallCommandHistory(int step, String& text);
```
Back the shell's `Ctrl+;`/`Ctrl+.` up/down recall (`COMMAND_HISTORY_MAX = 30`
entries, oldest evicted first). Only relevant if you're building something
that wants shell-style history recall of its own.

> Why `RoutedPath`, `AnsiFilterState`, and `TerminalStreamState` live in
> `global.h` instead of the `.ino` that actually uses each one: the Arduino
> IDE concatenates every `.ino` file and hoists every function prototype to
> the top of the combined sketch, *before* any file's own `#include`s run. A
> struct/class used as a parameter type in any hoisted function must already
> be visible at that point, so it has to be declared in `global.h`, not in the
> file where it's conceptually owned. Keep this in mind if a new type needs to
> appear in a function signature.

## 12. USB Mass Storage (`usb_msc.ino`)

Not really a reusable API — it's a special-cased modal mode that exposes the
raw SD card block device to a host PC over TinyUSB MSC
(`onMscRead`/`onMscWrite`/`onMscStartStop` wired to `SD.readRAW`/`writeRAW`).
Blocks until `Fn+\`` is pressed. Worth knowing about mainly so a new command
doesn't try to touch the SD card while USB mode is active.

## 13. Build-time config (`config.h`)

Currently just Motoko's MQTT defaults (`MOTOKO_DEFAULT_BROKER`,
`MOTOKO_DEFAULT_PORT`, `MOTOKO_CLIENT_ID`). If your app needs constants that
might change per-deployment, this is the intended place for them — the file
header notes it's "reserved for future settings."

## 14. Constraints worth knowing before you start

- **Everything is single-threaded and cooperative** outside the one
  dedicated-task exception (§8). A blocking call in your command handler
  freezes keyboard and display until it returns — that's the norm here, not a
  bug to route around. Just redraw (`drawTerminalHistory()`) before you start
  one so the user sees your "starting..." message first.
- **4-token command line, hard cap.** `splitCommand(entered, parts, 4)` is
  called once, in `commandProcessor()`. A command needing more positional
  arguments must parse them itself out of `parts[1..3]`, or that one call site
  has to change globally.
- **Command dispatch is exact-match, case-sensitive, linear scan** over a
  small static table — no aliases, no prefix matching.
- **New cross-file types go in `global.h`**, not their owning `.ino` (§11).
- **`ssh.ino` does not verify host keys** (no known-hosts store) — the file's
  own comments flag this as fine for trusted-LAN use only, not a general
  security boundary.
- **`batteryPercentCheck()`/`batteryVoltCheck()` are empty stubs** — don't
  call them (§5).

## 15. Quick recipe index

| I want to... | Use |
|---|---|
| Print a line of output | `addWrappedHistoryLine(text[, color])` |
| Add a new one-shot command | Write `handleFooCommand`, add to `commandTable[]` + help text |
| Add subcommands to a command | Branch on `parts[1]` inside the handler (see `wifi`/`ip`) |
| Build a full-screen local-input app | Follow `motoko.ino`'s state+`runFooBlocking()` pattern (§7) |
| Build a raw socket/pty client | Subclass `RemoteSession`, call `.run()` (§8) |
| Stream colored text from a byte source live | `AnsiFilterState` + `ansiFilterByte` → `TerminalStreamState` + `terminalStream*` (§8) |
| Read/write a file in the unified namespace | `resolvePath` → `routePath` → standard `FS`/`File` API (§9) |
| Check/require Wi-Fi before doing something | `WiFi.status() == WL_CONNECTED`, else print and `return` |
| Run something too heavy for the default stack | Dedicated FreeRTOS task, see `sshConnectAndRun`/`sshTaskEntry` (§8) |

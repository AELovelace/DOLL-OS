//   AppRunner.ino
//   tiny executable script runtime, ported from DS. Apps are plain text .dapp files
//   stored in /apps on LittleFS or /sd/apps on the SD card, then launched from the
//   shell with "run". The format is intentionally small: a few display/shell commands,
//   numeric variables, labels, and jumps. See docs/DAPP.md.
//
//   Ported near-verbatim -- the language is display-agnostic, so the changes are the SD
//   library (SD/SPI here, SD_MMC on DS), appDelay()'s idea of what "keep the system alive
//   while waiting" means, and appReadInput(), which reads the built-in keyboard instead of
//   alternating DS's two byte sources. See docs/PORT-FROM-DS.md, Phase 3.
//
//   Kept in sync with DS as of 2026-07-30: string variables (SETSTR/APPEND/INPUT), RAND,
//   and IFEQ/IFNE all landed in DS after this file was first ported across. Only the caps
//   differ, and Fn+Q is local -- see DAPP_MAX_STRING_LEN and appCancelRequested().
#include <LittleFS.h>
#include <FS.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

const int DAPP_MAX_LINES = 160;
const int DAPP_MAX_LABELS = 32;
const int DAPP_MAX_VARS = 16;
const int DAPP_MAX_STRING_VARS = 8;
//   DS caps string variables at 512 characters. Its slab lives in PSRAM; these are plain
//   heap Strings on a machine with ~215KB free and a fragmentation problem, so 8 of them
//   at 512 is 4KB of churn for text this terminal can't show anyway -- the panel is 38
//   characters wide, so 128 is already three and a half wrapped rows.
const int DAPP_MAX_STRING_LEN = 128;
const int DAPP_MAX_STEPS = 4000;

static bool endsWithIgnoreCase(String value, const String& suffix) {
    value.toLowerCase();
    String s = suffix;
    s.toLowerCase();
    return value.endsWith(s);
}

static String trimCopy(String value) {
    value.trim();
    return value;
}

static String stripMatchingQuotes(String value) {
    value.trim();
    if (value.length() >= 2) {
        char first = value[0];
        char last = value[value.length() - 1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substring(1, value.length() - 1);
        }
    }
    return value;
}

static bool isAppCommentOrBlank(const String& rawLine) {
    String line = trimCopy(rawLine);
    return line.length() == 0 || line.startsWith("#") || line.startsWith("//");
}

static int appColorByName(String name) {
    name.trim();
    name.toLowerCase();
    if (name == "black") return C_BLACK;
    if (name == "red") return C_RED;
    if (name == "green") return C_GREEN;
    if (name == "yellow") return C_YELLOW;
    if (name == "blue") return C_BLUE;
    if (name == "magenta") return C_MAGENTA;
    if (name == "cyan") return C_CYAN;
    if (name == "pink") return C_PINK;
    return C_WHITE;
}

static bool appOpenResolvedFile(const String& resolved, File& outFile) {
    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        return false;
    }

    File f = r.fs->open(r.realPath, "r");
    if (!f || f.isDirectory()) {
        if (f) {
            f.close();
        }
        return false;
    }

    outFile = f;
    return true;
}

static bool appOpenCandidate(const String& candidate, File& outFile, String& resolvedOut) {
    String resolved = resolvePath(cwd, candidate);
    if (appOpenResolvedFile(resolved, outFile)) {
        resolvedOut = resolved;
        return true;
    }

    if (!endsWithIgnoreCase(resolved, ".dapp") && appOpenResolvedFile(resolved + ".dapp", outFile)) {
        resolvedOut = resolved + ".dapp";
        return true;
    }

    return false;
}

//"run hello" searches the two app directories before treating the name as a path, so an
//app can be launched from anywhere without typing where it lives
static bool appOpenByName(const String& target, File& outFile, String& resolvedOut) {
    if (target.indexOf('/') >= 0) {
        return appOpenCandidate(target, outFile, resolvedOut);
    }

    String name = target;
    if (!endsWithIgnoreCase(name, ".dapp")) {
        name += ".dapp";
    }

    const String candidates[] = {
        "/sd/apps/" + name,
        "/apps/" + name,
        target,
    };

    for (int i = 0; i < 3; i++) {
        if (appOpenCandidate(candidates[i], outFile, resolvedOut)) {
            return true;
        }
    }
    return false;
}

static void ensureAppDirectories() {
    File flashApps = LittleFS.open("/apps");
    if (!flashApps || !flashApps.isDirectory()) {
        if (flashApps) {
            flashApps.close();
        }
        LittleFS.mkdir("/apps");
    } else {
        flashApps.close();
    }

    if (sdCardMounted) {
        File sdApps = SD.open("/apps");
        if (!sdApps || !sdApps.isDirectory()) {
            if (sdApps) {
                sdApps.close();
            }
            SD.mkdir("/apps");
        } else {
            sdApps.close();
        }
    }
}

static void listAppsInDir(fs::FS& fs, const String& realPath, const String& label) {
    File dir = fs.open(realPath);
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        outLine(label + ": (missing)");
        return;
    }

    outLine(label + ":", C_CYAN);
    File entry = dir.openNextFile();
    int count = 0;
    while (entry) {
        String name = entry.name();
        if (!entry.isDirectory() && endsWithIgnoreCase(name, ".dapp")) {
            outLine("  " + name + "  " + String(entry.size()) + "b");
            count++;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    if (count == 0) {
        outLine("  (none)");
    }
    dir.close();
}

void handleAppsCommand(const String parts[], int partCount) {
    ensureAppDirectories();
    outLine("Apps live in /sd/apps, or /apps on flash.", C_CYAN);
    if (sdCardMounted) {
        listAppsInDir(SD, "/apps", "/sd/apps");
    } else {
        outLine("/sd/apps: SD not mounted", C_YELLOW);
    }
    listAppsInDir(LittleFS, "/apps", "/apps");
}

static int appFindLabel(const DappLabel labels[], int labelCount, String name) {
    name.trim();
    if (name.startsWith(":")) {
        name.remove(0, 1);
    }
    for (int i = 0; i < labelCount; i++) {
        if (labels[i].name == name) {
            return labels[i].lineIndex;
        }
    }
    return -1;
}

static int appFindVar(DappVar vars[], const String& name) {
    for (int i = 0; i < DAPP_MAX_VARS; i++) {
        if (vars[i].used && vars[i].name == name) {
            return i;
        }
    }
    return -1;
}

static int appEnsureVar(DappVar vars[], const String& name) {
    int existing = appFindVar(vars, name);
    if (existing >= 0) {
        return existing;
    }
    for (int i = 0; i < DAPP_MAX_VARS; i++) {
        if (!vars[i].used) {
            vars[i].used = true;
            vars[i].name = name;
            vars[i].value = 0;
            return i;
        }
    }
    return -1;
}

static int appFindStringVar(DappStringVar vars[], const String& name) {
    for (int i = 0; i < DAPP_MAX_STRING_VARS; i++) {
        if (vars[i].used && vars[i].name == name) {
            return i;
        }
    }
    return -1;
}

static int appEnsureStringVar(DappStringVar vars[], const String& name) {
    int existing = appFindStringVar(vars, name);
    if (existing >= 0) {
        return existing;
    }
    for (int i = 0; i < DAPP_MAX_STRING_VARS; i++) {
        if (!vars[i].used) {
            vars[i].used = true;
            vars[i].name = name;
            vars[i].value = "";
            vars[i].value.reserve(48);   //DS reserves 80; one wrapped terminal row is ~38 chars here
            return i;
        }
    }
    return -1;
}

static void appSetStringValue(DappStringVar vars[], int slot, const String& value) {
    vars[slot].value = value.substring(0, DAPP_MAX_STRING_LEN);
}

static bool appIsInteger(const String& token) {
    if (token.length() == 0) {
        return false;
    }
    int start = (token[0] == '-' || token[0] == '+') ? 1 : 0;
    if (start >= token.length()) {
        return false;
    }
    for (int i = start; i < token.length(); i++) {
        if (!isDigit(token[i])) {
            return false;
        }
    }
    return true;
}

static bool appIsNameChar(char ch) {
    return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || ch == '_';
}

static long appBuiltinValue(String name) {
    name.trim();
    name.toLowerCase();
    if (name == "battery") return readBatteryPercent();
    if (name == "heap") return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (name == "millis") return millis();
    if (name == "seconds") return millis() / 1000;
    if (name == "wifi") return wifiIsConnected() == 1 ? 1 : 0;
    return 0;
}

static long appValueOf(String token, DappVar vars[]) {
    token.trim();
    token = stripMatchingQuotes(token);
    if (token.startsWith("$")) {
        token.remove(0, 1);
    }
    if (appIsInteger(token)) {
        return token.toInt();
    }
    int slot = appFindVar(vars, token);
    if (slot >= 0) {
        return vars[slot].value;
    }
    return appBuiltinValue(token);
}

static String appStringValueOf(String token, DappVar vars[], DappStringVar stringVars[]) {
    token.trim();
    if (token.startsWith("$")) {
        token.remove(0, 1);
    }
    int stringSlot = appFindStringVar(stringVars, token);
    if (stringSlot >= 0) {
        return stringVars[stringSlot].value;
    }
    int slot = appFindVar(vars, token);
    if (slot >= 0) {
        return String(vars[slot].value);
    }

    String lowered = token;
    lowered.toLowerCase();
    if (lowered == "cwd") return cwd;
    if (lowered == "ip") return WiFi.localIP().toString();
    if (lowered == "battery") return String(readBatteryPercent());
    if (lowered == "heap") return String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (lowered == "millis") return String(millis());
    if (lowered == "seconds") return String(millis() / 1000);
    if (lowered == "wifi") return wifiIsConnected() == 1 ? "1" : "0";
    return "";
}

static String appExpandText(String text, DappVar vars[], DappStringVar stringVars[]) {
    text = stripMatchingQuotes(text);
    String out = "";
    for (int i = 0; i < text.length(); i++) {
        if (text[i] != '$') {
            out += text[i];
            continue;
        }

        int start = i + 1;
        int end = start;
        while (end < text.length() && appIsNameChar(text[end])) {
            end++;
        }
        if (end == start) {
            out += '$';
            continue;
        }

        String name = text.substring(start, end);
        out += appStringValueOf(name, vars, stringVars);
        i = end - 1;
    }
    return out;
}

//   IFEQ/IFNE's operand rule: "$name", or a bare name that matches a live variable, reads as
//   that variable; anything else is text with $substitutions expanded.
//
//   The `quoted` test is near-dead in practice and kept only to stay diffable with DS: both
//   repos' splitCommand() strips the quotes off a token before it ever gets here, so IFEQ
//   $reply "quit" arrives as the bare word quit. It only bites if an app names a variable
//   after a word it also wants to compare against literally -- rare, and DS has it too.
static String appStringOperand(String token, DappVar vars[], DappStringVar stringVars[]) {
    token.trim();
    bool explicitVariable = token.startsWith("$");
    bool quoted = token.length() >= 2 &&
        ((token[0] == '"' && token[token.length() - 1] == '"') ||
         (token[0] == '\'' && token[token.length() - 1] == '\''));

    if (explicitVariable || (!quoted && appFindStringVar(stringVars, token) >= 0) || (!quoted && appFindVar(vars, token) >= 0)) {
        return appStringValueOf(token, vars, stringVars);
    }

    return appExpandText(token, vars, stringVars);
}

//   Fn+Q aborts a running app, matching the "this keystroke is local, get me out" chord ssh
//   and telnet already use (readRawKeyBytes, hardware.ino).
//
//   Peek, don't consume -- and note what "consume" means on this keyboard. isChange() is
//   destructive: it compares the held-key count against a latch and *updates the latch* on
//   the call that reports true (M5Cardputer's Keyboard.cpp), so the first caller after each
//   update() swallows the event and every later caller sees "no change". Calling it here
//   silently ate every keystroke readKeyboard() was about to read, which looked like an INPUT
//   prompt that wouldn't type.
//
//   isPressed() and keysState() have no such latch: updateKeysState() rebuilds the whole state
//   from the currently-held keys on every M5Cardputer.update(), so this reads the live chord and
//   leaves the change flag for readKeyboard() below. Level-triggered rather than edge-triggered,
//   which is what you want from an abort anyway -- holding the chord cancels.
//
//   DS has no equivalent: it can always drop the telnet session. Here an app owns the device
//   until it returns, so without this a WAIT loop or an INPUT prompt is only escapable by
//   power-cycling -- which is also what made DS's 4000-step budget unsafe to port before now.
static bool appCancelRequested() {
    if (!M5Cardputer.Keyboard.isPressed()) {
        return false;
    }
    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    return keys.fn && keysContainChar(keys, 'q');
}

//   WAIT/SLEEP's pause. An app blocks loop(), so anything loop() would normally do has to
//   happen here instead or it stops happening for the duration. DS pumped its FTP server,
//   radio task and display frame; the equivalents here are the M5 device update and the two
//   sprite redraws, so the status bar keeps ticking and a long WAIT doesn't look like a hang.
//
//   Returns false if the user asked to abort. Deliberately still absent: keyboardLogic().
//   Reading the keyboard properly here would let a keystroke land in currentCommand mid-app
//   and then get submitted the moment the app exits; appCancelRequested() only peeks.
static bool appDelay(unsigned long waitMs) {
    unsigned long started = millis();
    while (millis() - started < waitMs) {
        M5Cardputer.update();
        if (appCancelRequested()) {
            return false;
        }
        statusManagement();
        drawTerminalHistory();
        ftpService();
        maintainInternetConnection();
        delay(1);
    }
    return true;
}

//   INPUT's prompt. DS alternates its two byte sources here (telnet line editor, then the
//   keyboard one); with the telnet server cut this collapses to the same shape as
//   RemoteSession::runInlineCommandPrompt() -- a readKeyboard() loop over a private buffer,
//   with everything loop() would have done pumped by hand.
//
//   Returns false if the user aborted with Fn+Q, which stops the app.
static bool appReadInput(const String& prompt, String& out) {
    //   Borrow and put back the command-bar globals, exactly as runInlineCommandPrompt does:
    //   readKeyboard() and drawCommandBar() both index into them, and the shell's own cursor
    //   position has to survive an app that prompts.
    int savedCursorPos = commandCursorPos;
    int savedScrollOffset = commandScrollOffset;

    //   A private buffer, not currentCommand: a half-typed answer must not survive into the
    //   shell's input line when the app exits.
    String input = "";
    commandCursorPos = 0;
    commandScrollOffset = 0;

    bool cancelled = false;
    bool submitted = false;
    while (!submitted) {
        M5Cardputer.update();

        if (appCancelRequested()) {
            cancelled = true;
            break;
        }

        submitted = readKeyboard(input);

        statusManagement();
        ftpService();
        maintainInternetConnection();
        drawTerminalHistory();
        drawCommandBar(prompt, input);
        delay(1);
    }

    commandCursorPos = savedCursorPos;
    commandScrollOffset = savedScrollOffset;

    if (cancelled) {
        return false;
    }

    input.trim();
    outLine(prompt + input, C_CYAN);   //the command bar is transient, so echo what was answered into the scrollback
    out = input;
    return true;
}

//   RAND's generator. esp_random() rather than random(): the Arduino PRNG is seeded
//   identically every boot, so "RAND roll 1 6" would deal the same sequence to a freshly
//   powered device every time.
static long appRandomRange(long low, long high) {
    if (high < low) {
        long tmp = low;
        low = high;
        high = tmp;
    }

    uint32_t span = (uint32_t)(high - low + 1);
    if (span == 0) {
        return low;
    }
    return low + (long)(esp_random() % span);
}

static bool appCompare(long left, const String& op, long right) {
    if (op == "==" || op == "=") return left == right;
    if (op == "!=" || op == "<>") return left != right;
    if (op == ">") return left > right;
    if (op == "<") return left < right;
    if (op == ">=") return left >= right;
    if (op == "<=") return left <= right;
    return false;
}

//reads the whole file into the line array, recording label positions as it goes so GOTO
//can jump forward to a label it hasn't executed past yet
static bool appLoad(File& file, DappLine lines[], int& lineCount, DappLabel labels[], int& labelCount) {
    lineCount = 0;
    labelCount = 0;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.endsWith("\r")) {
            line.remove(line.length() - 1);
        }

        if (lineCount >= DAPP_MAX_LINES) {
            outLine("run: too many app lines (max " + String(DAPP_MAX_LINES) + ")", C_RED);
            return false;
        }

        String trimmed = trimCopy(line);
        if (!isAppCommentOrBlank(trimmed)) {
            if (trimmed.startsWith(":") && labelCount < DAPP_MAX_LABELS) {
                labels[labelCount++] = { trimmed.substring(1), lineCount };
            } else {
                String op = trimmed;
                int space = op.indexOf(' ');
                if (space >= 0) {
                    op = op.substring(0, space);
                }
                op.toUpperCase();
                if (op == "LABEL" && space >= 0 && labelCount < DAPP_MAX_LABELS) {
                    labels[labelCount++] = { trimCopy(trimmed.substring(space + 1)), lineCount };
                }
            }
        }

        lines[lineCount++].text = line;
    }

    return true;
}

static bool appExecute(DappLine lines[], int lineCount, DappLabel labels[], int labelCount) {
    DappVar vars[DAPP_MAX_VARS] = {};
    DappStringVar stringVars[DAPP_MAX_STRING_VARS] = {};
    int pc = 0;
    int steps = 0;
    int color = C_WHITE;

    while (pc >= 0 && pc < lineCount) {
        //a step budget rather than a time budget, as the backstop for a runaway loop. Fn+Q is
        //the interactive way out, but it's only sampled where an app pauses (WAIT and INPUT --
        //see appCancelRequested), so a tight loop with neither still needs this.
        if (++steps > DAPP_MAX_STEPS) {
            outLine("run: stopped after " + String(DAPP_MAX_STEPS) + " steps (possible loop)", C_RED);
            return false;
        }

        String line = trimCopy(lines[pc].text);
        pc++;
        if (isAppCommentOrBlank(line) || line.startsWith(":")) {
            continue;
        }

        int space = line.indexOf(' ');
        String op = (space >= 0) ? line.substring(0, space) : line;
        String arg = (space >= 0) ? trimCopy(line.substring(space + 1)) : "";
        op.toUpperCase();

        if (op == "LABEL") {
            continue;
        } else if (op == "PRINT" || op == "ECHO") {
            outLine(appExpandText(arg, vars, stringVars), color);
        } else if (op == "COLOR") {
            color = appColorByName(arg);
        } else if (op == "CLEAR" || op == "CLS") {
            outClearScreen();
        } else if (op == "WAIT" || op == "SLEEP") {
            if (!appDelay((unsigned long)appValueOf(arg, vars))) {
                outLine("run: cancelled", C_YELLOW);
                return false;
            }
        } else if (op == "SET") {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: SET needs <name> <value>", C_RED);
                return false;
            }
            int slot = appEnsureVar(vars, parts[0]);
            if (slot < 0) {
                outLine("run: too many variables", C_RED);
                return false;
            }
            vars[slot].value = appValueOf(parts[1], vars);
        } else if (op == "SETSTR") {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: SETSTR needs <name> <text>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            appSetStringValue(stringVars, slot, appExpandText(parts[1], vars, stringVars));
        } else if (op == "APPEND") {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: APPEND needs <name> <text>", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            appSetStringValue(stringVars, slot, stringVars[slot].value + appExpandText(parts[1], vars, stringVars));
        } else if (op == "INPUT") {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 1) {
                outLine("run: INPUT needs <name> [prompt]", C_RED);
                return false;
            }
            int slot = appEnsureStringVar(stringVars, parts[0]);
            if (slot < 0) {
                outLine("run: too many string variables", C_RED);
                return false;
            }
            String prompt = count >= 2 ? appExpandText(parts[1], vars, stringVars) : parts[0] + "> ";
            String reply;
            if (!appReadInput(prompt, reply)) {
                outLine("run: cancelled", C_YELLOW);
                return false;
            }
            appSetStringValue(stringVars, slot, reply);
        } else if (op == "ADD") {
            String parts[2];
            int count = splitCommand(arg, parts, 2);
            if (count < 2) {
                outLine("run: ADD needs <name> <value>", C_RED);
                return false;
            }
            int slot = appEnsureVar(vars, parts[0]);
            if (slot < 0) {
                outLine("run: too many variables", C_RED);
                return false;
            }
            vars[slot].value += appValueOf(parts[1], vars);
        } else if (op == "RAND") {
            String parts[3];
            int count = splitCommand(arg, parts, 3);
            if (count < 2) {
                outLine("run: RAND needs <name> <max> or <name> <min> <max>", C_RED);
                return false;
            }
            int slot = appEnsureVar(vars, parts[0]);
            if (slot < 0) {
                outLine("run: too many variables", C_RED);
                return false;
            }

            if (count == 2) {
                long maxExclusive = appValueOf(parts[1], vars);
                if (maxExclusive <= 0) {
                    outLine("run: RAND max must be greater than 0", C_RED);
                    return false;
                }
                vars[slot].value = appRandomRange(0, maxExclusive - 1);
            } else {
                vars[slot].value = appRandomRange(appValueOf(parts[1], vars), appValueOf(parts[2], vars));
            }
        } else if (op == "GOTO") {
            int target = appFindLabel(labels, labelCount, arg);
            if (target < 0) {
                outLine("run: label not found: " + arg, C_RED);
                return false;
            }
            pc = target;
        } else if (op == "IF") {
            String parts[5];
            int count = splitCommand(arg, parts, 5);
            String jumpOp = (count >= 4) ? parts[3] : "";
            jumpOp.toUpperCase();
            if (count < 5 || jumpOp != "GOTO") {
                outLine("run: IF syntax is IF <left> <op> <right> GOTO <label>", C_RED);
                return false;
            }
            if (appCompare(appValueOf(parts[0], vars), parts[1], appValueOf(parts[2], vars))) {
                int target = appFindLabel(labels, labelCount, parts[4]);
                if (target < 0) {
                    outLine("run: label not found: " + parts[4], C_RED);
                    return false;
                }
                pc = target;
            }
        } else if (op == "IFEQ" || op == "IFNE") {
            String parts[4];
            int count = splitCommand(arg, parts, 4);
            String jumpOp = (count >= 3) ? parts[2] : "";
            jumpOp.toUpperCase();
            if (count < 4 || jumpOp != "GOTO") {
                outLine("run: " + op + " syntax is " + op + " <left> <right> GOTO <label>", C_RED);
                return false;
            }
            bool equal = appStringOperand(parts[0], vars, stringVars) == appStringOperand(parts[1], vars, stringVars);
            if ((op == "IFEQ" && equal) || (op == "IFNE" && !equal)) {
                int target = appFindLabel(labels, labelCount, parts[3]);
                if (target < 0) {
                    outLine("run: label not found: " + parts[3], C_RED);
                    return false;
                }
                pc = target;
            }
        } else if (op == "EXIT" || op == "END") {
            return true;
        } else {
            outLine("run: unknown app command: " + op, C_RED);
            return false;
        }
    }

    return true;
}

void handleRunCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: run <app|path.dapp>");
        outLine("Put apps in /sd/apps or /apps, then run <name>.");
        return;
    }

    File file;
    String resolved;
    if (!appOpenByName(parts[1], file, resolved)) {
        outLine("run: app not found: " + parts[1], C_RED);
        outLine("Try 'apps' to list installed .dapp files.");
        return;
    }

    //   static, not stack. DS declares these as locals, but 160 DappLine + 32 DappLabel is
    //   ~3.2KB of String headers in one frame, and the Arduino loopTask only gets 8KB total
    //   (CONFIG_ARDUINO_LOOP_STACK_SIZE) -- which this is already several frames deep into.
    //   In .bss it costs the same 3.2KB always instead of risking a stack overflow, and the
    //   Strings sit empty between runs so nothing is held on the heap. Safe because "run"
    //   is not re-entrant: there is no RUN opcode, so an app can't launch another app.
    static DappLine lines[DAPP_MAX_LINES];
    static DappLabel labels[DAPP_MAX_LABELS];
    int lineCount = 0;
    int labelCount = 0;

    outLine("Running " + resolved, C_GREEN);
    bool loaded = appLoad(file, lines, lineCount, labels, labelCount);
    file.close();
    if (!loaded) {
        return;
    }

    bool ok = appExecute(lines, lineCount, labels, labelCount);
    outLine(ok ? "[app exited]" : "[app stopped]", ok ? C_GREEN : C_RED);
}

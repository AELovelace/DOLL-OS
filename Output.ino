//   Output.ino
//   The single line-output entry point for the shell, and the seam that lets code
//   ported from DS (../DS) drop in unchanged.
//
//   DS replaced DOLL-OS's sprite terminal with a telnet socket, and in doing so
//   renamed addWrappedHistoryLine(text[, color]) to outLine(text[, color]) and
//   swapped the per-row uint16_t color for an ANSI SGR integer. Every feature DS
//   grew since the fork -- the filesystem commands, the .dapp runtime, the editor --
//   is written against that spelling. Rather than rewrite each call site on the way
//   back, outLine() adopts DS's signature and forwards to DOLL-OS's existing
//   pixel-wrapped history. There is no socket here and no second surface: this is a
//   rename plus a color-space conversion, nothing more.
//
//   See docs/PORT-FROM-DS.md, Phase 2.

//maps an ANSI SGR foreground code (C_*, global.h) onto the M5GFX color the sprite
//actually draws with. Ported from DS's Display.ino, retargeted from TFT_eSPI's
//TFT_* constants to M5GFX's equivalents.
uint16_t ansiCodeToPixelColor(int code) {
    switch (code) {
        case C_BLACK:   return BLACK;
        case C_RED:     return RED;
        case C_GREEN:   return GREEN;
        case C_YELLOW:  return YELLOW;
        case C_BLUE:    return BLUE;
        case C_MAGENTA: return MAGENTA;
        case C_CYAN:    return CYAN;
        case C_PINK:    return PINK;
        default:        return WHITE;
    }
}

void outLine(const String& text) {
    outLine(text, C_WHITE);
}

void outLine(const String& text, int color) {
    addWrappedHistoryLine(text, ansiCodeToPixelColor(color));
}

//clears the terminal history. Was inline in commandProcessor()'s "clear" branch;
//lifted here so the .dapp runtime's CLEAR opcode and any future caller share it.
void outClearScreen() {
    historyCount = 0;
    historyHead = 0;
    scrollOffset = 0;
    terminalOpenRowOwner = nullptr;
}

//   The shell prompt, path-aware like a real shell ("/sd/apps > "). Single source of
//   truth for the two places a prompt appears -- the command bar (drawCommandBar's
//   prompt argument, from loop()) and the echo of each submitted command
//   (echoCommandLine below) -- so both always agree on where "here" is. Read fresh at
//   every call site rather than cached, since "cd" moves cwd underneath it.
//
//   Shorter cap than DS's 24: this panel is 240px wide, so the command bar holds
//   about 40 characters at the 6px GLCD font. A 24-character prompt would leave 16
//   for typing. 14 keeps "/sd/apps > " intact while leaving most of the bar editable.
const int SHELL_PROMPT_PATH_MAX = 14;

String shellPrompt() {
    String path = cwd;
    if (path.length() > SHELL_PROMPT_PATH_MAX) {
        //keep the tail -- the deepest segments are the ones that say where you are
        path = "..." + path.substring(path.length() - (SHELL_PROMPT_PATH_MAX - 3));
    }
    return path + " > ";
}

//echoes a submitted command into the scrollback so the transcript reads like a real
//shell instead of showing output with no record of what asked for it. The command bar
//is cleared on submit, so unlike a real terminal the typed line leaves no trace of its
//own -- this is what puts it in the history.
void echoCommandLine(const String& entered) {
    addWrappedHistoryLine(shellPrompt() + entered, ansiCodeToPixelColor(C_CYAN));
}

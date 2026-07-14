//   ansi.ino
//   Minimal ANSI/VT100 escape-sequence filter + UTF-8 sanitizer for remote text
//   streams (ssh.ino, telnet.ino). DOLL-OS's terminal is a scrolling line history, not a
//   grid-addressable screen, so most cursor-motion sequences are consumed and dropped rather
//   than emulated -- this is what was leaking through as raw "[m[m"-style garbage before. SGR
//   (color) sequences are interpreted, since historyLines already carries a color per row, and
//   so is CSI 'K' (erase in line): remote software commonly pairs a bare '\r' with it to redraw
//   the current line in place (e.g. a chat relay overwriting a user's prompt when someone else's
//   message arrives) -- dropping that erase silently used to leave stale trailing text behind.
//
//   Colors are per-row, not per-character, so a color change mid-line only takes
//   effect for whichever row is flushed after it -- a deliberate simplification,
//   not a bug, given the row-based history model.

//maps a subset of SGR (Select Graphic Rendition) parameters to the display's 16-bit
//colors. Only foreground colors + reset are handled -- bold/underline/background/etc.
//have no representation in a single-color-per-row model, so they're accepted (so the
//sequence still gets consumed) and otherwise ignored.
static uint16_t ansiSgrColor(int code, uint16_t defaultColor) {
    switch (code) {
        case 0:               return defaultColor;   //reset
        case 30: case 90:     return BLACK;
        case 31: case 91:     return RED;
        case 32: case 92:     return GREEN;
        case 33: case 93:     return YELLOW;
        case 34: case 94:     return BLUE;
        case 35: case 95:     return MAGENTA;
        case 36: case 96:     return CYAN;
        case 37: case 97:     return WHITE;
        default:              return defaultColor;
    }
}

//applies every ';'-separated SGR parameter in order, since real sequences like
//"\x1b[1;31m" chain several together
static uint16_t ansiApplySgr(const String& params, uint16_t currentColor, uint16_t defaultColor) {
    if (params.length() == 0) {
        return defaultColor;   //bare "\x1b[m" means reset
    }
    uint16_t color = currentColor;
    int start = 0;
    while (start <= (int)params.length()) {
        int sep = params.indexOf(';', start);
        String token = (sep == -1) ? params.substring(start) : params.substring(start, sep);
        int code = token.length() ? token.toInt() : 0;
        color = ansiSgrColor(code, defaultColor);
        if (sep == -1) {
            break;
        }
        start = sep + 1;
    }
    return color;
}

//feeds one incoming byte through the escape/UTF-8 filter.
//returns true and sets outCh to a byte that should be appended to visible text;
//returns false when the byte was consumed as part of an escape sequence, a UTF-8
//continuation byte, or a control character with no display representation.
//color is read (as the "current" color to modify) and written in place; colorChanged
//is set true whenever an SGR sequence just completed, so callers know to re-tag
//whatever row they're building. isBackspace is set true when the byte was BS (0x08) or
//DEL (0x7F) -- a remote erasing a character it previously echoed (e.g. a telnet/BBS server's
//own line editor backing over what you just typed) -- so callers can actually remove that
//character from history instead of silently dropping the byte, which is what made backspace
//look broken during a live telnet/ssh session. eraseToEndOfLine is set true on a CSI 'K'
//(erase in line) sequence -- the 0K/1K/2K parameter distinction isn't tracked, since all three
//are just variants of the same "\r" + erase + redraw-in-place idiom this filter needs to support.
bool ansiFilterByte(AnsiFilterState& st, uint8_t ch, uint16_t defaultColor, uint16_t& color, char& outCh, bool& colorChanged, bool& isBackspace, bool& eraseToEndOfLine) {
    colorChanged = false;
    isBackspace = false;
    eraseToEndOfLine = false;

    switch (st.state) {
        case ANSI_TEXT:
            if (ch == 0x1B) {   //ESC
                st.state = ANSI_ESC;
                return false;
            }
            if (st.utf8Remaining > 0) {
                if ((ch & 0xC0) == 0x80) {   //continuation byte -- swallow it
                    st.utf8Remaining--;
                    return false;
                }
                st.utf8Remaining = 0;   //malformed sequence; fall through and resync on this byte
            }
            if (ch >= 0x80) {
                //start of a multi-byte UTF-8 sequence -- this display's font only covers
                //ASCII, so collapse the whole codepoint into one placeholder rather than
                //showing one wrong glyph per byte
                if ((ch & 0xE0) == 0xC0)      st.utf8Remaining = 1;
                else if ((ch & 0xF0) == 0xE0) st.utf8Remaining = 2;
                else if ((ch & 0xF8) == 0xF0) st.utf8Remaining = 3;
                outCh = '?';
                return true;
            }
            if (ch == 0x08 || ch == 0x7F) {   //BS or DEL
                isBackspace = true;
                return false;
            }
            if (ch == '\t' || (ch >= 0x20 && ch < 0x7F)) {
                outCh = (char)ch;
                return true;
            }
            return false;   //other control bytes (BEL, ...) -- no display representation yet

        case ANSI_ESC:
            if (ch == '[') {
                st.state = ANSI_CSI;
                st.csiParams = "";
            } else if (ch == ']') {
                st.state = ANSI_OSC;
            } else {
                st.state = ANSI_TEXT;   //two-byte escape (ESC(, ESC=, ESC>, ...) -- fully consumed
            }
            return false;

        case ANSI_CSI:
            if (ch >= 0x40 && ch <= 0x7E) {   //final byte ends the sequence
                if (ch == 'm') {
                    color = ansiApplySgr(st.csiParams, color, defaultColor);
                    colorChanged = true;
                } else if (ch == 'K') {
                    eraseToEndOfLine = true;
                }
                st.state = ANSI_TEXT;
            } else {
                st.csiParams += (char)ch;   //parameter/intermediate byte, e.g. '1', ';', '3', '1'
            }
            return false;

        case ANSI_OSC:
            if (ch == 0x07) {   //BEL terminates OSC
                st.state = ANSI_TEXT;
            } else if (ch == 0x1B) {
                st.state = ANSI_OSC_ESC;
            }
            return false;

        case ANSI_OSC_ESC:
            st.state = (ch == '\\') ? ANSI_TEXT : ANSI_OSC;   //ESC \ (ST) terminates; otherwise keep eating OSC
            return false;
    }

    return false;
}

//   Edit.ino
//   "edit" app -- a full-screen nano-style text editor, ported from DS. Like DS's
//   version this is a full-screen takeover: `edit <file>` seizes loop() and runs its
//   own inner loop until the user quits.
//
//   Three things differ from DS, all forced by the hardware:
//
//   1. Caps are much smaller. DS keeps the file as a flat 128KB slab in PSRAM; the
//      StampS3 has no PSRAM at all, so this is 16KB of internal RAM. The flat-slab
//      design is kept -- in fact it matters *more* here. A per-line String array puts
//      every line's characters on the heap as its own small allocation, which is
//      exactly the fragmentation this board can least afford. One slab plus an offset
//      index is a single allocation that either succeeds or fails cleanly.
//
//   2. There is no byte decoder. DS drives the editor from a telnet socket and a BLE
//      keyboard bridge, so it parses ESC/CSI sequences into logical keys. Here the
//      keyboard is physical and M5Cardputer hands over an already-decoded KeysState,
//      so editKeyFromKeyboard() below replaces ~100 lines of escape-sequence parsing.
//
//   3. There is no second surface. DS renders the same grid twice, once to its panel
//      and once as ANSI to a telnet client. This renders only to the screen -- and
//      reuses the three sprites the shell already owns (status bar -> title, terminal
//      -> text, command bar -> hint) rather than allocating a framebuffer of its own,
//      which on a machine with ~215KB free would be the single largest thing here.
//
//   See docs/PORT-FROM-DS.md, Phase 5.

#include "esp_heap_caps.h"

//   Storage model
//
//   One flat byte slab plus a rebuilt line-offset index. Editing is memmove +
//   reindex; at the 16KB cap that is a ~16KB move plus a ~16KB scan per keystroke,
//   which is microseconds against a sprite push.
static const size_t EDIT_BUF_CAP   = 16 * 1024;
static const int    EDIT_MAX_LINES = 512;
static const int    EDIT_TAB_WIDTH = 4;

static char*    editBuf = nullptr;
static size_t   editLen = 0;             //bytes used in editBuf
static int32_t* editLineStart = nullptr; //byte offset of each line's first char
static int      editLineCount = 0;       //always >= 1; an empty buffer is one empty line
static size_t   editCursor = 0;          //byte offset into editBuf

static int    editTopLine = 0;      //first buffer line shown in the viewport
static int    editLeftCol = 0;      //first *display* column shown (horizontal scroll)
static int    editGoalCol = -1;     //display column Up/Down try to return to; -1 = use current
static bool   editModified = false; //unsaved changes
static bool   editNeedsRender = true;
static String editPathLogical;      //the shell-namespace path, as typed -- shown in the title bar
static String editStatus;           //transient message shown in the hint bar

//Ctrl+K cut buffer. Consecutive presses append, so three in a row lift three lines as
//one block and a single Ctrl+U puts them back -- nano's behavior, and the only reason
//this needs a "was the last key also a cut" flag.
static String editCutBuffer;
static bool   editLastKeyWasCut = false;

//Ctrl+W search term, kept between searches so re-opening the prompt and pressing Enter
//is "find next"
static String editLastSearch;

//   Undo
//
//   Records live below editInsertBytes/editDeleteBytes, the two functions every
//   mutation funnels through, so cut/paste/typing all record for free rather than each
//   feature remembering to. An insert record needs no payload (undoing it is a delete
//   of known length); a delete record keeps the bytes it removed. Runs of typing and of
//   backspacing each coalesce into one record, so 20 keystrokes are one undo step.
//
//   16 steps and a 2KB payload cap, against DS's 64 and 8KB -- undo history is the
//   easiest thing here to trade away for RAM.
static const int    EDIT_UNDO_MAX = 16;
static const size_t EDIT_UNDO_TEXT_MAX = 2 * 1024;   //per-record payload cap

struct EditUndoRec {
    bool   isInsert;
    size_t at;
    size_t len;
    char*  text;         //delete records only; nullptr for inserts
    size_t cursorBefore;
};

static EditUndoRec editUndo[EDIT_UNDO_MAX];
static int  editUndoCount = 0;
static int  editUndoHead = 0;
static bool editUndoSuspended = false;   //true while an undo is being applied, so
                                          //the mutators it calls don't record it
//which kind of run may still be extended by the next matching keystroke:
//0 = none, 1 = a run of typing, 2 = a run of backspacing
static int  editUndoRun = 0;

//editUndoCount as of the last successful save; -1 means "the saved state is no longer
//reachable by undoing" (history was evicted, or new edits diverged from it). Comparing
//against it is what lets undoing back to the saved state clear the modified flag
//instead of leaving a false "unsaved changes" prompt on exit.
static int editSavedUndoDepth = 0;

//viewport geometry, computed once per launch from the sprites
static int editRows = 0;
static int editCols = 0;
static int editCharW = 6;
static int editLineH = 10;

//half-period of the block cursor blink, matching the shell's own feel
static const unsigned long EDIT_CURSOR_BLINK_MS = 500;

//cap on what the modal prompts (filename, search term, line number) will accept
static const int EDIT_PROMPT_MAX = 128;

//   Buffer primitives

static void editReindex() {
    editLineCount = 0;
    editLineStart[editLineCount++] = 0;
    for (size_t i = 0; i < editLen && editLineCount < EDIT_MAX_LINES; i++) {
        if (editBuf[i] == '\n') {
            editLineStart[editLineCount++] = (int32_t)(i + 1);
        }
    }
}

//offset one past the last character of `line` (i.e. at its '\n', or at editLen for
//the final line)
static size_t editLineEnd(int line) {
    if (line + 1 < editLineCount) {
        return (size_t)editLineStart[line + 1] - 1;
    }
    return editLen;
}

static int editLineLen(int line) {
    return (int)(editLineEnd(line) - (size_t)editLineStart[line]);
}

static int editCursorLine() {
    int lo = 0, hi = editLineCount - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if ((size_t)editLineStart[mid] <= editCursor) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

//   Tab handling. A real '\t' is stored in the buffer (so Makefiles and the like
//   survive a round trip) and expanded to the next EDIT_TAB_WIDTH stop only at render
//   time -- which means buffer columns and display columns differ, and every
//   cursor/scroll calculation has to go through this one function.
//
//   Expands `line` into `out`. If bufCol >= 0, returns the display column that buffer
//   column lands on; otherwise returns the line's total display width.
static int editExpandLine(int line, String& out, int bufCol) {
    out = "";
    const size_t start = (size_t)editLineStart[line];
    const size_t end = editLineEnd(line);
    int mark = -1;

    for (size_t i = start; i < end; i++) {
        if ((int)(i - start) == bufCol) {
            mark = (int)out.length();
        }
        char c = editBuf[i];
        if (c == '\t') {
            int pad = EDIT_TAB_WIDTH - ((int)out.length() % EDIT_TAB_WIDTH);
            for (int p = 0; p < pad; p++) out += ' ';
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
            out += c;
        } else {
            out += '?';   //the screen font is ASCII; anything else shows as a placeholder
        }
    }
    if (bufCol >= 0 && mark < 0) {
        mark = (int)out.length();   //cursor sits at (or past) end of line
    }
    return (bufCol >= 0) ? mark : (int)out.length();
}

//display column of the cursor on its own line
static int editCursorDisplayCol() {
    int line = editCursorLine();
    String tmp;
    return editExpandLine(line, tmp, (int)(editCursor - (size_t)editLineStart[line]));
}

//buffer column on `line` whose display column is nearest to `wantDisplayCol` -- the
//inverse of editExpandLine, used by Up/Down so the cursor tracks a visual column
//through tab-indented lines instead of drifting
static int editBufColForDisplayCol(int line, int wantDisplayCol) {
    const int len = editLineLen(line);
    String tmp;
    for (int c = 0; c <= len; c++) {
        if (editExpandLine(line, tmp, c) >= wantDisplayCol) {
            return c;
        }
    }
    return len;
}

//   Undo ring

static int editUndoSlot(int logicalIndex) {
    return (editUndoHead + logicalIndex) % EDIT_UNDO_MAX;
}

static void editUndoFreeSlot(int slot) {
    if (editUndo[slot].text) {
        heap_caps_free(editUndo[slot].text);
        editUndo[slot].text = nullptr;
    }
}

static void editUndoClear() {
    for (int i = 0; i < EDIT_UNDO_MAX; i++) {
        editUndoFreeSlot(i);
    }
    editUndoCount = 0;
    editUndoHead = 0;
    editUndoRun = 0;
    editSavedUndoDepth = -1;
}

//recomputes the modified flag from how far the undo stack sits from the last save
static void editRefreshModified() {
    editModified = (editSavedUndoDepth < 0) || (editUndoCount != editSavedUndoDepth);
}

static char* editUndoAllocText(size_t n) {
    return (char*)heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

//appends a record. `removed` is the payload for delete records (ignored for inserts).
//runKind matches editUndoRun's encoding: pass 0 for an operation that must start its
//own record and end any run in progress.
static void editUndoPush(bool isInsert, size_t at, size_t len, const char* removed,
                         size_t cursorBefore, int runKind) {
    if (editUndoSuspended) {
        return;
    }

    //an op we can't record leaves a hole, and every older record's offsets are only
    //valid if everything after them is replayed first -- so drop the history rather
    //than keep a stack that would corrupt the buffer when applied
    if (!isInsert && len > EDIT_UNDO_TEXT_MAX) {
        editUndoClear();
        editRefreshModified();
        return;
    }

    //extend the record on top instead of pushing, when this is the next keystroke in a
    //run of the same kind and it's contiguous with what's already there
    if (runKind != 0 && runKind == editUndoRun && editUndoCount > 0) {
        const int top = editUndoSlot(editUndoCount - 1);
        EditUndoRec& r = editUndo[top];
        if (isInsert && r.isInsert && at == r.at + r.len) {
            r.len += len;                       //typing forward
            editRefreshModified();
            return;
        }
        if (!isInsert && !r.isInsert && at + len == r.at && r.len + len <= EDIT_UNDO_TEXT_MAX) {
            //backspacing leftward: the newly removed bytes belong in front of what this
            //record already holds, and the record's start moves back
            char* grown = editUndoAllocText(r.len + len);
            if (grown) {
                memcpy(grown, removed, len);
                memcpy(grown + len, r.text, r.len);
                heap_caps_free(r.text);
                r.text = grown;
                r.len += len;
                r.at = at;
                editRefreshModified();
                return;
            }
            //allocation failed -- fall through and push a fresh record instead
        }
    }

    char* payload = nullptr;
    if (!isInsert) {
        payload = editUndoAllocText(len);
        if (!payload) {
            editUndoClear();
            editRefreshModified();
            return;
        }
        memcpy(payload, removed, len);
    }

    int slot;
    if (editUndoCount < EDIT_UNDO_MAX) {
        slot = editUndoSlot(editUndoCount);
        editUndoCount++;
    } else {
        //ring full: the oldest record falls off, so the saved state is no longer
        //reachable by undoing and the depth comparison stops being meaningful
        slot = editUndoHead;
        editUndoFreeSlot(slot);
        editUndoHead = (editUndoHead + 1) % EDIT_UNDO_MAX;
        editSavedUndoDepth = -1;
    }
    //Editing after undoing back past the save point discards the very records that led
    //to it, so the saved content is no longer anywhere on this stack.
    //
    //The comparison reads oddly because editUndoCount was already incremented above:
    //the depth this record was pushed *at* is editUndoCount - 1, and the save is
    //unreachable once that is below it -- editUndoCount - 1 < depth, i.e. the test
    //below. Using '>' here instead would miss the exact case this exists for (save at
    //depth 5, undo to 4, type: the new record #5 is not the saved one) and leave a
    //modified buffer reporting itself clean on exit.
    if (editSavedUndoDepth >= editUndoCount) {
        editSavedUndoDepth = -1;
    }

    editUndo[slot].isInsert = isInsert;
    editUndo[slot].at = at;
    editUndo[slot].len = len;
    editUndo[slot].text = payload;
    editUndo[slot].cursorBefore = cursorBefore;
    editUndoRun = runKind;
    editRefreshModified();
}

//runKind is the undo-coalescing hint: 1 for a keystroke that may extend a run of
//typing, 2 for one that may extend a run of backspacing, 0 for anything that must
//stand as its own undo step (Enter, Tab, paste, forward-delete, cut).
static bool editInsertBytes(const char* s, size_t n, int runKind) {
    if (editLen + n > EDIT_BUF_CAP) {
        editStatus = "Buffer full (16KB limit)";
        return false;
    }
    //a newline that would push past the line index has nowhere to be recorded, and a
    //silently truncated index corrupts every offset after it
    if (editLineCount >= EDIT_MAX_LINES) {
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\n') {
                editStatus = "Line limit reached (" + String(EDIT_MAX_LINES) + ")";
                return false;
            }
        }
    }
    const size_t cursorBefore = editCursor;
    memmove(editBuf + editCursor + n, editBuf + editCursor, editLen - editCursor);
    memcpy(editBuf + editCursor, s, n);
    editLen += n;
    editReindex();
    editUndoPush(true, editCursor, n, nullptr, cursorBefore, runKind);
    editCursor += n;
    return true;
}

static void editDeleteBytes(size_t at, size_t n, int runKind) {
    if (at >= editLen || n == 0) {
        return;
    }
    if (at + n > editLen) {
        n = editLen - at;
    }
    const size_t cursorBefore = editCursor;
    editUndoPush(false, at, n, editBuf + at, cursorBefore, runKind);   //before the bytes go
    memmove(editBuf + at, editBuf + at + n, editLen - at - n);
    editLen -= n;
    if (editCursor >= at + n)      editCursor -= n;
    else if (editCursor > at)      editCursor = at;
    editReindex();
}

//applies the newest record and pops it
static void editApplyUndo() {
    if (editUndoCount == 0) {
        editStatus = "Nothing to undo";
        return;
    }
    const int slot = editUndoSlot(editUndoCount - 1);

    editUndoSuspended = true;
    if (editUndo[slot].isInsert) {
        editDeleteBytes(editUndo[slot].at, editUndo[slot].len, 0);
        editCursor = editUndo[slot].at;
    } else {
        editCursor = editUndo[slot].at;
        editInsertBytes(editUndo[slot].text, editUndo[slot].len, 0);
        editCursor = editUndo[slot].cursorBefore;
    }
    editUndoSuspended = false;

    if (editCursor > editLen) editCursor = editLen;
    editUndoFreeSlot(slot);
    editUndoCount--;
    editUndoRun = 0;
    editGoalCol = -1;
    editRefreshModified();
    editStatus = "Undid 1 change (" + String(editUndoCount) + " left)";
}

//   Movement

static void editScrollToCursor() {
    const int line = editCursorLine();
    if (line < editTopLine)                editTopLine = line;
    if (line >= editTopLine + editRows)    editTopLine = line - editRows + 1;
    if (editTopLine < 0)                   editTopLine = 0;

    const int col = editCursorDisplayCol();
    if (col < editLeftCol)                 editLeftCol = col;
    if (col >= editLeftCol + editCols)     editLeftCol = col - editCols + 1;
    if (editLeftCol < 0)                   editLeftCol = 0;
}

static void editMoveVertical(int delta) {
    const int line = editCursorLine();
    if (editGoalCol < 0) {
        editGoalCol = editCursorDisplayCol();
    }
    int target = line + delta;
    if (target < 0) target = 0;
    if (target >= editLineCount) target = editLineCount - 1;
    editCursor = (size_t)editLineStart[target] + editBufColForDisplayCol(target, editGoalCol);
}

static void editMoveLeft() {
    editGoalCol = -1;
    if (editCursor > 0) editCursor--;
}

static void editMoveRight() {
    editGoalCol = -1;
    if (editCursor < editLen) editCursor++;
}

static void editMoveHome() {
    editGoalCol = -1;
    editCursor = (size_t)editLineStart[editCursorLine()];
}

static void editMoveEnd() {
    editGoalCol = -1;
    editCursor = editLineEnd(editCursorLine());
}

//   Cut / paste

static const size_t EDIT_CUT_MAX = 4 * 1024;

//Ctrl+K: lifts the whole line the cursor is on, regardless of column (nano's default,
//i.e. without "cutfromcursor"). Consecutive presses append, so a run of them takes a
//block out that a single Ctrl+U puts back.
static void editDoCut() {
    const int line = editCursorLine();
    size_t start = (size_t)editLineStart[line];
    size_t end = editLineEnd(line);

    const bool hasNewline = (end < editLen && editBuf[end] == '\n');
    size_t removeFrom = start;
    size_t removeTo = hasNewline ? end + 1 : end;

    if (removeTo == removeFrom) {
        editStatus = "Nothing to cut";
        return;
    }
    //the final line carries no newline of its own, so deleting just its text would
    //leave the line before it terminated by a newline that now ends the buffer -- a
    //dangling empty last line. Take the preceding newline with it instead.
    if (!hasNewline && start > 0 && editBuf[start - 1] == '\n') {
        removeFrom = start - 1;
    }

    if (!editLastKeyWasCut) {
        editCutBuffer = "";
    }
    if (editCutBuffer.length() + (end - start) + 1 > EDIT_CUT_MAX) {
        editStatus = "Cut buffer full (4KB)";
        return;
    }

    //always store the line with a trailing newline, whether or not the buffer had one
    //there -- that's what makes a paste land as a whole line anywhere it goes
    editCutBuffer.reserve(editCutBuffer.length() + (end - start) + 1);
    for (size_t i = start; i < end; i++) {
        editCutBuffer += editBuf[i];
    }
    editCutBuffer += '\n';

    editCursor = removeFrom;
    editDeleteBytes(removeFrom, removeTo - removeFrom, 0);
    editGoalCol = -1;
    editLastKeyWasCut = true;
    editStatus = "Cut buffer: " + String((unsigned long)editCutBuffer.length()) + " bytes";
}

//Ctrl+U: pastes the cut buffer at the cursor
static void editDoUncut() {
    if (editCutBuffer.length() == 0) {
        editStatus = "Cut buffer is empty";
        return;
    }
    if (editInsertBytes(editCutBuffer.c_str(), editCutBuffer.length(), 0)) {
        editStatus = "Pasted " + String((unsigned long)editCutBuffer.length()) + " bytes";
    }
    editGoalCol = -1;
}

//   Search
//
//   Case-insensitive, because the alternative on a 38-column screen is mostly
//   frustration. Returns the match offset or -1.
static long editFindFrom(const char* needle, size_t nlen, size_t from) {
    if (nlen == 0 || nlen > editLen) {
        return -1;
    }
    for (size_t i = from; i + nlen <= editLen; i++) {
        size_t j = 0;
        while (j < nlen &&
               tolower((unsigned char)editBuf[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) {
            return (long)i;
        }
    }
    return -1;
}

//   Key decoding
//
//   This is the whole of what DS spends ~100 lines of ESC/CSI parsing on: the keyboard
//   is physical, so M5Cardputer hands us a decoded KeysState and we only have to map
//   it onto the logical vocabulary.
//
//   Chord choices follow the conventions hardware.ino already established for the
//   shell rather than nano's literal keys, because this keyboard has no arrow cluster,
//   no Home/End/PgUp/PgDn, and no way to type ^_:
//     Fn + ; . , /   arrows          (same as the shell's history/cursor keys)
//     Ctrl + letter  nano's chords   (^O save, ^X exit, ^K cut, ...)
//     Ctrl + L       go to line      (nano's ^_ is untypeable here)
//   Everything else nano binds that needs no new key keeps its usual chord.
static bool editKeyFromKeyboard(EditKey& key, char& ch) {
    key = EK_NONE;
    ch = 0;

    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
        return false;
    }

    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();

    if (keys.fn) {
        if (keysContainChar(keys, ';') || keysContainChar(keys, ':')) { key = EK_UP;    return true; }
        if (keysContainChar(keys, '.') || keysContainChar(keys, '>')) { key = EK_DOWN;  return true; }
        if (keysContainChar(keys, ','))                               { key = EK_LEFT;  return true; }
        if (keysContainChar(keys, '/'))                               { key = EK_RIGHT; return true; }
        return false;
    }

    if (keys.ctrl) {
        for (char c : keys.word) {
            switch (tolower((unsigned char)c)) {
                case 'o': key = EK_SAVE;      return true;
                case 'x': key = EK_EXIT;      return true;
                case 'c': key = EK_CANCEL;    return true;
                case 'a': key = EK_HOME;      return true;
                case 'e': key = EK_END;       return true;
                case 'y': key = EK_PGUP;      return true;
                case 'v': key = EK_PGDN;      return true;
                case 'd': key = EK_DELETE;    return true;
                case 'k': key = EK_CUT;       return true;
                case 'u': key = EK_UNCUT;     return true;
                case 'w': key = EK_SEARCH;    return true;
                case 'l': key = EK_GOTO;      return true;
                case 'g': key = EK_HELP;      return true;
                case 'z': key = EK_UNDO;      return true;
                default: break;
            }
        }
        return false;
    }

    //   Backspace is checked before the word loop and returns immediately: this
    //   keyboard can report a stray character alongside a delete press, which
    //   hardware.ino's readKeyboard() documents and handles the same way.
    if (keys.del) {
        key = EK_BACKSPACE;
        return true;
    }
    if (keys.enter) {
        key = EK_ENTER;
        return true;
    }
    if (keys.tab) {
        key = EK_TAB;
        return true;
    }

    for (char c : keys.word) {
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
            key = EK_CHAR;
            ch = c;
            return true;
        }
    }
    return false;
}

//   pumps the device and pulls at most one logical key. M5Cardputer.update() has to be
//   called here because the editor owns loop() for its whole lifetime -- nothing else
//   is refreshing the key matrix.
static bool editNextKey(EditKey& key, char& ch) {
    M5Cardputer.update();
    return editKeyFromKeyboard(key, ch);
}

//   Rendering
//
//   Reuses the shell's three sprites rather than allocating a framebuffer:
//     statusBarSprite  -> title bar (path, modified flag, cursor position)
//     terminalSprite   -> the text viewport
//     commandBarSprite -> hint/status bar
//   drawTerminalHistory()/statusManagement() repaint all three from scratch on the way
//   out, so borrowing them costs nothing.
//
//   Called from one place, after all pending input for this pass has been applied --
//   never from inside a key handler.

//the visible slice of one buffer line, already tab-expanded and horizontally scrolled
static String editVisibleRow(int line) {
    String expanded;
    editExpandLine(line, expanded, -1);
    if (editLeftCol >= (int)expanded.length()) {
        return "";
    }
    String slice = expanded.substring(editLeftCol);
    if ((int)slice.length() > editCols) {
        slice = slice.substring(0, editCols);
    }
    return slice;
}

static String editTitleText() {
    String t = editPathLogical;
    if (editModified) t += " *";
    return t;
}

static String editPositionText() {
    return String(editCursorLine() + 1) + "," + String(editCursorDisplayCol() + 1);
}

static String editHintText() {
    if (editStatus.length() > 0) {
        return editStatus;
    }
    return "^G help ^O save ^X exit ^K cut ^W find";
}

static void editRenderTitle() {
    statusBarSprite.fillSprite(PINK);
    statusBarSprite.setTextDatum(top_left);
    statusBarSprite.setTextColor(BLACK, PINK);
    statusBarSprite.drawString(editTitleText(), 5, 0);
    statusBarSprite.setTextDatum(top_right);
    statusBarSprite.drawString(editPositionText(), statusBarSprite.width() - 5, 0);
    statusBarSprite.setTextDatum(top_left);
    statusBarSprite.pushSprite(0, 0);
}

static void editRenderText() {
    terminalSprite.fillSprite(BLACK);
    terminalSprite.setTextDatum(top_left);
    terminalSprite.setTextColor(WHITE, BLACK);

    for (int r = 0; r < editRows; r++) {
        const int line = editTopLine + r;
        if (line >= editLineCount) break;
        const String row = editVisibleRow(line);
        if (row.length() > 0) {
            terminalSprite.drawString(row, TERMINAL_PADDING, 2 + r * editLineH);
        }
    }

    //block cursor, blinking
    if (((millis() / EDIT_CURSOR_BLINK_MS) & 1UL) == 0) {
        const int cLine = editCursorLine();
        const int cCol  = editCursorDisplayCol();
        const int r = cLine - editTopLine;
        const int c = cCol - editLeftCol;
        if (r >= 0 && r < editRows && c >= 0 && c < editCols) {
            const int x = TERMINAL_PADDING + c * editCharW;
            const int y = 2 + r * editLineH;
            terminalSprite.fillRect(x, y, editCharW, terminalSprite.fontHeight(), WHITE);
            const String row = editVisibleRow(cLine);
            if (c < (int)row.length()) {
                terminalSprite.setTextColor(BLACK, WHITE);
                terminalSprite.drawString(String(row[c]), x, y);
                terminalSprite.setTextColor(WHITE, BLACK);
            }
        }
    }

    terminalSprite.pushSprite(0, terminalAreaY());
}

static void editRenderHint() {
    commandBarSprite.fillSprite(BLACK);
    commandBarSprite.drawFastHLine(0, 0, commandBarSprite.width(), PINK);
    commandBarSprite.setTextDatum(top_left);
    commandBarSprite.setTextColor(editStatus.length() ? YELLOW : CYAN, BLACK);
    commandBarSprite.drawString(editHintText(), COMMAND_BAR_PADDING, COMMAND_BAR_PADDING);
    commandBarSprite.setTextColor(WHITE, BLACK);
    commandBarSprite.pushSprite(0, commandBarY());
}

static void editRender() {
    editScrollToCursor();
    editRenderTitle();
    editRenderText();
    editRenderHint();
}

//   Modal prompts (filename on ^O, save-on-exit confirmation). Both borrow the hint
//   bar and the same key source, and both keep rendering so the screen stays live.

//returns false if the user cancelled (^C or ^X)
static bool editPromptLine(const String& label, String& value) {
    while (true) {
        editStatus = label + value + "_";
        editRender();

        EditKey key;
        char ch;
        bool got = false;
        //spin until a key arrives, yielding so the idle task feeds the watchdog
        while (!got) {
            got = editNextKey(key, ch);
            if (!got) delay(2);
        }

        if (key == EK_ENTER)                       { editStatus = ""; return true; }
        if (key == EK_CANCEL || key == EK_EXIT)    { editStatus = ""; return false; }
        if (key == EK_BACKSPACE) {
            if (value.length() > 0) value.remove(value.length() - 1);
        } else if (key == EK_CHAR) {
            if ((int)value.length() < EDIT_PROMPT_MAX) value += ch;
        }
    }
}

//returns 'y', 'n', or 'c' (cancel)
static char editPromptYesNo(const String& label) {
    editStatus = label;
    editRender();
    while (true) {
        EditKey key;
        char ch;
        if (!editNextKey(key, ch)) {
            delay(2);
            continue;
        }
        if (key == EK_CANCEL || key == EK_EXIT) { editStatus = ""; return 'c'; }
        if (key == EK_CHAR) {
            if (ch == 'y' || ch == 'Y') { editStatus = ""; return 'y'; }
            if (ch == 'n' || ch == 'N') { editStatus = ""; return 'n'; }
        }
    }
}

//^W: prompts (prefilled with the last term, so Enter alone is "find next"), searches
//forward from just past the cursor, and wraps to the top once before giving up
static void editDoSearch() {
    String term = editLastSearch;
    if (!editPromptLine("Find: ", term) || term.length() == 0) {
        editStatus = "Cancelled";
        return;
    }
    editLastSearch = term;

    bool wrapped = false;
    long hit = editFindFrom(term.c_str(), term.length(), editCursor + 1);
    if (hit < 0) {
        hit = editFindFrom(term.c_str(), term.length(), 0);
        wrapped = true;
    }
    if (hit < 0) {
        editStatus = "\"" + term + "\" not found";
        return;
    }
    editCursor = (size_t)hit;
    editGoalCol = -1;
    editStatus = wrapped ? "Search wrapped to top" : "";
}

//^L: jump to a line number, clamped to the buffer
static void editDoGotoLine() {
    String s = "";
    if (!editPromptLine("Line: ", s)) {
        editStatus = "Cancelled";
        return;
    }
    s.trim();
    if (s.length() == 0) {
        editStatus = "Cancelled";
        return;
    }
    long n = s.toInt();
    if (n < 1) n = 1;
    if (n > editLineCount) n = editLineCount;
    editCursor = (size_t)editLineStart[n - 1];
    editGoalCol = -1;
    editStatus = "";
}

//   ^G help. The hint bar only has room for five chords, so this is where the rest of
//   the keymap actually lives. Modal: renders over the screen and waits.
static const char* EDIT_HELP_LINES[] = {
    "^O save    ^X exit",
    "^K cut line  ^U paste it back",
    "  consecutive ^K appends",
    "^W find (wraps)  ^L go to line",
    "^Z undo    ^G this screen",
    "^A line start  ^E line end",
    "^Y page up   ^V page down",
    "^D delete forward",
    "^C show position",
    "Fn+; Fn+. Fn+, Fn+/ = arrows",
    "Tabs kept as tabs. Saves via a",
    "temp file, so no truncation.",
};
static const int EDIT_HELP_COUNT = sizeof(EDIT_HELP_LINES) / sizeof(EDIT_HELP_LINES[0]);

static void editShowHelp() {
    statusBarSprite.fillSprite(PINK);
    statusBarSprite.setTextDatum(top_left);
    statusBarSprite.setTextColor(BLACK, PINK);
    statusBarSprite.drawString("edit -- keys", 5, 0);
    statusBarSprite.pushSprite(0, 0);

    //the help is taller than the viewport, so it pages: one screenful per keypress
    int shown = 0;
    while (shown < EDIT_HELP_COUNT) {
        terminalSprite.fillSprite(BLACK);
        terminalSprite.setTextDatum(top_left);
        terminalSprite.setTextColor(WHITE, BLACK);
        int drawn = 0;
        while (drawn < editRows && shown + drawn < EDIT_HELP_COUNT) {
            terminalSprite.drawString(EDIT_HELP_LINES[shown + drawn],
                                      TERMINAL_PADDING, 2 + drawn * editLineH);
            drawn++;
        }
        terminalSprite.pushSprite(0, terminalAreaY());
        shown += drawn;

        commandBarSprite.fillSprite(BLACK);
        commandBarSprite.drawFastHLine(0, 0, commandBarSprite.width(), PINK);
        commandBarSprite.setTextDatum(top_left);
        commandBarSprite.setTextColor(CYAN, BLACK);
        commandBarSprite.drawString(shown < EDIT_HELP_COUNT ? "any key: more" : "any key: back",
                                    COMMAND_BAR_PADDING, COMMAND_BAR_PADDING);
        commandBarSprite.setTextColor(WHITE, BLACK);
        commandBarSprite.pushSprite(0, commandBarY());

        EditKey key;
        char ch;
        while (!editNextKey(key, ch)) {
            delay(2);
        }
    }
    editStatus = "";
}

//   Load / save

//reads `logicalPath` into the buffer. A missing file is not an error -- it's a new
//file, same as nano. Returns false only on a real failure.
static bool editLoadFile(const String& logicalPath, String& err) {
    editLen = 0;
    editCursor = 0;

    const String resolved = resolvePath(cwd, logicalPath);
    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        err = "SD not mounted (insert card and reboot)";
        return false;
    }

    File f = r.fs->open(r.realPath, "r");
    if (!f) {
        editReindex();
        return true;   //new file
    }
    if (f.isDirectory()) {
        f.close();
        err = resolved + " is a directory";
        return false;
    }
    //refuse rather than truncate: silently loading half a file and then saving it back
    //would destroy the other half
    if (f.size() > EDIT_BUF_CAP) {
        const size_t sz = f.size();
        f.close();
        err = "file is " + String((unsigned long)sz) + "B, limit is "
            + String((unsigned long)EDIT_BUF_CAP) + "B";
        return false;
    }

    editLen = f.read((uint8_t*)editBuf, f.size());
    f.close();

    //CRLF files would otherwise render a trailing '?' on every line (the CR is not
    //printable), and the stray bytes would survive a save. Normalize on the way in;
    //the file is written back as LF.
    size_t w = 0;
    for (size_t i = 0; i < editLen; i++) {
        if (editBuf[i] == '\r' && i + 1 < editLen && editBuf[i + 1] == '\n') continue;
        editBuf[w++] = editBuf[i];
    }
    editLen = w;

    editReindex();
    if (editLineCount >= EDIT_MAX_LINES) {
        err = "file has more than " + String(EDIT_MAX_LINES) + " lines";
        return false;
    }
    return true;
}

//writes the buffer to `logicalPath` via a temp file + rename. Never truncates the
//target in place: this is a battery device, and a power loss partway through a
//truncating write destroys the file being edited rather than just failing.
static bool editSaveFile(const String& logicalPath, String& err) {
    const String resolved = resolvePath(cwd, logicalPath);
    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        err = "SD not mounted";
        return false;
    }

    const String tmpPath = r.realPath + ".tmp";
    File f = r.fs->open(tmpPath, "w");
    if (!f) {
        err = "cannot open " + resolved + " for writing";
        return false;
    }

    size_t written = 0;
    while (written < editLen) {
        //chunked so a large buffer doesn't sit in one blocking write
        const size_t chunk = min((size_t)1024, editLen - written);
        const size_t n = f.write((const uint8_t*)editBuf + written, chunk);
        if (n != chunk) {
            f.close();
            r.fs->remove(tmpPath);
            err = "short write (out of space?)";
            return false;
        }
        written += n;
    }
    f.close();

    r.fs->remove(r.realPath);   //no-op if it didn't exist
    if (!r.fs->rename(tmpPath, r.realPath)) {
        r.fs->remove(tmpPath);
        err = "rename failed";
        return false;
    }
    return true;
}

//^O: confirm the filename (prefilled, Enter accepts) then write. Updates the title bar
//to whatever was actually written, like nano's "save as".
static void editDoSave() {
    String name = editPathLogical;
    if (!editPromptLine("Write to: ", name)) {
        editStatus = "Cancelled";
        return;
    }
    if (name.length() == 0) {
        editStatus = "Cancelled (no name)";
        return;
    }

    String err;
    if (!editSaveFile(name, err)) {
        editStatus = "Error: " + err;
        return;
    }
    editPathLogical = name;
    //the buffer now matches the file, so this is the depth undoing back to
    //"unmodified" has to reach (see editRefreshModified)
    editSavedUndoDepth = editUndoCount;
    editRefreshModified();
    editStatus = "Wrote " + String((unsigned long)editLen) + " bytes";
}

//^X: returns true if the editor should close
static bool editDoExit() {
    if (!editModified) {
        return true;
    }
    const char answer = editPromptYesNo("Save changes? Y/N  (^C cancels)");
    if (answer == 'c') {
        return false;
    }
    if (answer == 'n') {
        return true;
    }
    editDoSave();
    return !editModified;   //a failed save keeps the editor open with the message up
}

//   Allocation

static bool editAlloc() {
    editBuf = (char*)heap_caps_calloc(EDIT_BUF_CAP, 1, MALLOC_CAP_8BIT);
    editLineStart = (int32_t*)heap_caps_calloc(EDIT_MAX_LINES, sizeof(int32_t), MALLOC_CAP_8BIT);
    if (!editBuf || !editLineStart) {
        if (editBuf) { heap_caps_free(editBuf); editBuf = nullptr; }
        if (editLineStart) { heap_caps_free(editLineStart); editLineStart = nullptr; }
        return false;
    }
    return true;
}

//   Everything is released on exit rather than held between launches. DS can afford to
//   keep an 128KB slab parked in PSRAM; 18KB of internal RAM here is worth handing back
//   to the shell, where ssh and FTP both want it.
static void editFree() {
    editUndoClear();   //each delete record owns its own payload
    if (editBuf) { heap_caps_free(editBuf); editBuf = nullptr; }
    if (editLineStart) { heap_caps_free(editLineStart); editLineStart = nullptr; }
}

//   Command entry point

void handleEditCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: edit <file>");
        outLine("  Full-screen text editor. Path is a normal shell", C_CYAN);
        outLine("  path (e.g. /sd/notes.txt). A missing file is", C_CYAN);
        outLine("  created on save.", C_CYAN);
        outLine("  ^O save, ^X exit, ^G help lists the rest.", C_CYAN);
        outLine("  Arrows are Fn+; Fn+. Fn+, Fn+/", C_CYAN);
        outLine("  Limits: 16KB, 512 lines, 16 undo steps.", C_CYAN);
        return;
    }

    if (!editAlloc()) {
        outLine("edit: out of memory", C_RED);
        editFree();
        return;
    }

    //geometry from the sprites the editor is about to borrow
    editCharW = max(1, (int)terminalSprite.textWidth("M"));
    editLineH = max((int)terminalSprite.fontHeight() + 2, 8);
    editCols = max(8, ((int)terminalSprite.width() - 2 * TERMINAL_PADDING) / editCharW);
    editRows = max(1, ((int)terminalSprite.height() - 4) / editLineH);

    editPathLogical = parts[1];
    editTopLine = 0;
    editLeftCol = 0;
    editGoalCol = -1;
    editStatus = "";
    editUndoClear();
    //editCutBuffer and editLastSearch deliberately survive across launches -- this
    //board has no clipboard, so cutting in one file and pasting into another is the
    //only way to move text between them. Only the "appending" flag resets, so the
    //first ^K of a new session starts a fresh cut rather than extending the old one.
    editLastKeyWasCut = false;

    String err;
    if (!editLoadFile(editPathLogical, err)) {
        outLine("edit: " + err, C_RED);
        editFree();
        return;
    }
    //loading writes editBuf directly rather than going through editInsertBytes, so
    //nothing was recorded and an empty undo stack is exactly the on-disk state
    editSavedUndoDepth = 0;
    editRefreshModified();

    bool blinkPhase = false;
    editNeedsRender = true;

    for (;;) {
        //drain every pending key, then render once. A sprite push is not free and the
        //keyboard can deliver a burst; rendering inside the key handler would make
        //fast typing lag behind.
        EditKey key;
        char ch;
        bool quit = false;

        while (editNextKey(key, ch)) {
            editNeedsRender = true;
            editStatus = "";   //any keypress clears a stale message

            //a run of typing or backspacing is only allowed to keep coalescing into one
            //undo step while nothing else intervenes, and only consecutive ^K presses
            //append to the cut buffer rather than replacing it
            if (key != EK_CHAR && key != EK_BACKSPACE) editUndoRun = 0;
            if (key != EK_CUT) editLastKeyWasCut = false;

            switch (key) {
                case EK_CHAR:      editInsertBytes(&ch, 1, 1); editGoalCol = -1; break;
                case EK_ENTER:     editInsertBytes("\n", 1, 0); editGoalCol = -1; break;
                case EK_TAB:       editInsertBytes("\t", 1, 0); editGoalCol = -1; break;
                case EK_BACKSPACE:
                    if (editCursor > 0) editDeleteBytes(editCursor - 1, 1, 2);
                    editGoalCol = -1;
                    break;
                case EK_DELETE:    editDeleteBytes(editCursor, 1, 0); editGoalCol = -1; break;
                case EK_LEFT:      editMoveLeft(); break;
                case EK_RIGHT:     editMoveRight(); break;
                case EK_UP:        editMoveVertical(-1); break;
                case EK_DOWN:      editMoveVertical(1); break;
                case EK_PGUP:      editMoveVertical(-editRows); break;
                case EK_PGDN:      editMoveVertical(editRows); break;
                case EK_HOME:      editMoveHome(); break;
                case EK_END:       editMoveEnd(); break;
                case EK_CUT:       editDoCut(); break;
                case EK_UNCUT:     editDoUncut(); break;
                case EK_SEARCH:    editDoSearch(); break;
                case EK_GOTO:      editDoGotoLine(); break;
                case EK_UNDO:      editApplyUndo(); break;
                case EK_HELP:      editShowHelp(); break;
                case EK_SAVE:      editDoSave(); break;
                case EK_EXIT:      if (editDoExit()) quit = true; break;
                case EK_CANCEL:    editStatus = editPositionText(); break;
                default: break;
            }
            if (quit) break;
        }
        if (quit) break;

        //blink is the one reason to repaint an otherwise-unchanged frame
        const bool phase = ((millis() / EDIT_CURSOR_BLINK_MS) & 1UL) == 0;
        if (phase != blinkPhase) {
            blinkPhase = phase;
            editNeedsRender = true;
        }

        if (editNeedsRender) {
            editRender();
            editNeedsRender = false;
        }

        delay(2);   //yield so CPU idle runs and feeds the task watchdog
    }

    editFree();

    //hand the screen back to the shell. refreshCounter is forced past its threshold so
    //statusManagement() repaints the real status bar on the very next tick instead of
    //leaving the editor's title bar up for up to 60 more.
    outLine("edit: closed " + editPathLogical, C_GREEN);
    refreshCounter = 61;
}

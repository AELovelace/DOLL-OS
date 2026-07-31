//   global.h
//   shared state and sprites used across DollOS

#pragma once

#include <ArduinoJson.h>

// M5Cardputer contains the StampS3's single addressable RGB LED. All LED helpers
// remain safe no-ops if a future compatible board reports no LED instances.
struct LedRgb {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

bool rearLedAvailable();
void rearLedSetRgb(uint8_t red, uint8_t green, uint8_t blue);
void rearLedSetRgbLong(long red, long green, long blue);
void rearLedOff();
void ledBegin();
void ledService();
void ledPulseStorageRead(bool isSd);
void ledPulseStorageWrite(bool isSd);
void ledPulseNetwork();
void ledPulseInput();
void ledPulseError();
void ledSetAppOverrideRgb(uint8_t red, uint8_t green, uint8_t blue);
void ledSetAppOverrideRgbLong(long red, long green, long blue);
void ledClearAppOverride();

//battery logic
int batteryPercent = 0;         //battery in percent
int batteryMillivolts = 0;      //battery in millivolts
int refreshCounter = 0;         //tick counter, statusManagement refreshes once this hits 60

//command logic
String currentCommand  = "";             //command buffer, filled by readKeyboard and cleared by commandProcessor
int commandCursorPos = 0;                //index within currentCommand where typing/deleting/cursor keys act
int commandScrollOffset = 0;             //first visible character index in the command bar when text overflows the screen
const int COMMAND_BAR_HEIGHT = 24;       //pixel height of the command bar at the bottom of the screen
const int COMMAND_BAR_PADDING = 4;       //pixel padding inside the command bar
LGFX_Sprite commandBarSprite(&M5Cardputer.Display);   //offscreen buffer the command bar gets drawn to before pushing

//command history (sent commands, recalled with fn+;/fn+. like a shell's up/down arrows)
const int COMMAND_HISTORY_MAX = 30;          //max previously sent commands remembered
String commandHistory[COMMAND_HISTORY_MAX];  //oldest at index 0, newest at commandHistoryCount - 1
int commandHistoryCount = 0;                 //number of valid entries in commandHistory
int commandHistoryHead = 0;                  //physical slot of the oldest command in the ring buffer
int commandHistoryIndex = -1;                //entry currently recalled into the command bar; -1 means not recalling
String commandHistoryDraft = "";             //in-progress typing stashed when recall starts, restored when recall runs past the newest entry

//storage
bool sdCardMounted = false;    //true once SD.begin() succeeds in initStorage()
String cwd = "/";              //current working directory in the unified namespace; SD_MOUNT and below route to the SD card
const String SD_MOUNT = "/sd"; //mount point where the SD card appears in the unified namespace ("sd" is reserved at flash root)

//result of routing an absolute unified-namespace path onto the physical filesystem that owns it.
//declared here (not in storage.ino) because the Arduino IDE hoists auto-generated function
//prototypes above every .ino file's own code, so a struct return type must already be visible
struct RoutedPath {
    fs::FS* fs;
    String realPath;
    bool isSd;
};

//File-backed shell aliases. Kept here because Arduino-generated prototypes for
//Alias.ino mention this type before that tab is concatenated.
struct AliasEntry {
    String name;
    String expansion;
};

//Dapper package/catalog records, also needed before generated prototypes.
struct DapperRecord {
    int packageFormat = 0;
    String id;
    String name;
    String summary;
    String version;
    String runtimeMin;
    String runtimeMaxExclusive;
    String sha256;
    String url;
    size_t size = 0;
    bool compatible = false;
    String incompatibility;
};

struct DapperInstalled {
    DapperRecord record;
    String repository;
    String installedPath;
};

//   Output color codes (Output.ino)
//   These are ANSI SGR foreground codes, not pixel colors -- DOLL-OS renders to a
//   sprite and has no terminal to send escape sequences to, so ansiCodeToPixelColor()
//   maps them onto M5GFX uint16_t colors at the point of drawing. They exist in this
//   indirect form so files ported from DS drop in unchanged: every DS call site says
//   outLine(text, C_CYAN), and rewriting each one to CYAN by hand would be churn that
//   makes the two trees stop diffing against each other. See docs/PORT-FROM-DS.md.
const int C_RESET   = 0;
const int C_WHITE   = 37;
const int C_BLACK   = 30;
const int C_RED     = 31;
const int C_GREEN   = 32;
const int C_YELLOW  = 33;
const int C_BLUE    = 34;
const int C_MAGENTA = 35;
const int C_CYAN    = 36;
const int C_PINK    = 95;   //bright magenta; stands in for the PINK accent color

//   Editor (Edit.ino) -- the "edit" app's logical key vocabulary. Used only inside
//   Edit.ino, but it still has to live here: the Arduino builder generates hoisted
//   prototypes for that file's `static` functions too, and a prototype mentioning
//   EditKey lands above Edit.ino's own definitions. Same trap, and the same fix, as
//   RoutedPath and the Dapp* structs.
//
//   DS also declares an EditKeyState here for its ESC/CSI byte decoder. There is no
//   such decoder in this port -- keystrokes arrive as a Keyboard_Class::KeysState from
//   the physical keyboard, already decoded -- so that type has no counterpart.
enum EditKey {
    EK_NONE, EK_CHAR, EK_ENTER, EK_TAB, EK_BACKSPACE, EK_DELETE,
    EK_LEFT, EK_RIGHT, EK_UP, EK_DOWN,
    EK_HOME, EK_END, EK_PGUP, EK_PGDN,
    EK_SAVE, EK_EXIT, EK_CANCEL,
    EK_CUT, EK_UNCUT, EK_SEARCH, EK_GOTO, EK_UNDO, EK_HELP
};

//   .dapp script runtime (AppRunner.ino). These live here rather than in AppRunner.ino
//   for the same reason RoutedPath does: the Arduino builder hoists auto-generated
//   prototypes for a tab's static functions above that tab's own type definitions, so a
//   prototype mentioning DappLine lands before the struct exists.
struct DappLine {
    String text;
};

struct DappLabel {
    String name;
    int lineIndex;
};

struct DappVar {
    String name;
    long value;
    bool used;
};

struct DappStringVar {
    String name;
    String value;
    bool used;
};

//DIM arrays share one fixed pool owned by DappProgram. On Cardputer all blocks
//come from internal RAM, so AppRunner keeps smaller caps than the PSRAM DS build.
struct DappArray {
    String name;
    long* values;
    int size;
    bool used;
};

struct DappProgram {
    DappLine* lines = nullptr;
    DappLabel* labels = nullptr;
    DappVar* vars = nullptr;
    DappStringVar* stringVars = nullptr;
    DappArray* arrays = nullptr;
    long* arrayPool = nullptr;
    int* callStack = nullptr;
    int lineCount = 0;
    int labelCount = 0;
    int arrayPoolUsed = 0;
    int callDepth = 0;
    String fault = "";

    bool alloc();
    ~DappProgram();

    DappProgram() {}
    DappProgram(const DappProgram&) = delete;
    DappProgram& operator=(const DappProgram&) = delete;
};

enum DappKeyPhase { DKEY_NORMAL, DKEY_ESC, DKEY_CSI };
struct DappKeyState {
    DappKeyPhase phase = DKEY_NORMAL;
    String params = "";
    unsigned long escAtMs = 0;
};

struct DappCanvasCell {
    char ch;
    uint8_t color;
};
DappCanvasCell* dappCanvasCells = nullptr;
int dappCanvasCols = 0;
int dappCanvasRows = 0;
bool dappCanvasActive = false;

#define DOLL_BOARD_ID "m5cardputer"
#define DAPP_RUNTIME_VERSION "1.3.0"
#define DAPP_PACKAGE_FORMAT 1

int splitCommand(const String& input, String parts[], int maxParts);
bool keyboardEventIsDebounced(const Keyboard_Class::KeysState& keys);
void ensureDefaultAliases();
bool expandCommandAlias(String& command, String& aliasName, String& aliasExpansion);
void handleAliasCommand(const String parts[], int partCount);
void handleAppsCommand(const String parts[], int partCount);
void handleDapperCommand(const String parts[], int partCount);
void handleRunCommand(const String parts[], int partCount);
void handleUnaliasCommand(const String parts[], int partCount);
void seedBundledApps();

//status bar
const int STATUS_BAR_HEIGHT = 14;   //pixel height of the top status bar
LGFX_Sprite statusBarSprite(&M5Cardputer.Display);   //offscreen buffer the status bar gets drawn to before pushing

//heap instrumentation
const int HEAP_CHECKPOINT_MAX = 16;
struct HeapCheckpoint {
    const char* tag;
    uint32_t freeHeap;
    uint32_t largestBlock;
    uint32_t minFreeHeap;
};
HeapCheckpoint heapCheckpoints[HEAP_CHECKPOINT_MAX];
int heapCheckpointCount = 0;
int heapCheckpointHead = 0;

//terminal
const int TERMINAL_PADDING = 4;             //pixel padding around the terminal history text
const int HISTORY_MAX_LINES = 120;          //max rows kept in historyLines before old rows get shifted out
const int HISTORY_ROW_MAX_CHARS = 96;       //max characters kept per wrapped terminal row, including the trailing NUL
LGFX_Sprite terminalSprite(&M5Cardputer.Display);   //offscreen buffer the terminal history gets drawn to before pushing
struct HistoryRow {
    char text[HISTORY_ROW_MAX_CHARS];
    uint16_t color = WHITE;
};
HistoryRow historyRows[HISTORY_MAX_LINES];  //ring buffer of wrapped terminal history rows with inline storage
int historyCount = 0;                       //number of valid rows currently in historyLines
int historyHead = 0;                        //physical slot of the oldest logical history row in the ring buffer
int scrollOffset = 0;                       //how many rows back from the newest line the view is scrolled

//ANSI/UTF-8 filtering for remote text streams (ssh, telnet). Declared here (not in
//ansi.ino) for the same reason RoutedPath is declared here: the Arduino IDE hoists
//auto-generated function prototypes above every .ino file's own code, so a type used
//as a function parameter must already be visible.
enum AnsiParseState {
    ANSI_TEXT,      //ordinary text/UTF-8 bytes
    ANSI_ESC,       //saw ESC (0x1B), waiting to see what kind of sequence follows
    ANSI_CSI,       //ESC [ ... -- consuming parameter/intermediate bytes until a final byte
    ANSI_OSC,       //ESC ] ... -- consuming until BEL or ST (ESC \)
    ANSI_OSC_ESC    //inside OSC, saw ESC, waiting to see if '\' closes it (ST)
};

struct AnsiFilterState {
    AnsiParseState state = ANSI_TEXT;
    String csiParams = "";     //accumulated CSI parameter bytes, e.g. "1;31"
    int utf8Remaining = 0;     //continuation bytes still expected for the current UTF-8 sequence
};

//per-caller state for the shared character-streaming terminal API (terminalStreamPutChar /
//terminalStreamNewline / terminalStreamReset in terminal.ino). One instance per independent
//stream of incoming text (ssh's stdout vs stderr, telnet's socket, future callers) so that
//streams which interleave arbitrarily each track their own in-progress row without corrupting
//each other's content. Declared here (not in terminal.ino) for the same hoisted-prototype reason
//as RoutedPath/AnsiFilterState.
struct TerminalStreamState {
    String pendingRow = "";     //characters accumulated so far for this stream's current, not-yet-closed row
    size_t cursorCol = 0;       //index within pendingRow the next character is written at; normally == pendingRow.length()
                                 //(appending), but a bare '\r' (carriage return without linefeed) rewinds it to 0 so the
                                 //next characters overwrite in place instead of appending -- the "\r" + erase-line + text
                                 //idiom remote chat/line-editor software (e.g. telehack's relay) uses to redraw a line
};

extern String sshInputBuffer;
extern String motokoChannel;
extern String motokoInputBuffer;
extern AnsiFilterState telnetAnsi;
extern AnsiFilterState sshStdoutAnsi;
extern AnsiFilterState sshStderrAnsi;
extern TerminalStreamState telnetStream;
extern TerminalStreamState sshStdoutStream;
extern TerminalStreamState sshStderrStream;

//which TerminalStreamState currently "owns" the last row in historyRows, i.e. may
//keep extending it via updateLastHistoryRow. nullptr = no stream owns an open row right now.
//Uses each caller's own state-struct address as a lightweight token -- no ID registry needed,
//works for any future caller automatically.
TerminalStreamState* terminalOpenRowOwner = nullptr;

//shared modal loop for character-oriented remote sessions (ssh shell, telnet in character mode).
//Replaces the old per-feature pattern of buffering a full line locally and only sending it once
//Enter is pressed -- a real remote pty/telnet stream needs every keystroke immediately (arrow
//keys, ctrl+c, backspace-before-enter, and full-screen/interactive programs all depend on it).
//Subclasses (TelnetSession in telnet.ino, SshShellSession in ssh.ino) only need to supply the
//transport; this class owns keystroke capture, the local escape chord (Fn+Q), and the
//poll/pump/redraw loop shape they'd otherwise each reimplement. Implemented in RemoteSession.ino.
//
//Declared here, not alongside its subclasses, for the same hoisting reason as AnsiFilterState/
//TerminalStreamState above: it must already be visible to every .ino file before any subclass
//(each defined further down the sketch, after that file's own protocol-library #include) uses it.
class RemoteSession {
public:
    virtual ~RemoteSession() {}

    //runs until the remote closes or the user hits Fn+Q. Callers still print their own
    //"session ended" line afterward -- this only owns the live back-and-forth.
    void run();

protected:
    virtual void pumpIncoming() = 0;                   //drain whatever's arrived since last poll into terminal history
    virtual bool isClosed() = 0;                       //has the remote end gone away
    virtual void sendBytes(const String& bytes) = 0;   //forward raw keystroke bytes to the remote
    virtual void drawInputRow() = 0;                   //draw this session's prompt/status row
    virtual void onClosed() {}                         //called once, the first time isClosed() is observed true

    //byte(s) sent to the remote when the user presses backspace/delete. Differs by transport:
    //a real unix pty (ssh) expects DEL (0x7F) as its erase character, but classic telnet/BBS
    //servers (e.g. telehack.com) implement their own line editor against the original ASCII
    //backspace (0x08) and may not recognize DEL as an erase request at all. Override per subclass.
    virtual String backspaceBytes() { return "\x7f"; }

private:
    //Fn+K handler (RemoteSession.ino) -- runs one shell command via commandProcessor()
    //without ending the session. Not virtual: identical for every subclass, unlike
    //pumpIncoming/isClosed/sendBytes which are transport-specific.
    void runInlineCommandPrompt();
};

//dice

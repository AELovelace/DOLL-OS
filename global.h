//   global.h
//   shared state and sprites used across DollOS

#pragma once

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

//command history (sent commands, recalled with ctrl+;/ctrl+. like a shell's up/down arrows)
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
};

//dice

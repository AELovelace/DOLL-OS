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

//terminal
const int TERMINAL_PADDING = 4;             //pixel padding around the terminal history text
const int HISTORY_MAX_LINES = 120;          //max rows kept in historyLines before old rows get shifted out
LGFX_Sprite terminalSprite(&M5Cardputer.Display);   //offscreen buffer the terminal history gets drawn to before pushing
String historyLines[HISTORY_MAX_LINES];     //ring-ish buffer of wrapped terminal history rows
uint16_t historyColors[HISTORY_MAX_LINES];  //text color for each row in historyLines, parallel array
int historyCount = 0;                       //number of valid rows currently in historyLines
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

//dice

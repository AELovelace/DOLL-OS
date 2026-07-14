//   global.h
//   shared state and sprites used across DollOS

#pragma once

//battery logic
int batteryPercent = 0;         //battery in percent
int batteryMillivolts = 0;      //battery in millivolts
int refreshCounter = 0;         //tick counter, statusManagement refreshes once this hits 60

//command logic
String currentCommand  = "";             //command buffer, filled by readKeyboard and cleared by commandProcessor
const int COMMAND_BAR_HEIGHT = 24;       //pixel height of the command bar at the bottom of the screen
const int COMMAND_BAR_PADDING = 4;       //pixel padding inside the command bar
LGFX_Sprite commandBarSprite(&M5Cardputer.Display);   //offscreen buffer the command bar gets drawn to before pushing

//storage
bool sdCardMounted = false;   //true once SD.begin() succeeds in initStorage()

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
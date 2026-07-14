//   terminal.ino
//   handles system-related features for DollOS

//status bar management
void statusManagement(){
    //check battery if it's been more than 60 ticks
    if(refreshCounter >= 60){
        batteryPercent = M5Cardputer.Power.getBatteryLevel();               //get batteryPercent  
        batteryMillivolts = M5Cardputer.Power.getBatteryVoltage();          //get battery voltage   
        String batteryText = "Battery:  " + String(batteryPercent) + "% "   //battery indicator string construction
                            + String(batteryMillivolts/1000.0f) + "V";    
        //draw Dollputer banner
        statusBarSprite.fillSprite(BLACK);                                  //fill sprite area with black
        statusBarSprite.setTextDatum(top_left);                             //text align top-left
        statusBarSprite.setTextColor(PINK,BLACK);                           //set text color
        statusBarSprite.drawString("DOLL-OS V0.1",10,0);                    //DOLL-OS header
        //draw battery percentage
        statusBarSprite.setTextDatum(top_right);
        statusBarSprite.drawString(batteryText, statusBarSprite.width() - 5, 0);
        //draw divider
        statusBarSprite.drawFastHLine(0, STATUS_BAR_HEIGHT - 2, statusBarSprite.width(), PINK);
        //push updated sprite
        statusBarSprite.pushSprite(0,0);
        //reset refresh counter
        refreshCounter = 0;
    }   else{
        refreshCounter++;
    }
}

//Terminal area code

//returns terminal area
int terminalAreaY(){
    return STATUS_BAR_HEIGHT;
}

//returns terminal like height
int terminalVisibleLines(){
    const int lineHeight = 12;                                                                  //line height of 12px
    const int availableHeight = max(0, (int)terminalSprite.height() - (TERMINAL_PADDING * 2));  //figure out the max available space by taking total pixel height of terminalSprite. TERMINAL_PADDING * 2 subtracts padding from both top and bottom and clamps result to minimum of 0
    return max(1, availableHeight / lineHeight);    //max 1 prevents 0 from being returned, takes available height of screen, divides by lineheight, and returns number of possible rows.                                           
}

void addHistoryRow(const String& row, uint16_t color) {

    if (historyCount < HISTORY_MAX_LINES) {             //if history count is less than max lines, add to history.
        historyLines[historyCount] = row;               //add to history array
        historyColors[historyCount] = color;            //remember its color alongside it
        historyCount++;
    } else {                                            //else, shift all lines up and add new line at the end
        for (int i = 1; i < HISTORY_MAX_LINES; i++) {   //for i is less than the max lines of history
            historyLines[i - 1] = historyLines[i];      //shift i back one place to make new history slot open.
            historyColors[i - 1] = historyColors[i];
        }
        historyLines[HISTORY_MAX_LINES - 1] = row;      //insert row at last index available.
        historyColors[HISTORY_MAX_LINES - 1] = color;
    }
    //snap back to newest output after adding row.
    scrollOffset = 0;
}

//plain overload: existing call sites that don't care about color keep working unchanged
void addWrappedHistoryLine(const String& line) {
    addWrappedHistoryLine(line, WHITE);
}

void addWrappedHistoryLine(const String& line, uint16_t color) {
    const int maxWidth = (int)terminalSprite.width() - (TERMINAL_PADDING * 2);  //keep test in terminal sprite padding
    if (maxWidth <0){
        return;
    }

    String row = "";    //build temporary string to store row in.

    for(int i = 0; i < line.length(); i++){
        char ch = line[i]; //get character at index i
        String candidate = row + ch;

        //if adding this character exceeds the row width
        if(row.length() > 0 && terminalSprite.textWidth(candidate) > maxWidth){
            addHistoryRow(row, color);
            row = "";

            //skip leading space on next row
            if (ch == ' '){
                continue;
            }
        }

        row += ch;
    }

    addHistoryRow(row, color); //add any remaining text as a new row
}

void scrollHistory(int delta) {
    const int maxScroll = max(0, historyCount - terminalVisibleLines());
    scrollOffset = constrain(scrollOffset + delta, 0, maxScroll);
}

void drawTerminalHistory() {
    terminalSprite.fillSprite(BLACK);
    terminalSprite.setTextDatum(top_left);

    if (historyCount == 0) {
        terminalSprite.pushSprite(0, terminalAreaY());
        return;
    }

    const int lineHeight = 12;
    const int visibleLines = terminalVisibleLines();
    const int maxScroll = max(0, historyCount - visibleLines);

    scrollOffset = constrain(scrollOffset, 0, maxScroll);

    int lastLine = historyCount - 1 - scrollOffset;
    int firstLine = max(0, lastLine - visibleLines + 1);

    int y = TERMINAL_PADDING;
    for (int i = firstLine; i <= lastLine; i++) {
        terminalSprite.setTextColor(historyColors[i], BLACK);
        terminalSprite.drawString(historyLines[i], TERMINAL_PADDING, y);
        y += lineHeight;
    }
    terminalSprite.setTextColor(WHITE, BLACK);   //reset so anything drawing without its own color call gets the default
    terminalSprite.pushSprite(0, terminalAreaY());

}

//USB Mass Storage warning banner

//covers the terminal area with a red warning while the SD card is exposed over USB
void drawUsbWarning() {
    const int top = terminalAreaY();
    const int height = M5Cardputer.Display.height() - top - COMMAND_BAR_HEIGHT;
    M5Cardputer.Display.fillRect(0, top, M5Cardputer.Display.width(), height, RED);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextColor(WHITE, RED);
    M5Cardputer.Display.drawString("USB MODE ACTIVE", M5Cardputer.Display.width() / 2, top + height / 2 - 8);
    M5Cardputer.Display.drawString("FN + ` to exit", M5Cardputer.Display.width() / 2, top + height / 2 + 8);
}
void drawBootLogo() {
    M5Cardputer.Display.setTextSize(2);
    const int top = terminalAreaY();
    const int height = M5Cardputer.Display.height() - top - COMMAND_BAR_HEIGHT;
    M5Cardputer.Display.fillRect(0, top, M5Cardputer.Display.width(), height, CYAN);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextColor(PINK, BLACK);
    M5Cardputer.Display.drawString("DOLL-OS", M5Cardputer.Display.width() / 2, top + height / 2 - 8);
    M5Cardputer.Display.drawString("V-0.1", M5Cardputer.Display.width() / 2, top + height / 2 + 8);
    M5Cardputer.Display.setTextSize(1);
}
//Command Bar Area
int commandBarY(){
    return M5Cardputer.Display.height() - COMMAND_BAR_HEIGHT;
}

//Draws command bar at bottom fo the screen
void drawCommandBar(const String& text) {
    //get location of y from commandBarY();
    commandBarSprite.fillSprite(BLACK);
    commandBarSprite.drawFastHLine(0,0,commandBarSprite.width(), WHITE);
    commandBarSprite.drawString(">", COMMAND_BAR_PADDING,COMMAND_BAR_PADDING);
    commandBarSprite.drawString(text,COMMAND_BAR_PADDING + 12, COMMAND_BAR_PADDING);
    commandBarSprite.pushSprite(0,commandBarY());
}


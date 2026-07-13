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

void addHistoryRow(const String& row) {
     
    if (historyCount < HISTORY_MAX_LINES) {             //if history count is less than max lines, add to history.
        historyLines[historyCount++] = row;             //add to history array and increment count
    } else {                                            //else, shift all lines up and add new line at the end
        for (int i = 1; i < HISTORY_MAX_LINES; i++) {   //for i is less than the max lines of history
            historyLines[i - 1] = historyLines[i];      //shift i back one place to make new history slot open. 
        }
        historyLines[HISTORY_MAX_LINES - 1] = row;      //insert row at last index available. 
    }
    //snap back to newest output after adding row. 
    scrollOffset = 0;
}

void addWrappedHistoryLine(const String& line) {
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
            addHistoryRow(row);
            row = "";

            //skip leading space on next row
            if (ch == ' '){
                continue;
            }
        }

        row += ch;
    }

    addHistoryRow(row); //add any remaining text as a new row
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
        terminalSprite.drawString(historyLines[i], TERMINAL_PADDING, y);
        y += lineHeight;        
    }
    terminalSprite.pushSprite(0, terminalAreaY());

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


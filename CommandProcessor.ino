//   CommandProcessor.ino
//   parses and runs terminal commands for DollOS
//   vibe coded struct system for commands, as i was out of ideas. 
//   command processing

//split commands for ingestion into command subsystem
int splitCommand(const String& input, String parts[], int maxParts) {
    String working = input; //make a working copy of input
    working.trim();         //remove trailing spaces

    int count = 0;  //count how many things we stored
    int start = 0;  //start at first char
    int len = working.length();
    //keep going until we run out of text or modifier slots
    while (start < len && count < maxParts){
        while (start < len && working[start] == ' '){
            start++;
        }
        // if we reached end and encounter no spaces, we are done
        if(start >= len) {
            break;
        }
        char c = working[start];
        //quoted token: everything up to the matching quote is one argument, quotes stripped
        if (c == '"' || c == '\'') {
            char quote = c;
            start++; //skip opening quote
            int end = working.indexOf(quote, start);
            if (end == -1) {
                //no closing quote, take the rest of the input as-is
                parts[count++] = working.substring(start);
                break;
            }
            parts[count++] = working.substring(start, end);
            start = end + 1; //skip closing quote
            continue;
        }
        // Find the next space after the current word. returns -1 if none found.
        int end = working.indexOf(' ', start); //location of space

        // If no more spaces were found, this is the last token.
        if (end == -1){
            parts[count++] = working.substring(start);  //record token using substring
            break;
        }
        // Otherwise store the current token.
        parts[count++] = working.substring(start, end); //record using substring to trim start and end
        start = end + 1;    //Move past the space and continue scanning.
    }
    return(count);
}

//remembers a sent command, evicting the oldest entry once full (mirrors addHistoryRow's shift logic)
static int commandHistoryPhysicalIndex(int logicalIndex) {
    return (commandHistoryHead + logicalIndex) % COMMAND_HISTORY_MAX;
}

void addCommandHistory(const String& cmd) {
    if (cmd.length() == 0) {
        return;
    }
    if (commandHistoryCount < COMMAND_HISTORY_MAX) {
        int slot = commandHistoryPhysicalIndex(commandHistoryCount);
        commandHistory[slot] = cmd;
        commandHistoryCount++;
    } else {
        commandHistory[commandHistoryHead] = cmd;
        commandHistoryHead = (commandHistoryHead + 1) % COMMAND_HISTORY_MAX;
    }
    commandHistoryIndex = -1;   //sending a command always ends any in-progress recall
}

//steps through previously sent commands into the live command buffer.
//step < 0 moves to older commands, step > 0 moves to newer commands and eventually back to the stashed draft.
void recallCommandHistory(int step, String& text) {
    if (commandHistoryCount == 0) {
        return;
    }

    if (commandHistoryIndex == -1) {
        if (step > 0) {   //already showing the live draft, nothing newer to recall
            return;
        }
        commandHistoryDraft = text;   //stash in-progress typing so it can be restored later
        commandHistoryIndex = commandHistoryCount - 1;
    } else {
        int newIndex = commandHistoryIndex + step;
        if (newIndex < 0) {
            newIndex = 0;
        } else if (newIndex >= commandHistoryCount) {
            commandHistoryIndex = -1;
            text = commandHistoryDraft;
            return;
        }
        commandHistoryIndex = newIndex;
    }

    text = commandHistory[commandHistoryPhysicalIndex(commandHistoryIndex)];
}

//dispatch table entry: command name -> handler
struct CommandEntry {
    const char* name;
    void (*handler)(const String parts[], int partCount);
};

void helpCommandHandler(const String parts[], int partCount) {
    outLine("Commands: alias, apps, battery, calc, cat, cd, clear, cp,");
    outLine("          dapper, del, dice, edit, free, ftp, help, ip,");
    outLine("          ls, mkdir, motoko, mv, ping, pwd, reboot, rm,");
    outLine("          run, ssh, status, telnet, unalias, uptime, usb, wifi");
}

//reboots the board. Nothing is flushed first because nothing here is buffered --
//LittleFS writes complete synchronously and the SD card is only ever written inside
//a command that has already returned by the time this runs.
void handleRebootCommand(const String parts[], int partCount) {
    outLine("Restarting...");
    drawTerminalHistory();   //ESP.restart() never returns to loop(), so paint the panel now --
                              //otherwise a reboot gives no on-screen feedback at all
    delay(500);
    ESP.restart();
}

void handleUptimeCommand(const String parts[], int partCount) {
    unsigned long totalSeconds = millis() / 1000;
    unsigned long days = totalSeconds / 86400;
    unsigned long hours = (totalSeconds % 86400) / 3600;
    unsigned long minutes = (totalSeconds % 3600) / 60;
    unsigned long seconds = totalSeconds % 60;

    char buf[64];
    snprintf(buf, sizeof(buf), "Uptime: %lu days, %02lu:%02lu:%02lu", days, hours, minutes, seconds);
    outLine(String(buf));
}

//one-screen summary of where the board stands on the network. Overlaps "wifi" deliberately:
//"wifi" is the subcommand surface for changing things, "status" is the read-only glance.
void handleStatusCommand(const String parts[], int partCount) {
    outLine("");
    outLine("Wi-Fi status", C_CYAN);
    outLine("-----------");
    if (wifiIsConnected() == 1) {
        outLine("Router: connected");
        outLine("Router SSID: " + WiFi.SSID());
        outLine("Station IP: " + WiFi.localIP().toString());
        outLine("Signal: " + String(WiFi.RSSI()) + " dBm");
    } else {
        outLine("Router: disconnected");
    }
    outLine("");
}

//sorted alphabetically for readability; lookup is a linear scan since the table is tiny
static const CommandEntry commandTable[] = {
    { "alias",  handleAliasCommand },
    { "apps",   handleAppsCommand },
    { "battery", handleBatteryCommand },
    { "calc",   handleCalcCommand },
    { "cat",    handleCatCommand },
    { "cd",     handleCdCommand },
    { "cp",     handleCpCommand },
    { "dapper", handleDapperCommand },
    { "del",    handleDelCommand },
    { "dice",   handleDiceCommand },
    { "edit",   handleEditCommand },
    { "free",   handleFreeCommand },
    { "ftp",    handleFtpCommand },
    { "help",   helpCommandHandler },
    { "ip",     handleIpCommand },
    { "ls",     handleLsCommand },
    { "mkdir",  handleMkdirCommand },
    { "motoko", handleMotokoCommand },
    { "mv",     handleMvCommand },
    { "ping",   handlePingCommand },
    { "pwd",    handlePwdCommand },
    { "reboot", handleRebootCommand },
    { "rm",     handleRmCommand },
    { "run",    handleRunCommand },
    { "ssh",    handleSshCommand },
    { "status", handleStatusCommand },
    { "telnet", handleTelnetCommand },
    { "unalias", handleUnaliasCommand },
    { "uptime", handleUptimeCommand },
    { "usb",    handleUsbCommand },
    { "wifi",   handleWifiCommand },
};
static const int commandTableSize = sizeof(commandTable) / sizeof(commandTable[0]);

//takes the finished command buffer, runs it, and clears command for the next entry
void commandProcessor(String& command) {
    if (command.length() == 0) {   //nothing typed, nothing to do
        return;
    }

    String entered = command;   //copy off the buffer before clearing it
    command = "";                //reset the shared buffer so the command bar goes blank
    commandCursorPos = 0;         //cursor and scroll reset alongside the now-empty buffer
    commandScrollOffset = 0;

    String trimmedEntered = entered;
    trimmedEntered.trim();
    addCommandHistory(trimmedEntered);   //remember this command for fn+;/fn+. recall

    String dispatchCommand = trimmedEntered;
    String aliasName;
    String aliasExpansion;
    expandCommandAlias(dispatchCommand, aliasName, aliasExpansion);

    String parts[8];
    int partCount = splitCommand(dispatchCommand, parts, 8);
    //if no parts, stop here.
    if (partCount == 0) {
        return;
    }
    if (parts[0] == "clear") {    //clear wipes history without echoing itself
        outClearScreen();
        return;
    }

    echoCommandLine(trimmedEntered);   //echo the command into the terminal history, under
                                        //the same path-aware prompt the command bar shows

    for (int i = 0; i < commandTableSize; i++) {
        if (parts[0] == commandTable[i].name) {
            commandTable[i].handler(parts, partCount);
            return;
        }
    }
    String unknown = dispatchCommand;
    if (aliasName.length() > 0) {
        unknown += " (from alias " + aliasName + ")";
    }
    outLine("Unknown command: " + unknown, C_RED);
}

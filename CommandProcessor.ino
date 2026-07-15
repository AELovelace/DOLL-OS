//   CommandProcessor.ino
//   parses and runs terminal commands for DollOS
// vibe coded struct system for commands, as i was out of ideas. 
//command processing

//split commands for ingestion into command subsystem
int splitCommand(const String& input, String parts[], int maxParts) {
    String working = input; //make a working copy of input
    working.trim();         //remove trailing spaces

    int count = 0;  //count how many things we stored
    int start = 0;  //start at first char
    //keep going until we run out of text or modifier slots
    while (start < working.length() && count < maxParts){
        while (start <working.length() && working[start] == ' '){
            start++;
        }   
    // if we reached end and encounter no spaces, we are done
    if(start >= working.length()) {
        break;
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
void addCommandHistory(const String& cmd) {
    if (cmd.length() == 0) {
        return;
    }
    if (commandHistoryCount < COMMAND_HISTORY_MAX) {
        commandHistory[commandHistoryCount++] = cmd;
    } else {
        for (int i = 1; i < COMMAND_HISTORY_MAX; i++) {
            commandHistory[i - 1] = commandHistory[i];
        }
        commandHistory[COMMAND_HISTORY_MAX - 1] = cmd;
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

    text = commandHistory[commandHistoryIndex];
}

//dispatch table entry: command name -> handler
struct CommandEntry {
    const char* name;
    void (*handler)(const String parts[], int partCount);
};

void helpCommandHandler(const String parts[], int partCount) {
    addWrappedHistoryLine("Commands: help, cd, clear, dice, wifi, ip, ls, pwd, ssh, telnet, usb, ping, motoko");
}

//sorted alphabetically for readability; lookup is a linear scan since the table is tiny
static const CommandEntry commandTable[] = {
    { "calc",   handleCalcCommand },
    { "cd",     handleCdCommand },
    { "dice",   handleDiceCommand },
    { "help",   helpCommandHandler },
    { "ip",     handleIpCommand },
    { "ls",     handleLsCommand },
    { "motoko", handleMotokoCommand },
    { "ping",   handlePingCommand },
    { "pwd",    handlePwdCommand },
    { "ssh",    handleSshCommand },
    { "telnet", handleTelnetCommand },
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
    addCommandHistory(trimmedEntered);   //remember this command for ctrl+;/ctrl+. recall

    String parts[8];
    int partCount = splitCommand(entered, parts, 8);
    //if no parts, stop here.
    if (partCount == 0) {
        return;
    }
    if (parts[0] == "clear") {    //clear wipes history without echoing itself
        historyCount = 0;
        scrollOffset = 0;
        terminalOpenRowOwner = nullptr;
        return;
    }

    addWrappedHistoryLine("> " + entered);   //echo the command into the terminal history

    for (int i = 0; i < commandTableSize; i++) {
        if (parts[0] == commandTable[i].name) {
            commandTable[i].handler(parts, partCount);
            return;
        }
    }
    addWrappedHistoryLine("Unknown command: " + entered);   //fallback for anything not recognized above
}
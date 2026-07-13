//   CommandProcessor.ino
//   parses and runs terminal commands for DollOS

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

//takes the finished command buffer, runs it, and clears command for the next entry
void commandProcessor(String& command) {
    if (command.length() == 0) {   //nothing typed, nothing to do
        return;
    }

    String entered = command;   //copy off the buffer before clearing it
    command = "";                //reset the shared buffer so the command bar goes blank

    String parts[4];
    int partCount = splitCommand(entered, parts, 4);
    //if no parts, stop here. 
    if (partCount == 0) {
        return;
    }
    if (parts[0] == "clear") {    //clear wipes history without echoing itself
        historyCount = 0;
        scrollOffset = 0;
        return;
    }

    addWrappedHistoryLine("> " + entered);   //echo the command into the terminal history

    if (parts[0] == "help") {
        addWrappedHistoryLine("Commands: help, clear");
        return;
    } 
    if (parts[0] == "wifi") {
        // Run the Wi-Fi scanner.
        handleWifiCommand(parts, partCount);
        return;

    } else {                                          //fallback for anything not recognized above
        addWrappedHistoryLine("Unknown command: " + entered);
    }
}
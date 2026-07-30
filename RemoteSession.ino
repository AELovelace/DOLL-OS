//   RemoteSession.ino
//   shared modal loop for character-oriented remote sessions -- see the RemoteSession class
//   declaration in global.h for what this is and why it's declared there instead of here.
void RemoteSession::run() {
    commandCursorPos = 0;   //no local editable buffer during a raw session; keeps drawInputRow's cursor bar sane
    drawTerminalHistory();
    drawInputRow();

    bool closedNotified = false;

    while (true) {
        M5Cardputer.update();
        delay(10);

        pumpIncoming();

        if (isClosed()) {
            if (!closedNotified) {
                onClosed();
                closedNotified = true;
            }
            break;
        }

        String rawOut;
        bool escapePressed;
        bool backspacePressed;
        bool cmdModePressed;
        if (readRawKeyBytes(rawOut, escapePressed, backspacePressed, cmdModePressed)) {
            sendBytes(rawOut);
        } else if (escapePressed) {
            break;
        } else if (cmdModePressed) {
            runInlineCommandPrompt();
        } else if (backspacePressed) {
            sendBytes(backspaceBytes());
        }

        drawTerminalHistory();
        drawInputRow();
    }

    //final drain so output already in flight isn't lost when the loop exits -- every character is
    //drawn live by terminalStreamPutChar, so there's no buffered tail to flush
    pumpIncoming();
}

//   Pauses raw byte forwarding just long enough for the user to type and run one full shell
//   command (e.g. "ip", "ls") via Fn+K, then resumes forwarding untouched. Dispatches through
//   commandProcessor() and returns to the raw session afterward rather than moving on to a new
//   phase. commandProcessor() no-ops on an empty line, so submitting nothing is the "back out"
//   gesture.
//
//   The remote is still pumped every pass: an ssh pty or telnet server doesn't stop sending
//   because we stopped reading, and letting the socket back up while the user types would at
//   best stall the session and at worst drop it.
void RemoteSession::runInlineCommandPrompt() {
    //   Save and restore the command-bar globals. readKeyboard() and drawCommandBar() both
    //   index into commandCursorPos/commandScrollOffset, and run() deliberately parked them
    //   at 0 for the raw session -- so this borrows them and puts them back, rather than
    //   leaving the caller's cursor bar pointing into a buffer that no longer exists.
    int savedCursorPos = commandCursorPos;
    int savedScrollOffset = commandScrollOffset;

    //   A dedicated buffer, not currentCommand: a half-typed inline command must not survive
    //   into the shell's own input line when the session ends.
    String cmdBuffer = "";
    commandCursorPos = 0;
    commandScrollOffset = 0;

    bool submitted = false;
    while (true) {
        M5Cardputer.update();
        delay(10);

        pumpIncoming();
        if (isClosed()) {
            break;   //remote died mid-typing -- discard whatever was half-entered
        }

        if (readKeyboard(cmdBuffer)) {
            submitted = true;
            break;
        }

        drawTerminalHistory();
        drawCommandBar("cmd> ", cmdBuffer);
    }

    //dispatch before restoring: commandProcessor() clears the buffer it's given and zeroes
    //both globals itself, so restoring first would just be undone here
    if (submitted) {
        commandProcessor(cmdBuffer);
    }

    commandCursorPos = savedCursorPos;
    commandScrollOffset = savedScrollOffset;
}

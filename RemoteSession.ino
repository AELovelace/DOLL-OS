//   RemoteSession.ino
//   shared modal loop for character-oriented remote sessions -- see the RemoteSession class
//   declaration in global.h for what this is and why it's declared there instead of here.
//stub. refine later
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
        if (readRawKeyBytes(rawOut, escapePressed, backspacePressed)) {
            sendBytes(rawOut);
        } else if (escapePressed) {
            break;
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

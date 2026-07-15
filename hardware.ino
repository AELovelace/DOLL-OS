//   hardware.ino
//   handles keyboard input and battery reads for DollOS
// wrote this myself. 
//Keyboard Management

const unsigned long KEYBOARD_DEBOUNCE_MS = 45;

static unsigned long lastKeyboardEventAt = 0;
static uint32_t lastKeyboardEventSignature = 0;

static uint32_t keyboardEventSignature(const Keyboard_Class::KeysState& keys) {
    uint32_t signature = 2166136261u;

    auto mixByte = [&signature](uint8_t value) {
        signature ^= value;
        signature *= 16777619u;
    };

    mixByte(keys.fn ? 1 : 0);
    mixByte(keys.ctrl ? 1 : 0);
    mixByte(keys.del ? 1 : 0);
    mixByte(keys.enter ? 1 : 0);

    for (char ch : keys.word) {
        mixByte((uint8_t)ch);
    }

    return signature;
}

static bool keyboardEventIsDebounced(const Keyboard_Class::KeysState& keys) {
    unsigned long now = millis();
    uint32_t signature = keyboardEventSignature(keys);

    if (signature == lastKeyboardEventSignature &&
        now - lastKeyboardEventAt < KEYBOARD_DEBOUNCE_MS) {
        return true;
    }

    lastKeyboardEventAt = now;
    lastKeyboardEventSignature = signature;
    return false;
}

//polls the keyboard once per loop and redraws/dispatches on change
void keyboardLogic(){
    int previousLength = currentCommand.length();        //snapshot enough state to detect an edit without copying the buffer
    int previousCursorPos = commandCursorPos;
    bool enterPressed = readKeyboard(currentCommand);     //read keyboard, updates currentCommand in place
    if(currentCommand.length() != previousLength || commandCursorPos != previousCursorPos){
        drawCommandBar(currentCommand);
    }

    if (enterPressed) {                //enter was hit, hand the finished command off to the processor
        commandProcessor(currentCommand);
    }
}

//checks whether target char is present in this keystate's word buffer
bool keysContainChar(const Keyboard_Class::KeysState& keys, char target) {
    for (char ch : keys.word) {   //loop over every character reported this keystate
        if (ch == target) {
            return true;
        }
    }
    return false;
}

//read the keyboard, appends to text in place and returns true once enter is pressed
bool readKeyboard(String& text) {
    //M5Cardputer.update();

    if (!M5Cardputer.Keyboard.isChange()) {   //nothing changed since last poll, bail early
        return false;
    }

    if (!M5Cardputer.Keyboard.isPressed()) {   //change was a key release, not a press
        return false;
    }

    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    if (keyboardEventIsDebounced(keys)) {
        return false;
    }

    if (keys.fn) {                              //fn held, treat this as a scroll/shortcut combo instead of text entry
        if (keysContainChar(keys, ';')) {       //fn + ; scrolls the terminal history down
            scrollHistory(1);
            return false;
        }

        if (keysContainChar(keys, '.')) {       //fn + . scrolls the terminal history up
            scrollHistory(-1);
            return false;
        }

        if (keysContainChar(keys, ',')) {       //fn + , moves the cursor left within the command buffer
            if (commandCursorPos > 0) {
                commandCursorPos--;
            }
            return false;
        }

        if (keysContainChar(keys, '/')) {       //fn + / moves the cursor right within the command buffer
            if (commandCursorPos < text.length()) {
                commandCursorPos++;
            }
            return false;
        }
        return false; // Ignore other function key combinations for now
    }

    if (keys.ctrl) {                            //ctrl held, recall previously sent commands instead of typing
        //the driver reports the shifted variant ('>' / ':') while ctrl is held, not the bare '.' / ';'
        if (keysContainChar(keys, ':')) {       //ctrl + ; recalls older commands, like pressing up in a shell
            recallCommandHistory(-1, text);
            commandCursorPos = text.length();   //jump cursor to end of recalled text, like a shell
            return false;
        }

        if (keysContainChar(keys, '>')) {       //ctrl + . recalls newer commands, like pressing down in a shell
            recallCommandHistory(1, text);
            commandCursorPos = text.length();   //jump cursor to end of recalled text, like a shell
            return false;
        }
        return false; // Ignore other ctrl combinations for now
    }

    // Exclusive branch: on this keyboard, .word can carry stray characters alongside
    // a backspace press, so handle del on its own and skip the append below.
    if (keys.del) {
        if (commandCursorPos > 0) {
            text.remove(commandCursorPos - 1, 1);   //remove the character just before the cursor
            commandCursorPos--;
        }
    } else {
        // Insert normal characters at the cursor position.
        text.reserve(text.length() + keys.word.size());
        for (char character : keys.word) {
            int oldLength = text.length();
            text += ' ';
            for (int i = oldLength; i > commandCursorPos; i--) {
                text.setCharAt(i, text[i - 1]);
            }
            text.setCharAt(commandCursorPos, character);
            commandCursorPos++;
        }
    }

    // Signal that the command is complete.
    return keys.enter;
}

//translates one raw keystroke into the literal bytes a remote character-oriented stream expects
//(ssh shell pty, telnet in character mode). Unlike readKeyboard, nothing is buffered or edited
//locally -- every keypress becomes bytes immediately and is handed back for the caller to forward,
//the same way a real terminal emulator feeds a pty. Used by RemoteSession::run() (RemoteSession.ino).
//
//Returns true if this keystroke produced bytes to send (outBytes is only meaningful then).
//escapePressed is set instead when the user hit the local Fn+Q "disconnect" chord; backspacePressed
//is set instead when the user hit backspace/delete -- callers translate that to the right byte(s)
//for their transport via RemoteSession::backspaceBytes() rather than a byte hardcoded here.
//vibe stub. i got overwhelmed and opened vscode. 
bool readRawKeyBytes(String& outBytes, bool& escapePressed, bool& backspacePressed) {
    outBytes = "";
    escapePressed = false;
    backspacePressed = false;

    if (!M5Cardputer.Keyboard.isChange()) {
        return false;
    }
    if (!M5Cardputer.Keyboard.isPressed()) {
        return false;
    }

    Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
    if (keyboardEventIsDebounced(keys)) {
        return false;
    }

    if (keys.fn) {                              //fn combos stay local -- scrolling and the escape chord, never sent to the remote
        if (keysContainChar(keys, ';')) {
            scrollHistory(1);
        } else if (keysContainChar(keys, '.')) {
            scrollHistory(-1);
        } else if (keysContainChar(keys, 'q')) {
            escapePressed = true;
        }
        return false;
    }

    if (keys.ctrl) {                            //forward ctrl+letter as its control byte (ctrl+c -> 0x03, etc.) --
        for (char c : keys.word) {              //remote shells and telehack alike rely on these for signals/line-editing
            char lower = tolower((unsigned char)c);
            if (lower >= 'a' && lower <= 'z') {
                outBytes += (char)(lower - 'a' + 1);
            }
        }
        return outBytes.length() > 0;
    }

    if (keys.del) {
        backspacePressed = true;
        return false;
    }

    if (keys.enter) {
        outBytes = "\r";
        return true;
    }

    for (char c : keys.word) {
        outBytes += c;
    }
    return outBytes.length() > 0;
}

//TODO: read battery percent from M5Cardputer.Power, mirrors statusManagement's inline read
int batteryPercentCheck(){

}

//TODO: read battery millivolts from M5Cardputer.Power, mirrors statusManagement's inline read
int batteryVoltCheck(){

}
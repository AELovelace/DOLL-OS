//   hardware.ino
//   handles keyboard input and battery reads for DollOS
// wrote this myself. 
//Keyboard Management

//polls the keyboard once per loop and redraws/dispatches on change
void keyboardLogic(){
    String previousCommand = currentCommand;             //snapshot command buffer before polling
    bool enterPressed = readKeyboard(currentCommand);     //read keyboard, updates currentCommand in place
    if(currentCommand != previousCommand){                //only redraw the command bar if the text actually changed
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
        if (keysContainChar(keys, '.')) {       //ctrl + . recalls older commands, like pressing up in a shell
            recallCommandHistory(-1, text);
            commandCursorPos = text.length();   //jump cursor to end of recalled text, like a shell
            return false;
        }

        if (keysContainChar(keys, ';')) {       //ctrl + ; recalls newer commands, like pressing down in a shell
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
        for (char character : keys.word) {
            text = text.substring(0, commandCursorPos) + character + text.substring(commandCursorPos);
            commandCursorPos++;
        }
    }

    // Signal that the command is complete.
    return keys.enter;
}

//TODO: read battery percent from M5Cardputer.Power, mirrors statusManagement's inline read
int batteryPercentCheck(){

}

//TODO: read battery millivolts from M5Cardputer.Power, mirrors statusManagement's inline read
int batteryVoltCheck(){

}
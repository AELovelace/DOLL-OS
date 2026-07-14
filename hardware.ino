//   hardware.ino
//   handles keyboard input and battery reads for DollOS

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
        return false; // Ignore other function key combinations for now
    }

    // Exclusive branch: on this keyboard, .word can carry stray characters alongside
    // a backspace press, so handle del on its own and skip the append below.
    if (keys.del) {
        if (text.length() > 0) {
            text.remove(text.length() - 1);
        }
    } else {
        // Add normal characters.
        for (char character : keys.word) {
            text += character;
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
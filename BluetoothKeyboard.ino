//   BluetoothKeyboard.ino
//   turns the Cardputer into a BLE HID keyboard until Fn+Q exits back to DOLL-OS

#include <ESP32BLECombo.h>

static ESP32BLECombo bleKeyboard;
static bool bleKeyboardStarted = false;

static const uint8_t BLE_HID_RAW_OFFSET = 136;
static const uint8_t BLE_MODIFIER_OFFSET = 128;

static void bleKeyboardPressRaw(uint8_t hidKey) {
    bleKeyboard.press(BLE_HID_RAW_OFFSET + hidKey);
}

static void bleKeyboardPressModifiers(uint8_t modifiers) {
    for (uint8_t bit = 0; bit < 8; bit++) {
        if (modifiers & (1 << bit)) {
            bleKeyboard.press(BLE_MODIFIER_OFFSET + bit);
        }
    }
}

static bool bleKeyboardKeysContainAnyChar(const Keyboard_Class::KeysState& keys, char first, char second) {
    return keysContainChar(keys, first) || keysContainChar(keys, second);
}

static void bleKeyboardTapRaw(uint8_t hidKey, uint8_t modifiers = 0) {
    bleKeyboardPressModifiers(modifiers);
    bleKeyboardPressRaw(hidKey);
    delay(8);
    bleKeyboard.releaseAll();
}

static void bleKeyboardSendKeys(const Keyboard_Class::KeysState& keys) {
    bleKeyboardPressModifiers(keys.modifiers);

    int sentKeys = 0;
    for (uint8_t hidKey : keys.hid_keys) {
        if (sentKeys >= 6) {
            break;   //HID keyboard reports carry at most six simultaneous non-modifier keys
        }
        bleKeyboardPressRaw(hidKey);
        sentKeys++;
    }

    delay(8);
    bleKeyboard.releaseAll();
}

static bool bleKeyboardHandleFnChord(const Keyboard_Class::KeysState& keys, bool& exitRequested) {
    exitRequested = false;

    if (!keys.fn) {
        return false;
    }

    if (keysContainChar(keys, 'q') || keysContainChar(keys, 'Q')) {
        exitRequested = true;
        return true;
    }

    if (!bleKeyboard.isConnected()) {
        return true;   //local Fn chords should not leak as printable keys while waiting to pair
    }

    if (bleKeyboardKeysContainAnyChar(keys, ';', ':')) {
        bleKeyboardTapRaw(0x52);   //Up arrow
    } else if (bleKeyboardKeysContainAnyChar(keys, '.', '>')) {
        bleKeyboardTapRaw(0x51);   //Down arrow
    } else if (bleKeyboardKeysContainAnyChar(keys, ',', '<')) {
        bleKeyboardTapRaw(0x50);   //Left arrow
    } else if (bleKeyboardKeysContainAnyChar(keys, '/', '?')) {
        bleKeyboardTapRaw(0x4F);   //Right arrow
    }

    return true;
}

static void drawBleKeyboardStatus(const String& deviceName) {
    String status = bleKeyboard.isConnected() ? "paired" : "pairing";
    drawCommandBar("bt> ", status + " " + deviceName + "  Fn+Q quit");
}

static bool ensureBleKeyboardStarted(const String& deviceName) {
    if (bleKeyboardStarted) {
        return true;
    }

    ESP32BLEComboConfig cfg;
    cfg.mode = ESP32BLEComboMode::KEYBOARD_ONLY;
    cfg.deviceName = deviceName;
    cfg.manufacturer = "DOLL-OS";
    cfg.batteryLevel = constrain(readBatteryPercent(), 0, 100);
    cfg.appearance = ESP32BLEComboAppearance::KEYBOARD;
    cfg.enableSecurity = true;
    cfg.keyPressDelayMs = 8;
    cfg.keyReleaseDelayMs = 8;
    cfg.keyIntervalDelayMs = 4;

    if (!bleKeyboard.begin(cfg)) {
        return false;
    }

    bleKeyboardStarted = true;
    return true;
}

static void runBluetoothKeyboardMode(const String& deviceName) {
    if (!ensureBleKeyboardStarted(deviceName)) {
        outLine("btkbd: failed to start BLE HID", C_RED);
        return;
    }

    outLine("Bluetooth keyboard mode on", C_CYAN);
    outLine("Pair with: " + deviceName);
    outLine("Fn+Q exits. Fn+; Fn+. Fn+, Fn+/ send arrow keys.");

    bool wasConnected = bleKeyboard.isConnected();
    unsigned long lastBatteryUpdateAt = 0;

    while (true) {
        M5Cardputer.update();
        delay(10);

        unsigned long now = millis();
        if (now - lastBatteryUpdateAt > 30000) {
            bleKeyboard.setBatteryLevel(constrain(readBatteryPercent(), 0, 100));
            lastBatteryUpdateAt = now;
        }

        bool connected = bleKeyboard.isConnected();
        if (connected != wasConnected) {
            outLine(connected ? "btkbd: host connected" : "btkbd: host disconnected", connected ? C_GREEN : C_YELLOW);
            wasConnected = connected;
        }

        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
            if (keyboardEventIsDebounced(keys)) {
                drawTerminalHistory();
                drawBleKeyboardStatus(deviceName);
                continue;
            }

            bool exitRequested;
            if (bleKeyboardHandleFnChord(keys, exitRequested)) {
                if (exitRequested) {
                    break;
                }
            } else if (connected) {
                bleKeyboardSendKeys(keys);
            }
        }

        drawTerminalHistory();
        drawBleKeyboardStatus(deviceName);
    }

    bleKeyboard.releaseAll();
    bleKeyboard.end();
    bleKeyboardStarted = false;
    outLine("Bluetooth keyboard mode off", C_YELLOW);
}

void handleBluetoothKeyboardCommand(const String parts[], int partCount) {
    if (partCount > 1 && parts[1] == "help") {
        outLine("Usage: btkbd [device-name]");
        outLine("Turns this Cardputer into a BLE keyboard until Fn+Q.");
        outLine("Arrow keys: Fn+; up, Fn+. down, Fn+, left, Fn+/ right.");
        return;
    }

    String deviceName = "DOLL-OS Keyboard";
    if (partCount > 1) {
        deviceName = parts[1];
    }
    deviceName.trim();
    if (deviceName.length() == 0) {
        deviceName = "DOLL-OS Keyboard";
    }

    runBluetoothKeyboardMode(deviceName);
}

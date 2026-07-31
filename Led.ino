//   Led.ino
//   Cardputer RGB LED support shared by native modules and .dapp's LED opcode.

static uint8_t ledClampByte(long value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

bool rearLedAvailable() {
    return M5.Led.getCount() > 0;
}

void rearLedSetRgb(uint8_t red, uint8_t green, uint8_t blue) {
    if (!rearLedAvailable()) return;
    M5.Led.setColor(0, red, green, blue);
}

void rearLedSetRgbLong(long red, long green, long blue) {
    rearLedSetRgb(ledClampByte(red), ledClampByte(green), ledClampByte(blue));
}

void rearLedOff() {
    rearLedSetRgb(0, 0, 0);
}

static bool ledAppOverride = false;
static LedRgb ledAppColor = { 0, 0, 0 };
static LedRgb ledPulseColor = { 0, 0, 0 };
static unsigned long ledPulseUntilMs = 0;
static LedRgb ledLastColor = { 255, 255, 255 };

static void ledWriteIfChanged(LedRgb color) {
    if (color.red == ledLastColor.red && color.green == ledLastColor.green &&
        color.blue == ledLastColor.blue) return;
    rearLedSetRgb(color.red, color.green, color.blue);
    ledLastColor = color;
}

static void ledPulse(LedRgb color, unsigned long durationMs) {
    ledPulseColor = color;
    ledPulseUntilMs = millis() + durationMs;
}

void ledBegin() {
    if (rearLedAvailable()) M5.Led.setBrightness(255);
    ledLastColor = { 255, 255, 255 };
    ledWriteIfChanged({ 0, 0, 0 });
}

void ledService() {
    if (!rearLedAvailable()) return;
    unsigned long now = millis();
    if ((long)(ledPulseUntilMs - now) > 0) {
        ledWriteIfChanged(ledPulseColor);
    } else if (ledAppOverride) {
        ledWriteIfChanged(ledAppColor);
    } else {
        ledWriteIfChanged({ 0, 0, 0 });
    }
}

void ledPulseStorageRead(bool isSd) {
    ledPulse(isSd ? LedRgb{ 0, 90, 255 } : LedRgb{ 90, 60, 0 }, 90);
}

void ledPulseStorageWrite(bool isSd) {
    ledPulse(isSd ? LedRgb{ 255, 255, 255 } : LedRgb{ 255, 90, 0 }, 140);
}

void ledPulseNetwork() { ledPulse({ 0, 180, 180 }, 90); }
void ledPulseInput() { ledPulse({ 180, 0, 180 }, 70); }
void ledPulseError() { ledPulse({ 255, 0, 0 }, 220); }

void ledSetAppOverrideRgb(uint8_t red, uint8_t green, uint8_t blue) {
    ledAppOverride = true;
    ledAppColor = { red, green, blue };
}

void ledSetAppOverrideRgbLong(long red, long green, long blue) {
    ledSetAppOverrideRgb(ledClampByte(red), ledClampByte(green), ledClampByte(blue));
}

void ledClearAppOverride() {
    ledAppOverride = false;
    ledAppColor = { 0, 0, 0 };
}

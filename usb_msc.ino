//   usb_msc.ino
//   exposes the mounted SD card to a PC as a USB Mass Storage drive
//   entered/exited via the blocking "usb" terminal command (Fn+` to exit)

#include <Arduino.h>
#if !SOC_USB_OTG_SUPPORTED || ARDUINO_USB_MODE
#error This board's USB Mode must be set to "USB-OTG (TinyUSB)" for USB Mass Storage support
#else

#include <USB.h>
#include <USBMSC.h>

USBMSC msc;
bool mscStarted = false;   //true once vendorID/callbacks/msc.begin()/USB.begin() have run (only ever needs to happen once)

static int32_t onMscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    uint32_t secSize = SD.sectorSize();
    if (!secSize) {
        return -1;
    }
    for (uint32_t i = 0; i < bufsize / secSize; i++) {
        if (!SD.readRAW((uint8_t*)buffer + (i * secSize), lba + i)) {
            return -1;
        }
    }
    return bufsize;
}

static int32_t onMscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    uint32_t secSize = SD.sectorSize();
    if (!secSize) {
        return -1;
    }
    for (uint32_t i = 0; i < bufsize / secSize; i++) {
        if (!SD.writeRAW(buffer + (i * secSize), lba + i)) {
            return -1;
        }
    }
    return bufsize;
}

static bool onMscStartStop(uint8_t power_condition, bool start, bool load_eject) {
    return true;
}

//blocks until Fn+` is pressed, keeping the SD card exposed as a USB drive the whole time
void runUsbModeBlocking() {
    drawUsbWarning();

    while (true) {
        M5Cardputer.update();
        delay(10);

        if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
            continue;
        }

        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
        if (keys.fn && keysContainChar(keys, '`')) {
            break;
        }
    }

    msc.mediaPresent(false);

    drawTerminalHistory();
    drawCommandBar(currentCommand);
    addWrappedHistoryLine("USB mode off");
}

//handles the "usb" command: exposes the SD card over USB MSC and blocks until the user exits
void handleUsbCommand(const String parts[], int partCount) {
    if (!sdCardMounted) {
        addWrappedHistoryLine("usb: SD card not mounted");
        return;
    }

    if (!mscStarted) {
        msc.vendorID("DOLLOS");
        msc.productID("SD Card");
        msc.productRevision("1.0");
        msc.onRead(onMscRead);
        msc.onWrite(onMscWrite);
        msc.onStartStop(onMscStartStop);
        msc.isWritable(true);
        msc.begin(SD.numSectors(), SD.sectorSize());
        USB.begin();
        mscStarted = true;
    }

    msc.mediaPresent(true);
    addWrappedHistoryLine("USB mode on (Fn+` to exit)");
    runUsbModeBlocking();
}

#endif /* !SOC_USB_OTG_SUPPORTED || ARDUINO_USB_MODE */

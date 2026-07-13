//   storage.ino
//   mounts internal (LittleFS) and SD storage and provides filesystem test commands

#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

//SD card is wired to its own SPI bus on the Cardputer (CS=12, MOSI=14, MISO=39, SCK=40),
//separate from the bus M5Cardputer.begin() sets up for the display
const int SD_CS_PIN = 12;
const int SD_MOSI_PIN = 14;
const int SD_MISO_PIN = 39;
const int SD_SCK_PIN = 40;

//mounts LittleFS (formatting it on first boot if needed) and the SD card, called once from setup()
void initStorage() {
    if (!LittleFS.begin(true)) {   //true = format LittleFS if mount fails (e.g. first boot)
        addWrappedHistoryLine("LittleFS: mount failed");
    }

    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sdCardMounted = SD.begin(SD_CS_PIN, SPI);
    if (!sdCardMounted) {
        addWrappedHistoryLine("SD: not detected");
    }
}

//lists one directory of a mounted filesystem into the terminal history
void listDirectory(fs::FS& fs, const String& path) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) {
        addWrappedHistoryLine("ls: " + path + " not found");
        return;
    }

    File entry = dir.openNextFile();
    int entryCount = 0;
    while (entry) {
        String line = entry.isDirectory() ? "  [DIR]  " : ("  " + String(entry.size()) + "b  ");
        line += entry.name();
        addWrappedHistoryLine(line);
        entryCount++;
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    if (entryCount == 0) {
        addWrappedHistoryLine("(empty)");
    }
}

//handles the "ls" command, lists LittleFS by default or the SD card with -sd
//
//Expected forms:
//ls
//ls <path>
//ls -sd
//ls -sd <path>
void handleLsCommand(const String parts[], int partCount) {
    bool useSd = (partCount > 1 && parts[1] == "-sd");

    String path = "/";
    if (useSd && partCount > 2) {
        path = parts[2];
    } else if (!useSd && partCount > 1) {
        path = parts[1];
    }

    if (useSd) {
        if (!sdCardMounted) {
            addWrappedHistoryLine("SD not mounted (insert card and reboot)");
            return;
        }
        addWrappedHistoryLine("SD:" + path);
        listDirectory(SD, path);
    } else {
        addWrappedHistoryLine("Flash:" + path);
        listDirectory(LittleFS, path);
    }
}

//   storage.ino
//   mounts internal (LittleFS) and SD storage and provides filesystem test commands
//vibe-coded. is a stub
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

//lists one directory of a mounted filesystem into the terminal history.
//showSdMount adds a synthetic "sd" entry, used when listing flash root so the mount point is discoverable
void listDirectory(fs::FS& fs, const String& path, bool showSdMount) {
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

    if (showSdMount) {
        addWrappedHistoryLine("  [DIR]  sd");
        entryCount++;
    }

    if (entryCount == 0) {
        addWrappedHistoryLine("(empty)");
    }
}

//routes an absolute path in the unified namespace to the physical filesystem that owns it.
//paths at or under SD_MOUNT map onto the SD card with the mount prefix stripped; everything else is LittleFS
RoutedPath routePath(const String& resolvedPath) {
    bool onSd = (resolvedPath == SD_MOUNT) || resolvedPath.startsWith(SD_MOUNT + "/");
    if (onSd) {
        String realPath = resolvedPath.substring(SD_MOUNT.length());
        if (realPath.length() == 0) realPath = "/";
        return { &SD, realPath, true };
    }
    return { &LittleFS, resolvedPath, false };
}

//collapses "inputPath" (relative or absolute) against cwd into a clean absolute path in the
//unified namespace, resolving "." and ".." segments. Pure string math - knows nothing about
//LittleFS or SD; routePath() is the only place that maps the result onto a physical filesystem
String resolvePath(const String& cwd, const String& inputPath) {
    String combined = (inputPath.length() > 0 && inputPath[0] == '/')
        ? inputPath
        : cwd + "/" + inputPath;

    String stack[16];   //max path depth this shell will track
    int depth = 0;

    int start = 0;
    while (start < combined.length()) {
        while (start < combined.length() && combined[start] == '/') start++;
        int end = combined.indexOf('/', start);
        if (end == -1) end = combined.length();
        String segment = combined.substring(start, end);

        if (segment.length() == 0 || segment == ".") {
            //skip
        } else if (segment == "..") {
            if (depth > 0) depth--;
        } else if (depth < 16) {
            stack[depth++] = segment;
        }
        start = end;
    }

    String result = "/";
    for (int i = 0; i < depth; i++) {
        result += stack[i];
        if (i < depth - 1) result += "/";
    }
    return result;
}

//true if resolvedPath is a real, openable directory once routed to its physical filesystem
bool directoryExists(const String& resolvedPath) {
    RoutedPath r = routePath(resolvedPath);
    if (r.isSd && !sdCardMounted) {
        return false;
    }
    File dir = r.fs->open(r.realPath);
    bool ok = dir && dir.isDirectory();
    if (dir) dir.close();
    return ok;
}

//handles the "ls" command against the current working directory, or a path (relative or
//absolute) given as an argument. Transparently follows the SD_MOUNT seam into the SD card
void handleLsCommand(const String parts[], int partCount) {
    String target = (partCount > 1) ? parts[1] : "";
    String resolved = resolvePath(cwd, target);

    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        addWrappedHistoryLine("SD not mounted (insert card and reboot)");
        return;
    }

    addWrappedHistoryLine(resolved);
    listDirectory(*r.fs, r.realPath, !r.isSd && resolved == "/" && sdCardMounted);
}

//handles the "cd" command; bare "cd" returns to "/". Refuses to move into a path that
//doesn't resolve to a real directory (or the SD mount without a card present)
void handleCdCommand(const String parts[], int partCount) {
    String target = (partCount > 1) ? parts[1] : "/";
    String resolved = resolvePath(cwd, target);

    if (!directoryExists(resolved)) {
        addWrappedHistoryLine("cd: " + resolved + " not found");
        return;
    }
    cwd = resolved;
}

//handles the "pwd" command
void handlePwdCommand(const String parts[], int partCount) {
    addWrappedHistoryLine(cwd);
}

//handles the "cat" command; prints a text file into terminal history.
void handleCatCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        addWrappedHistoryLine("Usage: cat <file>");
        return;
    }

    String target = parts[1];
    String resolved = resolvePath(cwd, target);
    RoutedPath r = routePath(resolved);

    if (r.isSd && !sdCardMounted) {
        addWrappedHistoryLine("SD not mounted (insert card and reboot)");
        return;
    }

    File file = r.fs->open(r.realPath, "r");
    if (!file) {
        addWrappedHistoryLine("cat: " + resolved + " not found");
        return;
    }

    if (file.isDirectory()) {
        addWrappedHistoryLine("cat: " + resolved + " is a directory");
        file.close();
        return;
    }

    if (file.size() == 0) {
        addWrappedHistoryLine("(empty)");
        file.close();
        return;
    }

    while (file.available()) {
        String line = file.readStringUntil('\n');

        // readStringUntil('\n') can leave a trailing '\r' on CRLF files.
        if (line.endsWith("\r")) {
            line.remove(line.length() - 1);
        }

        addWrappedHistoryLine(line);
    }

    file.close();
}

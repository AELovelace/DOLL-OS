//   storage.ino
//   mounts internal (LittleFS) and SD storage and provides the filesystem commands.
//   The path-resolution/routing half was always DOLL-OS's own; the mutating commands
//   (mkdir/rm/del/cp/mv) came back from DS, where routePath()/resolvePath()/RoutedPath
//   are byte-identical -- the only transport difference is that this board wires SD
//   over SPI where DS uses the S3's SDMMC peripheral, so &SD stands in for &SD_MMC.
//   See docs/PORT-FROM-DS.md, Phase 3.
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
    //begin(true) asks the LittleFS driver to auto-format when the mount fails, which covers
    //a blank first-boot partition -- including the one right after switching to this sketch's
    //partitions.csv, which moves the partition and so presents as blank. But a partition left
    //half-written isn't reliably recovered by that path, and booting with settings storage dead
    //means no saved wifi.cfg and a silent fall back to config.h's defaults forever. So on
    //failure, force an explicit format + clean remount: settings reset, but storage lives.
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS: mount failed; forcing format...");
        if (LittleFS.format() && LittleFS.begin(false)) {
            outLine("LittleFS: was corrupt -- reformatted (settings reset)", C_YELLOW);
        } else {
            outLine("LittleFS: mount failed -- settings won't persist", C_RED);
        }
    }

    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sdCardMounted = SD.begin(SD_CS_PIN, SPI);
    if (!sdCardMounted) {
        outLine("SD: not detected");
    }
}

//lists one directory of a mounted filesystem into the terminal history.
//showSdMount adds a synthetic "sd" entry, used when listing flash root so the mount point is discoverable
void listDirectory(fs::FS& fs, const String& path, bool showSdMount) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) {
        outLine("ls: " + path + " not found", C_RED);
        return;
    }

    File entry = dir.openNextFile();
    int entryCount = 0;
    while (entry) {
        String line = entry.isDirectory() ? "  [DIR]  " : ("  " + String(entry.size()) + "b  ");
        line += entry.name();
        outLine(line);
        entryCount++;
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    if (showSdMount) {
        outLine("  [DIR]  sd");
        entryCount++;
    }

    if (entryCount == 0) {
        outLine("(empty)");
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

//   Path helpers for the mutating commands below. Ported from DS.

//the two paths no command may remove or overwrite: the flash root and the SD mount point.
//Neither is a real directory entry that could be recreated, so losing one would strand the
//namespace with no way back short of a reflash.
static bool pathIsProtectedRoot(const String& resolvedPath) {
    return resolvedPath == "/" || resolvedPath == SD_MOUNT;
}

static String parentPathOf(const String& resolvedPath) {
    if (resolvedPath == "/" || resolvedPath.length() == 0) {
        return "/";
    }
    int slash = resolvedPath.lastIndexOf('/');
    if (slash <= 0) {
        return "/";
    }
    return resolvedPath.substring(0, slash);
}

static String baseNameOf(const String& resolvedPath) {
    if (resolvedPath == "/" || resolvedPath.length() == 0) {
        return "";
    }
    int slash = resolvedPath.lastIndexOf('/');
    if (slash < 0) {
        return resolvedPath;
    }
    return resolvedPath.substring(slash + 1);
}

//builds a child's path in the *unified* namespace from its parent's. childName may come back
//from openNextFile() as an absolute path in the underlying filesystem's own terms, which for
//the SD card is missing the SD_MOUNT prefix -- hence the isSd branch.
static String childPathOf(const String& parentResolved, bool parentIsSd, const String& childName) {
    if (childName.startsWith("/")) {
        return parentIsSd ? SD_MOUNT + childName : childName;
    }
    if (parentResolved == "/") {
        return "/" + childName;
    }
    return parentResolved + "/" + childName;
}

static bool ensureMountedForPath(const String& commandName, const RoutedPath& r) {
    if (r.isSd && !sdCardMounted) {
        outLine(commandName + ": SD not mounted (insert card and reboot)", C_RED);
        return false;
    }
    return true;
}

//removes a file, an empty directory, or (with recursive) a whole subtree. Recurses through
//childPathOf so every level re-routes properly across the SD_MOUNT seam.
static bool removeResolvedPath(const String& commandName, const String& resolvedPath, bool recursive) {
    if (pathIsProtectedRoot(resolvedPath)) {
        outLine(commandName + ": refusing to remove " + resolvedPath, C_RED);
        return false;
    }

    RoutedPath r = routePath(resolvedPath);
    if (!ensureMountedForPath(commandName, r)) {
        return false;
    }

    File entry = r.fs->open(r.realPath);
    if (!entry) {
        outLine(commandName + ": " + resolvedPath + " not found", C_RED);
        return false;
    }

    if (!entry.isDirectory()) {
        entry.close();
        if (!r.fs->remove(r.realPath)) {
            outLine(commandName + ": could not remove " + resolvedPath, C_RED);
            return false;
        }
        return true;
    }

    if (!recursive) {
        File child = entry.openNextFile();
        bool hasChild = child;
        if (child) {
            child.close();
        }
        entry.close();
        if (hasChild) {
            outLine(commandName + ": " + resolvedPath + " is a directory (use -r)", C_RED);
            return false;
        }
        if (!r.fs->rmdir(r.realPath)) {
            outLine(commandName + ": could not remove directory " + resolvedPath, C_RED);
            return false;
        }
        return true;
    }

    File child = entry.openNextFile();
    while (child) {
        String childName = child.name();
        child.close();
        if (!removeResolvedPath(commandName, childPathOf(resolvedPath, r.isSd, childName), true)) {
            entry.close();
            return false;
        }
        child = entry.openNextFile();
    }
    entry.close();

    if (!r.fs->rmdir(r.realPath)) {
        outLine(commandName + ": could not remove directory " + resolvedPath, C_RED);
        return false;
    }
    return true;
}

//copies one file, streaming through a small stack buffer so file size doesn't bound the copy.
//Works across the LittleFS/SD seam, which is why it can't just be a rename.
static bool copyResolvedFile(const String& sourceResolved, const String& destResolved, bool overwrite) {
    RoutedPath source = routePath(sourceResolved);
    RoutedPath dest = routePath(destResolved);
    if (!ensureMountedForPath("cp", source) || !ensureMountedForPath("cp", dest)) {
        return false;
    }

    File in = source.fs->open(source.realPath, "r");
    if (!in) {
        outLine("cp: " + sourceResolved + " not found", C_RED);
        return false;
    }
    if (in.isDirectory()) {
        in.close();
        outLine("cp: " + sourceResolved + " is a directory", C_RED);
        return false;
    }

    //"cp foo /some/dir" means "into that directory", the same as a real shell
    String finalDestResolved = destResolved;
    File destProbe = dest.fs->open(dest.realPath);
    if (destProbe && destProbe.isDirectory()) {
        destProbe.close();
        finalDestResolved = destResolved;
        if (!finalDestResolved.endsWith("/")) {
            finalDestResolved += "/";
        }
        finalDestResolved += baseNameOf(sourceResolved);
        dest = routePath(finalDestResolved);
    } else if (destProbe) {
        destProbe.close();
    }

    if (pathIsProtectedRoot(finalDestResolved)) {
        in.close();
        outLine("cp: invalid destination " + finalDestResolved, C_RED);
        return false;
    }

    File existing = dest.fs->open(dest.realPath);
    if (existing) {
        bool isDir = existing.isDirectory();
        existing.close();
        if (isDir) {
            in.close();
            outLine("cp: " + finalDestResolved + " is a directory", C_RED);
            return false;
        }
        if (!overwrite) {
            in.close();
            outLine("cp: " + finalDestResolved + " already exists", C_RED);
            return false;
        }
        dest.fs->remove(dest.realPath);
    }

    String parent = parentPathOf(finalDestResolved);
    if (!directoryExists(parent)) {
        in.close();
        outLine("cp: destination directory not found: " + parent, C_RED);
        return false;
    }

    File out = dest.fs->open(dest.realPath, "w");
    if (!out) {
        in.close();
        outLine("cp: could not create " + finalDestResolved, C_RED);
        return false;
    }

    uint8_t buffer[256];
    while (in.available()) {
        size_t readCount = in.read(buffer, sizeof(buffer));
        if (readCount == 0) {
            break;
        }
        if (out.write(buffer, readCount) != readCount) {
            out.close();
            in.close();
            //a partial file is worse than no file -- the user would have no way to tell
            dest.fs->remove(dest.realPath);
            outLine("cp: write failed for " + finalDestResolved, C_RED);
            return false;
        }
        delay(1);   //yield so a large copy doesn't starve the watchdog
    }

    out.close();
    in.close();
    return true;
}

//handles the "ls" command against the current working directory, or a path (relative or
//absolute) given as an argument. Transparently follows the SD_MOUNT seam into the SD card
void handleLsCommand(const String parts[], int partCount) {
    String target = (partCount > 1) ? parts[1] : "";
    String resolved = resolvePath(cwd, target);

    RoutedPath r = routePath(resolved);
    if (r.isSd && !sdCardMounted) {
        outLine("SD not mounted (insert card and reboot)", C_RED);
        return;
    }

    outLine(resolved);
    listDirectory(*r.fs, r.realPath, !r.isSd && resolved == "/" && sdCardMounted);
}

//handles the "cd" command; bare "cd" returns to "/". Refuses to move into a path that
//doesn't resolve to a real directory (or the SD mount without a card present)
void handleCdCommand(const String parts[], int partCount) {
    String target = (partCount > 1) ? parts[1] : "/";
    String resolved = resolvePath(cwd, target);

    if (!directoryExists(resolved)) {
        outLine("cd: " + resolved + " not found", C_RED);
        return;
    }
    cwd = resolved;
}

//handles the "pwd" command
void handlePwdCommand(const String parts[], int partCount) {
    outLine(cwd);
}

//handles the "cat" command; prints a text file into terminal history.
void handleCatCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: cat <file>");
        return;
    }

    String target = parts[1];
    String resolved = resolvePath(cwd, target);
    RoutedPath r = routePath(resolved);

    if (r.isSd && !sdCardMounted) {
        outLine("SD not mounted (insert card and reboot)", C_RED);
        return;
    }

    File file = r.fs->open(r.realPath, "r");
    if (!file) {
        outLine("cat: " + resolved + " not found", C_RED);
        return;
    }

    if (file.isDirectory()) {
        outLine("cat: " + resolved + " is a directory", C_RED);
        file.close();
        return;
    }

    if (file.size() == 0) {
        outLine("(empty)");
        file.close();
        return;
    }

    while (file.available()) {
        String line = file.readStringUntil('\n');

        // readStringUntil('\n') can leave a trailing '\r' on CRLF files.
        if (line.endsWith("\r")) {
            line.remove(line.length() - 1);
        }

        outLine(line);
    }

    file.close();
}

//handles "mkdir <dir>". Only creates one level -- the parent must already exist, so a typo
//deep in a path fails loudly instead of quietly building a tree nobody asked for.
void handleMkdirCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: mkdir <dir>");
        return;
    }

    String resolved = resolvePath(cwd, parts[1]);
    if (pathIsProtectedRoot(resolved)) {
        outLine("mkdir: invalid path " + resolved, C_RED);
        return;
    }

    RoutedPath r = routePath(resolved);
    if (!ensureMountedForPath("mkdir", r)) {
        return;
    }

    File existing = r.fs->open(r.realPath);
    if (existing) {
        existing.close();
        outLine("mkdir: " + resolved + " already exists", C_RED);
        return;
    }

    String parent = parentPathOf(resolved);
    if (!directoryExists(parent)) {
        outLine("mkdir: parent not found: " + parent, C_RED);
        return;
    }

    if (!r.fs->mkdir(r.realPath)) {
        outLine("mkdir: could not create " + resolved, C_RED);
        return;
    }
    outLine("mkdir: created " + resolved, C_GREEN);
}

//handles "rm [-r] <path>"
void handleRmCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: rm [-r] <path>");
        return;
    }

    bool recursive = false;
    int targetIndex = 1;
    if (parts[1] == "-r" || parts[1] == "-R") {
        recursive = true;
        targetIndex = 2;
    }
    if (targetIndex >= partCount) {
        outLine("Usage: rm [-r] <path>");
        return;
    }

    String resolved = resolvePath(cwd, parts[targetIndex]);
    if (removeResolvedPath("rm", resolved, recursive)) {
        outLine("rm: removed " + resolved, C_GREEN);
    }
}

//handles "del [-r] <path>" -- the DOS spelling of rm, kept because this is a toy shell and
//muscle memory goes both ways
void handleDelCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        outLine("Usage: del [-r] <path>");
        return;
    }

    bool recursive = false;
    int targetIndex = 1;
    if (parts[1] == "-r" || parts[1] == "-R") {
        recursive = true;
        targetIndex = 2;
    }
    if (targetIndex >= partCount) {
        outLine("Usage: del [-r] <path>");
        return;
    }

    String resolved = resolvePath(cwd, parts[targetIndex]);
    if (removeResolvedPath("del", resolved, recursive)) {
        outLine("del: removed " + resolved, C_GREEN);
    }
}

//handles "cp <source> <dest>". Never overwrites an existing file.
void handleCpCommand(const String parts[], int partCount) {
    if (partCount < 3) {
        outLine("Usage: cp <source> <dest>");
        return;
    }

    String sourceResolved = resolvePath(cwd, parts[1]);
    String destResolved = resolvePath(cwd, parts[2]);
    if (copyResolvedFile(sourceResolved, destResolved, false)) {
        outLine("cp: " + sourceResolved + " -> " + destResolved, C_GREEN);
    }
}

//handles "mv <source> <dest>". Within one filesystem this is a rename; across the LittleFS/SD
//seam there is no such primitive, so it degrades to copy-then-remove (files only).
void handleMvCommand(const String parts[], int partCount) {
    if (partCount < 3) {
        outLine("Usage: mv <source> <dest>");
        return;
    }

    String sourceResolved = resolvePath(cwd, parts[1]);
    if (pathIsProtectedRoot(sourceResolved)) {
        outLine("mv: refusing to move " + sourceResolved, C_RED);
        return;
    }

    RoutedPath source = routePath(sourceResolved);
    if (!ensureMountedForPath("mv", source)) {
        return;
    }

    File sourceFile = source.fs->open(source.realPath, "r");
    if (!sourceFile) {
        outLine("mv: " + sourceResolved + " not found", C_RED);
        return;
    }
    bool sourceIsDirectory = sourceFile.isDirectory();
    sourceFile.close();

    String destResolved = resolvePath(cwd, parts[2]);
    RoutedPath dest = routePath(destResolved);
    if (!ensureMountedForPath("mv", dest)) {
        return;
    }

    File destProbe = dest.fs->open(dest.realPath);
    if (destProbe && destProbe.isDirectory()) {
        destProbe.close();
        if (!destResolved.endsWith("/")) {
            destResolved += "/";
        }
        destResolved += baseNameOf(sourceResolved);
        dest = routePath(destResolved);
    } else if (destProbe) {
        destProbe.close();
    }

    if (pathIsProtectedRoot(destResolved)) {
        outLine("mv: invalid destination " + destResolved, C_RED);
        return;
    }

    String parent = parentPathOf(destResolved);
    if (!directoryExists(parent)) {
        outLine("mv: destination directory not found: " + parent, C_RED);
        return;
    }

    File existing = dest.fs->open(dest.realPath);
    if (existing) {
        existing.close();
        outLine("mv: " + destResolved + " already exists", C_RED);
        return;
    }

    if (source.fs == dest.fs) {
        if (!source.fs->rename(source.realPath, dest.realPath)) {
            outLine("mv: could not move " + sourceResolved, C_RED);
            return;
        }
        outLine("mv: " + sourceResolved + " -> " + destResolved, C_GREEN);
        return;
    }

    if (sourceIsDirectory) {
        outLine("mv: cross-filesystem directory moves are not supported", C_RED);
        return;
    }

    if (!copyResolvedFile(sourceResolved, destResolved, false)) {
        return;
    }
    if (!removeResolvedPath("mv", sourceResolved, false)) {
        outLine("mv: copied, but could not remove original " + sourceResolved, C_RED);
        return;
    }
    outLine("mv: " + sourceResolved + " -> " + destResolved, C_GREEN);
}

//   FtpServer.ino
//   exposes the mounted SD card to the network as an FTP server -- a second way to move
//   files on and off the card, alongside "usb" (usb_msc.ino) and pulling the card out.
//   Ported from DS, where it was the *only* way (that board's single USB-C port is a
//   serial bridge, so a mass-storage drive can never enumerate on a host). Here it's a
//   convenience: no cable, no unmounting, and it works while the shell keeps running.
//
//   In Windows Explorer's address bar type  ftp://<station-ip>/  (or use FileZilla/WinSCP).
//
//   Unlike "usb", this does NOT block: the server is serviced one non-blocking tick at a
//   time from loop() (ftpService below), so the shell, screen and keyboard all keep working
//   while a transfer is in flight -- large files just get chunked across many loop()
//   iterations. Toggle it with the "ftp" command; it stays off until asked for.
//
//   Storage backend + credentials: the library is a separately-compiled translation unit,
//   so the SD selection lives in its config header (libraries/SimpleFTPServer/FtpServerKey.h
//   -> DEFAULT_STORAGE_TYPE_ESP32 STORAGE_SD), not here -- which is exactly why this sketch
//   carries its own copy of the library. begin() does NOT re-mount the card; it uses the one
//   storage.ino already mounted. Creds come from config.h.
#include <Arduino.h>
#include <SimpleFTPServer.h>

//command port 21, passive data port 50009 -- the library defaults (FtpServer.h)
static FtpServer ftpSrv;
static bool ftpActive = false;

//serviced every loop() tick while active -- non-blocking, drives the FTP state
//machine one step (accept/auth/one buffer of transfer) and returns immediately
void ftpService() {
    if (ftpActive) {
        ftpSrv.handleFTP();
    }
}

static void ftpStart() {
    if (ftpActive) {
        outLine("ftp: already running", C_YELLOW);
        return;
    }
    if (!sdCardMounted) {
        outLine("ftp: SD card not mounted", C_RED);
        return;
    }
    //begin() only starts the listeners + allocates the transfer buffer; the SD card
    //stays mounted exactly as storage.ino left it
    ftpSrv.begin(FTP_USER, FTP_PASS);
    ftpActive = true;

    outLine("FTP server on", C_GREEN);
    if (wifiIsConnected() == 1) {
        outLine("  ftp://" + WiFi.localIP().toString() + "/");
    } else {
        outLine("  (WiFi not connected -- reachable once it joins)", C_YELLOW);
    }
    outLine("  user: " + String(FTP_USER) + "  pass: " + String(FTP_PASS));
    outLine("  serving the SD card -- 'ftp off' to stop");
}

static void ftpStop() {
    if (!ftpActive) {
        outLine("ftp: not running", C_YELLOW);
        return;
    }
    ftpSrv.end();
    ftpActive = false;
    outLine("FTP server off");
}

static void ftpStatus() {
    if (!ftpActive) {
        outLine("FTP: off  ('ftp on' to start)");
        return;
    }
    outLine("FTP: on", C_GREEN);
    if (wifiIsConnected() == 1) {
        outLine("  ftp://" + WiFi.localIP().toString() + "/");
    }
    outLine("  user: " + String(FTP_USER) + "  pass: " + String(FTP_PASS));
}

//handles the "ftp" command: "ftp on"/"start" begins, "ftp off"/"stop" ends, bare
//"ftp" reports status. Non-modal -- the prompt returns immediately either way.
void handleFtpCommand(const String parts[], int partCount) {
    String sub = (partCount > 1) ? parts[1] : "";
    sub.toLowerCase();

    if (sub == "on" || sub == "start") {
        ftpStart();
    } else if (sub == "off" || sub == "stop") {
        ftpStop();
    } else if (sub == "" || sub == "status") {
        ftpStatus();
    } else {
        outLine("Usage: ftp [on|off|status]");
    }
}

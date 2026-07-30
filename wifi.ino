//   wifi.ino
//   handles wifi scanning/connectivity for DollOS
//half and half. currently optimizing and changing so it works how i want. 
#include <LittleFS.h>

//path of the saved wifi credentials file on LittleFS
const char* WIFI_CREDS_PATH = "/wifi.cfg";

static bool wifiStaModeEnabled = false;

static void ensureWifiStaMode() {
    if (wifiStaModeEnabled) {
        return;
    }

    recordHeapCheckpoint("wifi pre-mode");
    WiFi.mode(WIFI_STA);

    //   Turn OFF the esp32 core's built-in auto-reconnect (it defaults to ON). Left on, a
    //   failed join -- e.g. the config.h default SSID isn't present -- makes the driver spin
    //   on association *forever* in the background, and a permanently-busy radio blocks
    //   everything else: esp_wifi_scan_start() and esp_wifi_set_config() both fail outright
    //   while the STA is mid-connect, so "wifi scan" fails instantly and "wifi connect" hits
    //   "cannot set config". We drive our own reconnection instead
    //   (maintainInternetConnection below, a bounded 10s tick), which leaves the radio idle
    //   between attempts so scans and manual connects can get in. Ported from DS, where this
    //   was diagnosed the hard way.
    WiFi.setAutoReconnect(false);

    //   Stop the core mirroring credentials into its own NVS copy. DOLL-OS keeps the
    //   authoritative credentials in /wifi.cfg (saveWifiCredentials below), so the NVS
    //   copy is both redundant and the one piece of state that could put the radio on
    //   a network without anyone asking -- a later bare WiFi.begin() reuses it. Also
    //   saves an NVS write on every connect.
    //
    //   This only stops *new* writes. If a previous build already stored an SSID there,
    //   clear it once with "wifi forget".
    WiFi.persistent(false);

    wifiStaModeEnabled = true;
    recordHeapCheckpoint("wifi post-mode");
}

//   Whether the user has asked to be on a network *this session*. Nothing sets this at
//   boot -- DOLL-OS never joins a network unbidden. It flips true only after a "wifi
//   connect" actually succeeds, and false again on "wifi disconnect".
//
//   maintainInternetConnection() below is gated on it, which is the whole point: without
//   the gate, removing the boot-time join would accomplish nothing, because the reconnect
//   tick would notice "not connected" and join anyway about ten seconds later.
static bool wifiUserWantsConnection = false;

//   Bounded reconnect tick, called every loop() pass. Re-establishes a connection the user
//   already made and that dropped afterwards -- an AP rebooting, walking out of range and
//   back. It does not initiate one. Retries at most every reconnectInterval, leaving the
//   radio idle in between (see the setAutoReconnect note above for why that matters).
unsigned long previousReconnectAttempt = 0;
const unsigned long reconnectInterval = 10000;

void maintainInternetConnection() {
    if (!wifiUserWantsConnection) {
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }
    if (millis() - previousReconnectAttempt < reconnectInterval) {
        return;
    }
    previousReconnectAttempt = millis();

    String ssid, password;
    if (!loadWifiCredentials(ssid, password)) {
        ssid = STA_DEFAULT_SSID;
        password = STA_DEFAULT_PASSWORD;
    }
    WiFi.begin(ssid.c_str(), password.c_str());
}

void scanWifiNetworks() {
    ensureWifiStaMode();    //station mode, needed before WiFi.scanNetworks() will work
    WiFi.scanDelete();      //clear wifi scan state
    recordHeapCheckpoint("wifi pre-scan");
    addWrappedHistoryLine("Scanning for Wifi Networks");
    drawTerminalHistory();  //draw before blocking wifi scan begins. 

    //perform the wifi scan
    int networkCount = WiFi.scanNetworks();
    // -1 is a failure
    if(networkCount < 0){
        addWrappedHistoryLine("Scan Failed");
        WiFi.scanDelete();
        return;
    }
    //if no networks found
    if(networkCount == 0){
        addWrappedHistoryLine("No Networks Found");
        WiFi.scanDelete();
        return;
    }
    //if networks found
    addWrappedHistoryLine(String(networkCount) + " Networks Found");

    for (int i = 0; i < networkCount; i++){
        String ssid = WiFi.SSID(i);
        //if length of ssid is zero
        if (ssid.length() == 0){
            ssid = "<hidden>";
        }
        //if length is more than 14 characters truncate
        if (ssid.length() > 14){
            ssid = ssid.substring(0,14) + "..."; //substring truncates string from char to char
        }
        String security;    //store wifi security type
        if(WiFi.encryptionType(i) == WIFI_AUTH_OPEN){   //if wifi auth open
            security = "OPEN";
        }   else{                                       //else secure
            security = "SECURE";
        }
        String result = String(i+1) + ". "              //build line and store as result
                        + ssid + " " 
                        + String(WiFi.RSSI(i)) + "dBm " 
                        + "ch" + String(WiFi.channel(i))
                        + security;
        addWrappedHistoryLine(result);     //add to history
                        
    }
    WiFi.scanDelete();
    recordHeapCheckpoint("wifi post-scan");
}

void showWifiStatus() {
    // Put the radio in station mode so normal client networking works.
    ensureWifiStaMode();
    // If we are not connected, print a short status message and stop.
    if (wifiIsConnected() == 0) {
        addWrappedHistoryLine("WiFi: not connected");
        return;
    }
    // If connected, print a few details that are useful in a terminal.
    wifiStatus();
}

// Connect to a Wi-Fi network using the provided SSID and password.
//
// This is a simple blocking version:
// it waits up to 15 seconds for the connection result.
// That is fine for a first pass in a small terminal OS.
void connectWifiNetwork(const String& ssid, const String& password) {
    // Put the radio in station mode before attempting a client connection.
    ensureWifiStaMode();

    // Tell the user what we are trying to do.
    addWrappedHistoryLine("Connecting to: " + ssid);
    drawTerminalHistory();

    // Start the connection attempt.
    // c_str() converts Arduino String into the C-style text WiFi.begin expects.
    recordHeapCheckpoint("wifi pre-begin");
    WiFi.begin(ssid.c_str(), password.c_str());
    recordHeapCheckpoint("wifi post-begin");

    // Wait up to 15 seconds for a result.
    const unsigned long timeoutMs = 15000;
    unsigned long startTime = millis();

    // Keep waiting while:
    // - we are not connected yet
    // - and we have not timed out
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeoutMs) {
        // Small delay so we do not spin too aggressively.
        delay(250);
    }

    // Check whether the connection actually succeeded.
    if (wifiIsConnected() == 1) {
        recordHeapCheckpoint("wifi connected");
        //   Arm the reconnect tick, but only now. Setting it on the *attempt* would mean a
        //   typo'd SSID left the radio retrying a network that doesn't exist every 10s for
        //   the rest of the session; setting it on success means the tick only ever
        //   re-establishes something that demonstrably worked once.
        wifiUserWantsConnection = true;
        wifiStatus();
    } else {
        recordHeapCheckpoint("wifi connect fail");
        addWrappedHistoryLine("WiFi connect failed");
    }
}

//   "wifi disconnect" -- drops the link and disarms the reconnect tick, so it stays down.
void disconnectWifiNetwork() {
    wifiUserWantsConnection = false;
    WiFi.disconnect(false);   //radio stays initialized so "wifi scan" still works
    addWrappedHistoryLine("WiFi disconnected");
}

//   "wifi forget" -- clears the ESP's own NVS copy of the last SSID/password. Nothing in
//   DOLL-OS reads that copy (credentials live in /wifi.cfg), but a build from before
//   WiFi.persistent(false) may have left one behind, and it is the only state that could
//   still put the radio on a network without being asked. Leaves /wifi.cfg alone.
void forgetWifiNvs() {
    wifiUserWantsConnection = false;
    ensureWifiStaMode();
    WiFi.disconnect(false, true);   //eraseap = true
    addWrappedHistoryLine("Cleared the radio's stored network");
    addWrappedHistoryLine("(/wifi.cfg is untouched)");
}

// Save WiFi credentials to LittleFS so they can be reused after reboot.
// Stored as two lines: ssid, then password.
bool saveWifiCredentials(const String& ssid, const String& password) {
    File file = LittleFS.open(WIFI_CREDS_PATH, "w");
    if (!file) {
        return false;
    }
    file.println(ssid);
    file.println(password);
    file.close();
    return true;
}

// Load previously saved WiFi credentials from LittleFS.
// Returns true and fills ssid/password if a saved file was found.
bool loadWifiCredentials(String& ssid, String& password) {
    File file = LittleFS.open(WIFI_CREDS_PATH, "r");
    if (!file) {
        return false;
    }
    ssid = file.readStringUntil('\n');
    password = file.readStringUntil('\n');
    file.close();

    ssid.trim();
    password.trim();
    return ssid.length() > 0;
}

// Handle everything that starts with the "wifi" command.
//
// Expected forms:
// wifi
// wifi scan
// wifi connect <ssid> <password>
void handleWifiCommand(const String parts[], int partCount) {
    // If the user typed only "wifi", show current status.
    if (partCount == 1) {
        showWifiStatus();
        return;
    }

    // If the user typed "wifi scan", run your existing scan function.
    if (parts[1] == "scan") {
        scanWifiNetworks();
        return;
    }

    if (parts[1] == "disconnect") {
        disconnectWifiNetwork();
        return;
    }

    if (parts[1] == "forget") {
        forgetWifiNvs();
        return;
    }

    // If the user typed "wifi connect ap password", try to connect.
    if (parts[1] == "connect") {
        // With no ssid/password given, fall back to saved credentials.
        if (partCount < 4) {
            String savedSsid, savedPassword;
            if (loadWifiCredentials(savedSsid, savedPassword)) {
                connectWifiNetwork(savedSsid, savedPassword);
            } else {
                addWrappedHistoryLine("Usage: wifi connect <ssid> <password>");
            }
            return;
        }

        connectWifiNetwork(parts[2], parts[3]);
        return;
    }

    // If the user typed "wifi save ssid password", store credentials on LittleFS.
    if (parts[1] == "save") {
        // With no ssid/password given, save the network we are currently connected to.
        if (partCount < 4) {
            if (wifiIsConnected() != 1) {
                addWrappedHistoryLine("Usage: wifi save <ssid> <password>");
                return;
            }
            if (saveWifiCredentials(WiFi.SSID(), WiFi.psk())) {
                addWrappedHistoryLine("Saved WiFi credentials");
            } else {
                addWrappedHistoryLine("Failed to save WiFi credentials");
            }
            return;
        }

        if (saveWifiCredentials(parts[2], parts[3])) {
            addWrappedHistoryLine("Saved WiFi credentials");
        } else {
            addWrappedHistoryLine("Failed to save WiFi credentials");
        }
        return;
    }

    // If the subcommand is unknown, print a small help message.
    wifiHelp();
}
void wifiHelp(){
    addWrappedHistoryLine("WiFi subcommands:");
    addWrappedHistoryLine("wifi");
    addWrappedHistoryLine("wifi scan");
    addWrappedHistoryLine("wifi connect <ssid> <password>");
    addWrappedHistoryLine("wifi connect  (uses saved)");
    addWrappedHistoryLine("wifi disconnect");
    addWrappedHistoryLine("wifi save <ssid> <password>");
    addWrappedHistoryLine("wifi forget  (clears radio NVS)");
    addWrappedHistoryLine("Never connects on its own -- ask it to.");
    return;
}
void wifiStatus(){
    if (wifiIsConnected() == 1) {
        addWrappedHistoryLine("WiFi connected");
        addWrappedHistoryLine("SSID: " + WiFi.SSID());
        addWrappedHistoryLine("IP: " + WiFi.localIP().toString());
        addWrappedHistoryLine("Gateway: " + WiFi.gatewayIP().toString());
        addWrappedHistoryLine("Subnet: " + WiFi.subnetMask().toString());
    } else {
        addWrappedHistoryLine("WiFi Not Connected");
    }
}
int wifiIsConnected(){
    if (WiFi.status() == WL_CONNECTED) {
        return 1;
    } else{
        return 0;
    }
}
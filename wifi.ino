//   wifi.ino
//   handles wifi scanning/connectivity for DollOS


//TODO: actually scan and report nearby networks, currently just flips radio into station mode
void scanWifiNetworks() {
    WiFi.mode(WIFI_STA);    //station mode, needed before WiFi.scanNetworks() will work
    WiFi.scanDelete();      //clear wifi scan state
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
}

void showWifiStatus() {
    // Put the radio in station mode so normal client networking works.
    WiFi.mode(WIFI_STA);

    // Ask the WiFi library for the current connection state.
    wl_status_t status = WiFi.status();

    // If we are not connected, print a short status message and stop.
    if (status != WL_CONNECTED) {
        addWrappedHistoryLine("WiFi: not connected");
        return;
    }

    // If connected, print a few details that are useful in a terminal.
    addWrappedHistoryLine("WiFi: connected");
    addWrappedHistoryLine("SSID: " + WiFi.SSID());
    addWrappedHistoryLine("IP: " + WiFi.localIP().toString());
    addWrappedHistoryLine("RSSI: " + String(WiFi.RSSI()) + " dBm");
}

// Connect to a Wi-Fi network using the provided SSID and password.
//
// This is a simple blocking version:
// it waits up to 15 seconds for the connection result.
// That is fine for a first pass in a small terminal OS.
void connectWifiNetwork(const String& ssid, const String& password) {
    // Put the radio in station mode before attempting a client connection.
    WiFi.mode(WIFI_STA);

    // Tell the user what we are trying to do.
    addWrappedHistoryLine("Connecting to: " + ssid);
    drawTerminalHistory();

    // Start the connection attempt.
    // c_str() converts Arduino String into the C-style text WiFi.begin expects.
    WiFi.begin(ssid.c_str(), password.c_str());

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
    if (WiFi.status() == WL_CONNECTED) {
        addWrappedHistoryLine("WiFi connected");
        addWrappedHistoryLine("SSID: " + WiFi.SSID());
        addWrappedHistoryLine("IP: " + WiFi.localIP().toString());
    } else {
        addWrappedHistoryLine("WiFi connect failed");
    }
}

// Handle everything that starts with the "wifi" command.
//
// Expected forms:
// wifi
// wifi -scan
// wifi -connect <ssid> <password>
void handleWifiCommand(const String parts[], int partCount) {
    // If the user typed only "wifi", show current status.
    if (partCount == 1) {
        showWifiStatus();
        return;
    }

    // If the user typed "wifi -scan", run your existing scan function.
    if (parts[1] == "scan") {
        scanWifiNetworks();
        return;
    }

    // If the user typed "wifi -connect ap password", try to connect.
    if (parts[1] == "connect") {
        // We need at least 4 tokens:
        // parts[0] = wifi
        // parts[1] = -connect
        // parts[2] = ssid
        // parts[3] = password
        if (partCount < 4) {
            addWrappedHistoryLine("Usage: wifi -connect <ssid> <password>");
            return;
        }

        connectWifiNetwork(parts[2], parts[3]);
        return;
    }

    // If the subcommand is unknown, print a small help message.
    addWrappedHistoryLine("WiFi subcommands:");
    addWrappedHistoryLine("wifi");
    addWrappedHistoryLine("wifi -scan");
    addWrappedHistoryLine("wifi -connect <ssid> <password>");
}
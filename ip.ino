//   ip.ino
//   handles the "ip" command: local IP info, a ping-based subnet scan, and
//   an ARP scan via the esp32ARP library (https://github.com/liquidCS/esp32ARP)
//vibe coded stub
#include <ESP32Ping.h>
#include <esp32ARP.h>

static esp32ARP arp;
static bool arpInitialized = false;

//formats a 6-byte MAC address as "AA:BB:CC:DD:EE:FF"
//takes a raw byte pointer rather than mac_addr_t: Arduino hoists this function's
//prototype to the top of the combined sketch, before ip.ino's own #include
//<esp32ARP.h> runs, so a mac_addr_t parameter would reference an as-yet-undefined type
String formatMacAddr(const uint8_t* addr) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return String(buf);
}

//computes the /24-style network and broadcast octets for the current local IP + subnet mask.
//only the last octet is varied when scanning, so this only covers the full subnet when the
//mask leaves the first three octets fully set (e.g. 255.255.255.0 and smaller host ranges).
void ipComputeRange(byte net[4], byte bcast[4]) {
    IPAddress local = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    for (int i = 0; i < 4; i++) {
        net[i] = local[i] & mask[i];
        bcast[i] = net[i] | (~mask[i] & 0xFF);
    }
}

//"ip" with no subcommand: show current network info
void ipShowInfo() {
    if (WiFi.status() != WL_CONNECTED) {
        addWrappedHistoryLine("ip: WiFi not connected. Run 'wifi connect' first.", RED);
        return;
    }
    addWrappedHistoryLine("IP: " + WiFi.localIP().toString());
    addWrappedHistoryLine("Gateway: " + WiFi.gatewayIP().toString());
    addWrappedHistoryLine("Subnet: " + WiFi.subnetMask().toString());
    addWrappedHistoryLine("MAC: " + WiFi.macAddress());
    addWrappedHistoryLine("DNS: " + WiFi.dnsIP().toString());
}

//"ip scan": ping sweep across the local /24-ish subnet
void ipScanNetwork() {
    if (WiFi.status() != WL_CONNECTED) {
        addWrappedHistoryLine("ip: WiFi not connected. Run 'wifi connect' first.", RED);
        return;
    }

    byte net[4], bcast[4];
    ipComputeRange(net, bcast);

    addWrappedHistoryLine("Scanning " + String(net[0]) + "." + String(net[1]) + "." + String(net[2]) + ".0/24 (this may take a while)");
    drawTerminalHistory();

    int found = 0;
    String pending = "";
    for (int h = net[3] + 1; h < bcast[3]; h++) {
        IPAddress target(net[0], net[1], net[2], h);
        if (Ping.ping(target, 1)) {
            found++;
            if (pending.length() == 0) {
                pending = target.toString();
            } else {
                addWrappedHistoryLine(pending + "  " + target.toString(), GREEN);
                pending = "";
                drawTerminalHistory();
            }
        }
    }
    if (pending.length() > 0) {
        addWrappedHistoryLine(pending, GREEN);
    }

    addWrappedHistoryLine(String(found) + " host(s) responded");
}

//"ip arp": ARP scan of the local subnet via the esp32ARP library
void ipArpScan() {
    if (WiFi.status() != WL_CONNECTED) {
        addWrappedHistoryLine("ip: WiFi not connected. Run 'wifi connect' first.", RED);
        return;
    }

    if (!arpInitialized) {
        arp.init();
        arpInitialized = true;
    }

    byte net[4], bcast[4];
    ipComputeRange(net, bcast);

    addWrappedHistoryLine("ARP scanning " + String(net[0]) + "." + String(net[1]) + "." + String(net[2]) + ".0/24");
    drawTerminalHistory();

    //broadcast a request to every host in range, then give the network a moment to reply
    for (int h = net[3] + 1; h < bcast[3]; h++) {
        IPAddress target(net[0], net[1], net[2], h);
        arp.sendRequest(target);
        delay(5);
    }
    delay(1000);

    int found = 0;
    for (int h = net[3] + 1; h < bcast[3]; h++) {
        IPAddress target(net[0], net[1], net[2], h);
        mac_addr_t mac;
        if (arp.lookupEntry(target, mac) >= 0) {
            addWrappedHistoryLine(target.toString() + " -> " + formatMacAddr(mac.addr), CYAN);
            found++;
            drawTerminalHistory();
        }
    }

    addWrappedHistoryLine(String(found) + " host(s) found");
}

//handles the "ip" command
//
//Expected forms:
//ip
//ip scan
//ip arp
void handleIpCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        ipShowInfo();
        return;
    }

    if (parts[1] == "scan") {
        ipScanNetwork();
        return;
    }

    if (parts[1] == "arp") {
        ipArpScan();
        return;
    }

    addWrappedHistoryLine("ip subcommands:");
    addWrappedHistoryLine("ip");
    addWrappedHistoryLine("ip scan");
    addWrappedHistoryLine("ip arp");
}

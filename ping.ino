//   ping.ino
//   handles the "ping" command, sends ICMP echo requests over the current WiFi connection
// vibe coded stub
#include <ESP32Ping.h>

//handles the "ping" command
//
//Expected forms:
//ping <address>
//ping <address> <count>
void handlePingCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        addWrappedHistoryLine("Usage: ping <address> <count>");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        addWrappedHistoryLine("ping: WiFi not connected. Run 'wifi connect' first.", RED);
        return;
    }

    String address = parts[1];

    int count = 4;   //default to 4 pings
    if (partCount > 2) {
        count = parts[2].toInt();
        if (count < 1) {
            count = 4;
        } else if (count > 255) {   //Ping.ping() takes a byte, cap so toInt() garbage can't wrap around
            count = 255;
        }
    }

    addWrappedHistoryLine("Pinging " + address + " x" + String(count));
    drawTerminalHistory();   //draw before the blocking ping begins

    if (Ping.ping(address.c_str(), (byte)count)) {
        addWrappedHistoryLine("Reply from " + address + ", avg " + String(Ping.averageTime(), 1) + "ms");
    } else {
        addWrappedHistoryLine("Ping to " + address + " failed");
    }
}

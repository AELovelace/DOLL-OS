//   motoko.ino
//   MQTT chat client, launched by the "motoko" command. Takes over the screen
//   and keyboard until "/quit" is typed, then hands control back to the terminal.
//   Reuses DOLL-OS's existing terminal history (addWrappedHistoryLine /
//   drawTerminalHistory / scrollHistory), the shared readKeyboard() input
//   handler, and commandBarSprite for rendering, rather than keeping its own
//   separate log buffer, sprite, or keyboard polling loop.

#include <PubSubClient.h>

WiFiClient motokoWifiClient;
PubSubClient motokoMqttClient(motokoWifiClient);



enum MotokoInputMode { MOTOKO_ASK_CHANNEL, MOTOKO_ASK_MESSAGE };
MotokoInputMode motokoInputMode = MOTOKO_ASK_CHANNEL;

String motokoChannel = "";
String motokoInputBuffer = "";

unsigned long motokoLastReconnectAttempt = 0;
const unsigned long MOTOKO_RECONNECT_INTERVAL_MS = 2000;   //non-blocking retry cadence, unlike the original's delay(1000)

//draws the "channel> "/"msg> " prompt via the existing command bar
void motokoDrawInputRow() {
    const char* prompt = (motokoInputMode == MOTOKO_ASK_CHANNEL) ? "channel> " : "msg> ";
    drawCommandBar(String(prompt) + motokoInputBuffer);
}

//PubSubClient callback: logs incoming messages into the shared terminal history,
//"answer" gets a bracketed prefix and a highlight color to make the reserved reply topic stand out
void motokoMqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    if (strcmp(topic, "answer") == 0) {
        addWrappedHistoryLine("[answer] " + msg, PINK);
    } else {
        addWrappedHistoryLine("< " + msg, YELLOW);
    }
}

//handles a finished line of input. Anything starting with "/" is treated as a command
//attempt -- never as a channel name or a published message -- so a mistyped slash
//command doesn't accidentally get sent to the channel/LLM.
void motokoHandleEnteredLine() {
    if (motokoInputBuffer.length() == 0) {
        return;
    }

    if (motokoInputBuffer.startsWith("/")) {
        if (motokoInputBuffer == "/battery") {
            int32_t level = M5Cardputer.Power.getBatteryLevel();
            if (level < 0) {
                addWrappedHistoryLine("Battery: unknown", YELLOW);
            } else {
                addWrappedHistoryLine("Battery: " + String(level) + "%", CYAN);
            }
        } else if (motokoInputBuffer == "/channel") {
            motokoInputMode = MOTOKO_ASK_CHANNEL;
        } else {
            addWrappedHistoryLine("Unsupported command: " + motokoInputBuffer, RED);
        }
        return;
    }

    if (motokoInputMode == MOTOKO_ASK_CHANNEL) {
        if (motokoChannel.length() > 0) {
            motokoMqttClient.unsubscribe(motokoChannel.c_str());
        }
        motokoChannel = motokoInputBuffer;
        motokoMqttClient.subscribe(motokoChannel.c_str());
        addWrappedHistoryLine("Channel: " + motokoChannel, CYAN);
        motokoInputMode = MOTOKO_ASK_MESSAGE;
    } else {
        bool ok = motokoMqttClient.publish(motokoChannel.c_str(), motokoInputBuffer.c_str());
        addWrappedHistoryLine((ok ? "> " : "FAILED: ") + motokoInputBuffer, ok ? GREEN : RED);
    }
}

//the modal loop: owns the keyboard and screen until "/quit" is entered
void runMotokoBlocking() {
    while (true) {
        M5Cardputer.update();
        delay(10);

        motokoMqttClient.loop();
        if (!motokoMqttClient.connected() && millis() - motokoLastReconnectAttempt > MOTOKO_RECONNECT_INTERVAL_MS) {
            motokoLastReconnectAttempt = millis();
            if (motokoMqttClient.connect(MOTOKO_CLIENT_ID)) {
                addWrappedHistoryLine("MQTT connected!", GREEN);
                motokoMqttClient.subscribe("answer");
                if (motokoChannel.length() > 0) {
                    motokoMqttClient.subscribe(motokoChannel.c_str());
                }
            }
        }

        bool enterPressed = readKeyboard(motokoInputBuffer);   //shared input handler; also services Fn+;/Fn+. scrolling

        if (enterPressed) {
            if (motokoInputBuffer == "/quit") {
                break;
            }
            motokoHandleEnteredLine();
            motokoInputBuffer = "";
        }

        drawTerminalHistory();
        motokoDrawInputRow();
    }

    motokoMqttClient.disconnect();
}

//handles the "motoko" command: motoko [broker-ip] [port]
void handleMotokoCommand(const String parts[], int partCount) {
    if (WiFi.status() != WL_CONNECTED) {
        addWrappedHistoryLine("motoko: WiFi not connected. Run 'wifi connect' first.", RED);
        return;
    }

    String broker = (partCount > 1) ? parts[1] : String(MOTOKO_DEFAULT_BROKER);
    int port = (partCount > 2) ? parts[2].toInt() : MOTOKO_DEFAULT_PORT;
    if (port <= 0) {
        port = MOTOKO_DEFAULT_PORT;
    }

    //reset session state so a relaunch starts clean
    motokoInputBuffer = "";
    motokoChannel = "";
    motokoInputMode = MOTOKO_ASK_CHANNEL;
    motokoLastReconnectAttempt = 0;

    motokoMqttClient.setServer(broker.c_str(), port);
    motokoMqttClient.setCallback(motokoMqttCallback);
    motokoMqttClient.setBufferSize(1024);

    addWrappedHistoryLine("MOTOKO " + broker + ":" + String(port), PINK);
    drawTerminalHistory();
    motokoDrawInputRow();

    runMotokoBlocking();

    drawTerminalHistory();
    drawCommandBar(currentCommand);
    addWrappedHistoryLine("motoko: exited");
}

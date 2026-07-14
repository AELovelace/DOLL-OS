//   telnet.ino
//   plain TCP telnet client for the "telnet" command. No libssh, no encryption --
//   just a raw socket wired into the same modal keyboard/terminal pattern ssh.ino
//   and motoko.ino use, plus just enough IAC option negotiation (RFC 854/855) that
//   well-behaved servers stop spamming control bytes into the terminal history.

static const int TELNET_DEFAULT_PORT = 23;
static const long TELNET_CONNECT_TIMEOUT_SEC = 8;   //bounds the blocking client.connect() call

//telnet protocol bytes (RFC 854)
static const uint8_t TELNET_IAC  = 255;
static const uint8_t TELNET_DONT = 254;
static const uint8_t TELNET_DO   = 253;
static const uint8_t TELNET_WONT = 252;
static const uint8_t TELNET_WILL = 251;
static const uint8_t TELNET_SB   = 250;
static const uint8_t TELNET_SE   = 240;

//walks incoming bytes: plain text, an IAC command, an IAC+option negotiation,
//or an IAC SB ... IAC SE subnegotiation block to be skipped wholesale
enum TelnetParseState { TELNET_NORMAL, TELNET_GOT_IAC, TELNET_GOT_CMD, TELNET_GOT_SB, TELNET_GOT_SB_IAC };

WiFiClient telnetClient;
String telnetInputBuffer = "";
String telnetLineRemainder = "";       //partial (no trailing \n yet) line carried between polls
bool telnetPartialRowActive = false;   //true while telnetLineRemainder is already live-drawn as history's last row
TelnetParseState telnetParseState = TELNET_NORMAL;
uint8_t telnetPendingCmd = 0;

//ANSI escape/UTF-8 filter state and current SGR-derived color -- lets a remote
//shell's color prompts/output come through as row colors instead of leaking raw
//escape bytes into the visible text
AnsiFilterState telnetAnsi;
uint16_t telnetColor = WHITE;

//draws the "> " prompt through the existing command bar
void telnetDrawInputRow() {
    drawCommandBar("> " + telnetInputBuffer);
}

//refuses every option a server tries to negotiate -- DO gets WONT, WILL gets
//DONT -- which keeps the session in plain character-stream mode without
//needing to actually implement any option (echo, terminal type, etc.)
void telnetReplyNegotiation(uint8_t cmd, uint8_t option) {
    uint8_t reply = (cmd == TELNET_DO) ? TELNET_WONT : TELNET_DONT;
    uint8_t buf[3] = { TELNET_IAC, reply, option };
    telnetClient.write(buf, 3);
}

//feeds one incoming byte through the IAC state machine; plain text bytes are
//appended to the pending line, which gets flushed into terminal history on '\n'
void telnetProcessByte(uint8_t ch) {
    switch (telnetParseState) {
        case TELNET_NORMAL:
            if (ch == TELNET_IAC) {
                telnetParseState = TELNET_GOT_IAC;
            } else if (ch == '\r') {
                //ignore; '\n' below is what actually ends a line
            } else if (ch == '\n') {
                if (telnetPartialRowActive) {
                    removeLastHistoryRow();   //drop the live preview row so the finished line can be word-wrapped properly
                    telnetPartialRowActive = false;
                }
                addWrappedHistoryLine(telnetLineRemainder, telnetColor);
                telnetLineRemainder = "";
            } else {
                char outCh;
                bool colorChanged;
                if (!ansiFilterByte(telnetAnsi, ch, WHITE, telnetColor, outCh, colorChanged)) {
                    break;
                }
                telnetLineRemainder += outCh;
                //show the line as it streams in rather than waiting for '\n' -- otherwise
                //prompts like "login:" that never end in a newline stay invisible until
                //more text arrives
                if (telnetPartialRowActive) {
                    updateLastHistoryRow(telnetLineRemainder, telnetColor);
                } else {
                    addHistoryRow(telnetLineRemainder, telnetColor);
                    telnetPartialRowActive = true;
                }
            }
            break;

        case TELNET_GOT_IAC:
            if (ch == TELNET_IAC) {                 //escaped 0xFF byte in the data stream
                telnetLineRemainder += (char)0xFF;
                telnetParseState = TELNET_NORMAL;
            } else if (ch == TELNET_SB) {
                telnetParseState = TELNET_GOT_SB;
            } else if (ch == TELNET_WILL || ch == TELNET_WONT || ch == TELNET_DO || ch == TELNET_DONT) {
                telnetPendingCmd = ch;
                telnetParseState = TELNET_GOT_CMD;
            } else {
                telnetParseState = TELNET_NORMAL;   //other IAC commands (NOP, AYT, GA, ...) need no reply
            }
            break;

        case TELNET_GOT_CMD:
            if (telnetPendingCmd == TELNET_DO || telnetPendingCmd == TELNET_WILL) {
                telnetReplyNegotiation(telnetPendingCmd, ch);
            }
            telnetParseState = TELNET_NORMAL;
            break;

        case TELNET_GOT_SB:
            if (ch == TELNET_IAC) {
                telnetParseState = TELNET_GOT_SB_IAC;
            }
            break;

        case TELNET_GOT_SB_IAC:
            telnetParseState = (ch == TELNET_SE) ? TELNET_NORMAL : TELNET_GOT_SB;
            break;
    }
}

//drains whatever's already buffered on the socket without blocking
void telnetPumpStream() {
    while (telnetClient.available() > 0) {
        telnetProcessByte((uint8_t)telnetClient.read());
    }
}

//the modal loop: owns the keyboard and screen for the life of the telnet session.
//"/quit" disconnects locally; the remote end closing the socket also exits.
void runTelnetBlocking() {
    telnetInputBuffer = "";
    drawTerminalHistory();
    telnetDrawInputRow();

    while (true) {
        M5Cardputer.update();
        delay(10);

        telnetPumpStream();
        if (!telnetClient.connected()) {
            addWrappedHistoryLine("telnet: remote closed the connection", YELLOW);
            break;
        }

        bool enterPressed = readKeyboard(telnetInputBuffer);   //shared input handler; also services Fn+;/Fn+. scrolling and ctrl+;/ctrl+. recall

        if (enterPressed) {
            if (telnetInputBuffer == "/quit") {
                break;
            }
            String line = telnetInputBuffer + "\r\n";
            telnetClient.write((const uint8_t*)line.c_str(), line.length());
            telnetInputBuffer = "";
        }

        drawTerminalHistory();
        telnetDrawInputRow();
    }

    //final drain so output already in flight isn't lost when the loop exits
    telnetPumpStream();
    if (telnetLineRemainder.length() > 0) {
        if (telnetPartialRowActive) {
            removeLastHistoryRow();   //already showing as a live preview row; drop it so the wrapped version isn't duplicated
            telnetPartialRowActive = false;
        }
        addWrappedHistoryLine(telnetLineRemainder, telnetColor);
        telnetLineRemainder = "";
    }
}

//handles the "telnet" command
//
//Expected forms:
//telnet host
//telnet host port
void handleTelnetCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        addWrappedHistoryLine("Usage: telnet host [port]");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        addWrappedHistoryLine("telnet: WiFi not connected. Run 'wifi connect' first.", RED);
        return;
    }

    String host = parts[1];
    int port = TELNET_DEFAULT_PORT;
    if (partCount > 2) {
        int parsedPort = parts[2].toInt();
        if (parsedPort > 0) {
            port = parsedPort;
        }
    }

    addWrappedHistoryLine("Connecting to " + host + ":" + String(port), PINK);
    drawTerminalHistory();

    telnetClient.setTimeout(TELNET_CONNECT_TIMEOUT_SEC);   //seconds; bounds the blocking connect below
    if (!telnetClient.connect(host.c_str(), port)) {
        addWrappedHistoryLine("telnet: connect failed", RED);
        return;
    }

    addWrappedHistoryLine("telnet: connected", GREEN);
    telnetParseState = TELNET_NORMAL;
    telnetLineRemainder = "";
    telnetPartialRowActive = false;
    telnetAnsi = AnsiFilterState();
    telnetColor = WHITE;

    runTelnetBlocking();

    telnetClient.stop();

    drawTerminalHistory();
    drawCommandBar(currentCommand);
    addWrappedHistoryLine("telnet: session ended");
}

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

//telnet options (RFC 857/858) we actually react to during negotiation
static const uint8_t TELNET_OPT_ECHO = 1;
static const uint8_t TELNET_OPT_SGA  = 3;   //suppress go-ahead

//walks incoming bytes: plain text, an IAC command, an IAC+option negotiation,
//or an IAC SB ... IAC SE subnegotiation block to be skipped wholesale
enum TelnetParseState { TELNET_NORMAL, TELNET_GOT_IAC, TELNET_GOT_CMD, TELNET_GOT_SB, TELNET_GOT_SB_IAC };

WiFiClient telnetClient;
TelnetParseState telnetParseState = TELNET_NORMAL;
uint8_t telnetPendingCmd = 0;

//ANSI escape/UTF-8 filter state and current SGR-derived color -- lets a remote
//shell's color prompts/output come through as row colors instead of leaking raw
//escape bytes into the visible text
AnsiFilterState telnetAnsi;
uint16_t telnetColor = WHITE;

//streaming-terminal state -- lets telnet pump bytes straight into the terminal live
//(see terminal.ino's terminalStreamPutChar)
TerminalStreamState telnetStream;

//draws a static prompt/hint through the existing command bar -- there's no local input buffer to
//show during a raw session, since every keystroke is forwarded immediately and whatever comes back
//over the wire (the remote's own echo) is what actually renders as typed text in terminal history
void telnetDrawInputRow() {
    drawCommandBar("> ", "Fn+Q: quit");
}

//refuses everything a server asks us (DO) to do, since we don't implement any option locally --
//but accepts ECHO and SUPPRESS-GO-AHEAD when a server announces it will do them (WILL) itself.
//That's what puts well-behaved servers into character-at-a-time mode instead of the line-buffered
//NVT default; telehack.com's live "relay" chat in particular depends on this to work at all.
void telnetReplyNegotiation(uint8_t cmd, uint8_t option) {
    uint8_t reply;
    if (cmd == TELNET_DO) {
        reply = TELNET_WONT;
    } else {
        reply = (option == TELNET_OPT_ECHO || option == TELNET_OPT_SGA) ? TELNET_DO : TELNET_DONT;
    }
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
                //a bare CR (not followed by LF) is the remote rewinding to the start of the
                //current line to redraw it in place -- e.g. telehack's relay overwriting a user's
                //prompt when another user's message arrives. '\n' below is what actually ends a line
                terminalStreamCarriageReturn(telnetStream);
            } else if (ch == '\n') {
                terminalStreamNewline(telnetStream);
            } else {
                char outCh;
                bool colorChanged;
                bool isBackspace;
                bool eraseToEndOfLine;
                //show the line as it streams in rather than waiting for '\n' -- otherwise
                //prompts like "login:" that never end in a newline stay invisible until
                //more text arrives
                if (ansiFilterByte(telnetAnsi, ch, WHITE, telnetColor, outCh, colorChanged, isBackspace, eraseToEndOfLine)) {
                    terminalStreamPutChar(telnetStream, outCh, telnetColor);
                } else if (isBackspace) {
                    terminalStreamBackspace(telnetStream);
                } else if (eraseToEndOfLine) {
                    terminalStreamEraseToEnd(telnetStream);
                }
            }
            break;

        case TELNET_GOT_IAC:
            if (ch == TELNET_IAC) {                 //escaped 0xFF byte in the data stream
                terminalStreamPutChar(telnetStream, (char)0xFF, telnetColor);
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

//forwards raw keystrokes into the socket and streams the remote's bytes back into terminal
//history via the IAC parser above. Fn+Q disconnects locally; the remote end closing the socket
//also exits. See RemoteSession in global.h for why the loop itself lives there instead of here.
class TelnetSession : public RemoteSession {
protected:
    void pumpIncoming() override {
        telnetPumpStream();
    }

    bool isClosed() override {
        return !telnetClient.connected();
    }

    void sendBytes(const String& bytes) override {
        //RFC 854 NVT requires CR be followed by LF (or NUL); a bare Enter keystroke only produces
        //"\r", so expand it here rather than teaching the generic key translator this transport's quirk
        if (bytes == "\r") {
            telnetClient.write((const uint8_t*)"\r\n", 2);
        } else {
            telnetClient.write((const uint8_t*)bytes.c_str(), bytes.length());
        }
    }

    void drawInputRow() override {
        telnetDrawInputRow();
    }

    void onClosed() override {
        addWrappedHistoryLine("telnet: remote closed the connection", YELLOW);
    }

    //classic telnet/BBS servers (e.g. telehack.com) implement their own line editor against the
    //original ASCII backspace (0x08), not the DEL (0x7F) a real unix pty's tty driver expects --
    //without this override the server may never recognize the erase request at all
    String backspaceBytes() override {
        return "\x08";
    }
};

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
    terminalStreamReset(telnetStream);
    telnetAnsi = AnsiFilterState();
    telnetColor = WHITE;

    TelnetSession session;
    session.run();
    terminalStreamReset(telnetStream);   //defensive: no stale ownership survives past this session

    telnetClient.stop();

    drawTerminalHistory();
    drawCommandBar(currentCommand);
    addWrappedHistoryLine("telnet: session ended");
}

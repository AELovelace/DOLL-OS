//   ssh.ino
//   SSH client for the "ssh" command, for doing sysadmin work on remote hosts
//   over WiFi. Almost all of the protocol/crypto work is handled by libssh_esp32
//   (https://github.com/ewpa/libssh-esp32), a libssh port built against ESP-IDF's
//   existing mbedtls, so this file only wires it into DOLL-OS's terminal history /
//   keyboard input path -- the same modal pattern motoko.ino uses for PubSubClient.

#include "libssh_esp32.h"   //Arduino/ESP32 glue; must precede libssh.h per the library's own examples
#include <libssh/libssh.h>

static const int SSH_DEFAULT_PORT = 22;
static const long SSH_CONNECT_TIMEOUT_SEC = 8;   //bounds the blocking ssh_connect() call
static const int SSH_PTY_COLS = 53;              //rough char width/height of the terminal sprite at text size 1
static const int SSH_PTY_ROWS = 18;

enum SshUiState { SSH_UI_PASSWORD, SSH_UI_SHELL };
SshUiState sshUiState = SSH_UI_PASSWORD;
String sshInputBuffer = "";
String sshStdoutRemainder = "";   //partial (no trailing \n yet) line carried between polls, per stream
String sshStderrRemainder = "";

//ANSI escape/UTF-8 filter state and current SGR-derived color, per stream -- lets a
//remote shell's color prompts/output (e.g. colored ls, a colored PS1) come through
//as row colors instead of leaking raw escape bytes into the visible text
AnsiFilterState sshStdoutAnsi;
AnsiFilterState sshStderrAnsi;
uint16_t sshStdoutColor = WHITE;
uint16_t sshStderrColor = RED;

static bool sshLibInitialized = false;

//draws the password/shell prompt through the existing command bar. Password
//characters are masked with '*' since they're rendered live as they're typed
void sshDrawInputRow() {
    if (sshUiState == SSH_UI_PASSWORD) {
        String masked;
        for (size_t i = 0; i < sshInputBuffer.length(); i++) {
            masked += '*';
        }
        drawCommandBar("password> " + masked);
    } else {
        drawCommandBar("$ " + sshInputBuffer);
    }
}

//drains whatever's already buffered on one ssh stream (stdout or stderr) without
//blocking, flushing complete lines into the shared terminal history as they arrive.
//
//takes a void* rather than ssh_channel: Arduino hoists this function's prototype to
//the top of the combined sketch, before ssh.ino's own #include <libssh/libssh.h> runs,
//so an ssh_channel parameter would reference an as-yet-undefined type
void sshPumpStream(void* channelPtr, int isStderr) {
    ssh_channel channel = (ssh_channel)channelPtr;
    String& remainder = isStderr ? sshStderrRemainder : sshStdoutRemainder;
    uint16_t defaultColor = isStderr ? RED : WHITE;
    AnsiFilterState& ansi = isStderr ? sshStderrAnsi : sshStdoutAnsi;
    uint16_t& color = isStderr ? sshStderrColor : sshStdoutColor;

    char buf[256];
    int n;
    while ((n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), isStderr)) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n') {
                addWrappedHistoryLine(remainder, color);
                remainder = "";
                continue;
            }
            char outCh;
            bool colorChanged;
            if (ansiFilterByte(ansi, (uint8_t)ch, defaultColor, color, outCh, colorChanged)) {
                remainder += outCh;
            }
        }
    }
}

//the modal loop: owns the keyboard and screen for the life of the SSH session.
//First collects a password and opens an interactive shell channel, then streams
//keystrokes out / remote output in. "/quit" disconnects locally; the remote end
//closing the channel also exits.
//
//takes a void* rather than ssh_session for the same reason sshPumpStream takes a
//void*: this function's prototype gets hoisted above ssh.ino's own libssh.h include
void runSshBlocking(void* sessionPtr, const String& user) {
    ssh_session session = (ssh_session)sessionPtr;
    ssh_channel channel = NULL;

    sshUiState = SSH_UI_PASSWORD;
    sshInputBuffer = "";
    addWrappedHistoryLine("Password for " + user + ":");
    drawTerminalHistory();
    sshDrawInputRow();

    while (true) {
        M5Cardputer.update();
        delay(10);

        if (sshUiState == SSH_UI_SHELL) {
            sshPumpStream(channel, 0);
            sshPumpStream(channel, 1);
            if (ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel)) {
                addWrappedHistoryLine("ssh: remote closed the connection", YELLOW);
                break;
            }
        }

        bool enterPressed = readKeyboard(sshInputBuffer);   //shared input handler; also services Fn+;/Fn+. scrolling

        if (enterPressed) {
            if (sshUiState == SSH_UI_PASSWORD) {
                String password = sshInputBuffer;
                sshInputBuffer = "";

                bool authOk = ssh_userauth_password(session, NULL, password.c_str()) == SSH_AUTH_SUCCESS;
                password = "";   //don't linger in RAM longer than necessary

                if (!authOk) {
                    addWrappedHistoryLine("ssh: authentication failed", RED);
                    break;
                }

                channel = ssh_channel_new(session);
                if (channel == NULL ||
                    ssh_channel_open_session(channel) != SSH_OK ||
                    ssh_channel_request_pty(channel) != SSH_OK ||
                    ssh_channel_change_pty_size(channel, SSH_PTY_COLS, SSH_PTY_ROWS) != SSH_OK ||
                    ssh_channel_request_shell(channel) != SSH_OK) {
                    addWrappedHistoryLine(String("ssh: shell setup failed: ") + ssh_get_error(session), RED);
                    if (channel != NULL) {
                        ssh_channel_free(channel);
                        channel = NULL;
                    }
                    break;
                }

                ssh_set_blocking(session, 0);   //non-blocking so keyboard/UI never stalls on a channel read
                addWrappedHistoryLine("ssh: connected", GREEN);
                sshUiState = SSH_UI_SHELL;
            } else {
                if (sshInputBuffer == "/quit") {
                    break;
                }
                String line = sshInputBuffer + "\n";
                ssh_channel_write(channel, line.c_str(), line.length());
                sshInputBuffer = "";
            }
        }

        drawTerminalHistory();
        sshDrawInputRow();
    }

    if (channel != NULL) {
        //final drain so output already in flight isn't lost when the loop exits
        sshPumpStream(channel, 0);
        sshPumpStream(channel, 1);
        if (sshStdoutRemainder.length() > 0) {
            addWrappedHistoryLine(sshStdoutRemainder, sshStdoutColor);
            sshStdoutRemainder = "";
        }
        if (sshStderrRemainder.length() > 0) {
            addWrappedHistoryLine(sshStderrRemainder, sshStderrColor);
            sshStderrRemainder = "";
        }
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
}

//handles the "ssh" command
//
//Expected forms:
//ssh user@host
//ssh user@host port
void handleSshCommand(const String parts[], int partCount) {
    if (partCount < 2) {
        addWrappedHistoryLine("Usage: ssh user@host [port]");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        addWrappedHistoryLine("ssh: WiFi not connected. Run 'wifi connect' first.", RED);
        return;
    }

    String spec = parts[1];
    int atIndex = spec.indexOf('@');
    if (atIndex <= 0) {
        addWrappedHistoryLine("Usage: ssh user@host [port]");
        return;
    }
    String user = spec.substring(0, atIndex);
    String host = spec.substring(atIndex + 1);

    unsigned int port = SSH_DEFAULT_PORT;
    if (partCount > 2) {
        int parsedPort = parts[2].toInt();
        if (parsedPort > 0) {
            port = (unsigned int)parsedPort;
        }
    }

    if (!sshLibInitialized) {
        //this port doesn't run its C++ static constructor reliably on ESP32/Arduino,
        //so libssh_begin() (== the same init ssh_init() would trigger) must be called
        //explicitly before any other libssh call, per the library's own examples
        libssh_begin();
        sshLibInitialized = true;
    }

    ssh_session session = ssh_new();
    if (session == NULL) {
        addWrappedHistoryLine("ssh: could not allocate session", RED);
        return;
    }

    long timeout = SSH_CONNECT_TIMEOUT_SEC;
    ssh_options_set(session, SSH_OPTIONS_HOST, host.c_str());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, user.c_str());
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    addWrappedHistoryLine("Connecting to " + host + ":" + String(port) + " as " + user, PINK);
    drawTerminalHistory();

    if (ssh_connect(session) != SSH_OK) {
        addWrappedHistoryLine(String("ssh: connect failed: ") + ssh_get_error(session), RED);
        ssh_free(session);
        return;
    }

    //NOTE: this accepts whatever host key the server presents without checking it
    //against a known-hosts store -- there isn't one on this device yet, so a
    //network-position attacker could substitute keys undetected. Acceptable for
    //trusted local-network sysadmin use; not a substitute for real host key pinning.

    sshStdoutRemainder = "";
    sshStderrRemainder = "";
    sshStdoutAnsi = AnsiFilterState();
    sshStderrAnsi = AnsiFilterState();
    sshStdoutColor = WHITE;
    sshStderrColor = RED;
    runSshBlocking(session, user);

    ssh_disconnect(session);
    ssh_free(session);

    drawTerminalHistory();
    drawCommandBar(currentCommand);
    addWrappedHistoryLine("ssh: session ended");
}

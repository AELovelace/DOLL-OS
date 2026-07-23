//   ssh.ino
//   SSH client for the "ssh" command, for doing sysadmin work on remote hosts
//   over WiFi. Almost all of the protocol/crypto work is handled by libssh_esp32
//   (https://github.com/ewpa/libssh-esp32), a libssh port built against ESP-IDF's
//   existing mbedtls, so this file only wires it into DOLL-OS's terminal history /
//   keyboard input path -- the same modal pattern motoko.ino uses for PubSubClient.
// vibe stub. works tho
#include "libssh_esp32.h"   //Arduino/ESP32 glue; must precede libssh.h per the library's own examples
#include <libssh/libssh.h>

static const int SSH_DEFAULT_PORT = 22;
static const long SSH_CONNECT_TIMEOUT_SEC = 8;   //bounds the blocking ssh_connect() call
static const int SSH_PTY_COLS = 53;              //rough char width/height of the terminal sprite at text size 1
static const int SSH_PTY_ROWS = 18;

//ssh_connect()'s mbedtls key exchange (and the rest of the session -- auth, channel
//setup, interactive shell) needs far more stack than the ~8KB default Arduino loop
//task provides; every official libssh_esp32 client example runs this work on its own
//task with a stack in this range rather than calling it straight from setup()/loop().
//Without this, the board stack-overflows and reboots the moment ssh_connect() starts
//its handshake -- see sshConnectAndRun/handleSshCommand below.
static const unsigned int SSH_TASK_STACK_SIZE = 40960;

String sshInputBuffer = "";   //password-entry phase only; the shell phase forwards raw keystrokes and has no local buffer

//ANSI escape/UTF-8 filter state and current SGR-derived color, per stream -- lets a
//remote shell's color prompts/output (e.g. colored ls, a colored PS1) come through
//as row colors instead of leaking raw escape bytes into the visible text
AnsiFilterState sshStdoutAnsi;
AnsiFilterState sshStderrAnsi;
uint16_t sshStdoutColor = WHITE;
uint16_t sshStderrColor = RED;

//streaming-terminal state, per stream -- lets stdout/stderr each pump bytes straight into
//the terminal live (see terminal.ino's terminalStreamPutChar) without corrupting each
//other's in-progress row when they interleave
TerminalStreamState sshStdoutStream;
TerminalStreamState sshStderrStream;

static bool sshLibInitialized = false;

//draws the password prompt through the existing command bar during the password-entry phase.
//Characters are masked with '*' since they're rendered live as they're typed
void sshDrawInputRow() {
    String masked;
    masked.reserve(sshInputBuffer.length());
    for (size_t i = 0; i < sshInputBuffer.length(); i++) {
        masked += '*';
    }
    drawCommandBar("password> ", masked);
}

//drains whatever's already buffered on one ssh stream (stdout or stderr) without
//blocking, flushing complete lines into the shared terminal history as they arrive.
//
//takes a void* rather than ssh_channel: Arduino hoists this function's prototype to
//the top of the combined sketch, before ssh.ino's own #include <libssh/libssh.h> runs,
//so an ssh_channel parameter would reference an as-yet-undefined type
void sshPumpStream(void* channelPtr, int isStderr) {
    ssh_channel channel = (ssh_channel)channelPtr;
    TerminalStreamState& stream = isStderr ? sshStderrStream : sshStdoutStream;
    uint16_t defaultColor = isStderr ? RED : WHITE;
    AnsiFilterState& ansi = isStderr ? sshStderrAnsi : sshStdoutAnsi;
    uint16_t& color = isStderr ? sshStderrColor : sshStdoutColor;

    char buf[256];
    int n;
    while ((n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), isStderr)) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r') {
                //a bare CR (not followed by LF) is the remote rewinding to the start of the
                //current line to redraw it in place -- e.g. a shell repainting its prompt
                terminalStreamCarriageReturn(stream);
                continue;
            }
            if (ch == '\n') {
                terminalStreamNewline(stream);
                continue;
            }
            char outCh;
            bool colorChanged;
            bool isBackspace;
            bool eraseToEndOfLine;
            if (ansiFilterByte(ansi, (uint8_t)ch, defaultColor, color, outCh, colorChanged, isBackspace, eraseToEndOfLine)) {
                terminalStreamPutChar(stream, outCh, color);
            } else if (isBackspace) {
                terminalStreamBackspace(stream);
            } else if (eraseToEndOfLine) {
                terminalStreamEraseToEnd(stream);
            }
        }
    }
}

//owns an authenticated interactive shell channel: forwards raw keystrokes to the remote pty and
//streams stdout/stderr back into terminal history via sshPumpStream above. Replaces the old
//"buffer a full line locally, send it on Enter" loop -- a real pty needs every keystroke
//immediately (arrow keys, ctrl+c, backspace-before-enter, and full-screen programs like vim/top
//all depend on it). See RemoteSession in global.h for why the loop itself lives there instead of
//here.
//
//unlike sshPumpStream/runSshBlocking below, this can take ssh_channel directly instead of void*:
//Arduino's auto-prototype hoisting only pulls forward free-function signatures, not class member
//functions, so a class defined here (after ssh.ino's own libssh.h include) with every method
//inlined in the class body never gets a prototype hoisted above that include in the first place.
class SshShellSession : public RemoteSession {
public:
    explicit SshShellSession(ssh_channel ch) : channel(ch) {}

protected:
    void pumpIncoming() override {
        sshPumpStream(channel, 0);
        sshPumpStream(channel, 1);
    }

    bool isClosed() override {
        return ssh_channel_is_eof(channel) || !ssh_channel_is_open(channel);
    }

    void sendBytes(const String& bytes) override {
        ssh_channel_write(channel, bytes.c_str(), bytes.length());
    }

    void drawInputRow() override {
        drawCommandBar("$ ", "Fn+Q: quit");
    }

    void onClosed() override {
        addWrappedHistoryLine("ssh: remote closed the connection", YELLOW);
    }

private:
    ssh_channel channel;
};

//the modal loop's password phase: collects a masked password locally (this one line does need
//local buffering -- ssh_userauth_password wants it all at once), opens the interactive shell
//channel on success, then hands off to SshShellSession for the raw keystroke-forwarding phase.
//"/quit" during password entry backs out locally; Fn+Q backs out of an established shell.
//
//takes a void* rather than ssh_session for the same reason sshPumpStream takes a
//void*: this function's prototype gets hoisted above ssh.ino's own libssh.h include
void runSshBlocking(void* sessionPtr, const String& user) {
    ssh_session session = (ssh_session)sessionPtr;
    ssh_channel channel = NULL;

    sshInputBuffer = "";
    addWrappedHistoryLine("Password for " + user + ":");
    drawTerminalHistory();
    sshDrawInputRow();

    while (true) {
        M5Cardputer.update();
        delay(10);

        bool enterPressed = readKeyboard(sshInputBuffer);   //shared input handler; also services Fn+;/Fn+. recall and Ctrl+;/Ctrl+. scrolling

        if (enterPressed) {
            if (sshInputBuffer == "/quit") {
                return;
            }

            String password = sshInputBuffer;
            sshInputBuffer = "";

            bool authOk = ssh_userauth_password(session, NULL, password.c_str()) == SSH_AUTH_SUCCESS;
            password = "";   //don't linger in RAM longer than necessary

            if (!authOk) {
                addWrappedHistoryLine("ssh: authentication failed", RED);
                return;
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
                }
                return;
            }

            ssh_set_blocking(session, 0);   //non-blocking so keyboard/UI never stalls on a channel read
            addWrappedHistoryLine("ssh: connected", GREEN);
            break;
        }

        drawTerminalHistory();
        sshDrawInputRow();
    }

    SshShellSession shellSession(channel);
    shellSession.run();

    //final drain so output already in flight isn't lost when the loop exits -- every character
    //is drawn live by terminalStreamPutChar, so there's no buffered tail to flush
    terminalStreamReset(sshStdoutStream);   //defensive: no stale ownership survives past this session
    terminalStreamReset(sshStderrStream);
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
}

//heap-allocated so it survives the handoff from handleSshCommand's stack frame to the
//dedicated ssh task's (sshTaskEntry frees it once the session ends)
struct SshTaskArgs {
    String user;
    String host;
    unsigned int port;
};

//true for the lifetime of the dedicated ssh task; handleSshCommand blocks on this so the
//loop task's own M5Cardputer.update()/drawing calls never run concurrently with the ssh
//task's (RemoteSession::run() drives that same keyboard/display state itself)
static volatile bool sshTaskRunning = false;

//session connect through teardown -- moved out of handleSshCommand so all of it, not just
//ssh_connect() itself, runs on the dedicated ssh task and gets SSH_TASK_STACK_SIZE bytes
//of stack rather than the default loop task's
void sshConnectAndRun(const String& user, const String& host, unsigned int port) {
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

    terminalStreamReset(sshStdoutStream);
    terminalStreamReset(sshStderrStream);
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

//entry point for the dedicated ssh task (see handleSshCommand) -- frees the args and
//clears sshTaskRunning so the loop task's wait below unblocks, then deletes itself
void sshTaskEntry(void* pvParameters) {
    SshTaskArgs* args = (SshTaskArgs*)pvParameters;
    sshConnectAndRun(args->user, args->host, args->port);
    delete args;
    sshTaskRunning = false;
    vTaskDelete(NULL);
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

    SshTaskArgs* args = new SshTaskArgs{user, host, port};
    sshTaskRunning = true;
    //priority/core match libssh_esp32's own client examples (e.g. examples/libssh_scp) --
    //core portNUM_PROCESSORS - 1 keeps it off the WiFi stack's core on dual-core boards
    BaseType_t created = xTaskCreatePinnedToCore(sshTaskEntry, "sshTask", SSH_TASK_STACK_SIZE, args,
        (tskIDLE_PRIORITY + 3), NULL, portNUM_PROCESSORS - 1);

    if (created != pdPASS) {
        //e.g. heap too fragmented for a 40KB stack -- bail out here instead of spinning on
        //sshTaskRunning forever, since nothing will ever clear it
        delete args;
        sshTaskRunning = false;
        addWrappedHistoryLine("ssh: could not start session (out of memory)", RED);
        return;
    }

    while (sshTaskRunning) {
        delay(10);
    }
}

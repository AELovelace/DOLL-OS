# DOLL-OS .dapp Apps

`.dapp` files are text executables for the DOLL-OS shell. Put them on the SD card
under:

```text
/apps
```

From DOLL-OS, the same folder appears as:

```text
/sd/apps
```

They also work from internal flash at `/apps`, which survives having no SD card
in the slot. Both directories are created for you the first time you run `apps`.

Use:

```text
apps
run hello
run /sd/apps/hello.dapp
```

`run <name>` searches `/sd/apps` then `/apps` then treats the name as a path, and
appends `.dapp` if you leave it off — so `run hello` finds
`/sd/apps/hello.dapp`.

## Example

```text
# /sd/apps/hello.dapp
COLOR cyan
PRINT "hello from a DOLL-OS app"
PRINT "cwd=$cwd ip=$ip battery=$battery%"
INPUT name "name> "
PRINT "hi, $name"
RAND lucky 1 100
PRINT "lucky number: $lucky"
WAIT 750
COLOR pink
PRINT "tiny executable acquired"
EXIT
```

## Commands

```text
PRINT <text>        print text, with $variables expanded
ECHO <text>         alias for PRINT
COLOR <name>        white, red, green, yellow, blue, magenta, cyan, pink
CLEAR               clear terminal history
CLS                 alias for CLEAR
WAIT <ms>           pause while keeping the status bar and display alive
SLEEP <ms>          alias for WAIT
SET <name> <value>  set a numeric variable
ADD <name> <value>  add to a numeric variable
RAND <n> <max>      set numeric variable n to 0..max-1
RAND <n> <min> <max> set numeric variable n to min..max
SETSTR <name> <txt> set a string variable
APPEND <name> <txt> append to a string variable
INPUT <name> [p]    read a line into a string variable
LABEL <name>        define a jump target
:<name>             shorthand label
GOTO <name>         jump to a label
IF <l> <op> <r> GOTO <name>
IFEQ <l> <r> GOTO <name>
IFNE <l> <r> GOTO <name>
EXIT                leave the app
END                 alias for EXIT
```

`IF` supports `=`, `==`, `!=`, `<>`, `<`, `<=`, `>`, and `>=`.
`RAND roll 6` returns `0..5`; `RAND roll 1 6` returns `1..6`.
`IFEQ` and `IFNE` compare strings. Quote string literals that contain spaces.

`INPUT` takes over the command bar until you press enter — the answer goes into a
string variable, and the shell's own half-typed line is left untouched
underneath. `Fn+Q` at an `INPUT` prompt aborts the app.

Built-ins usable as `$name` or numeric values:

```text
$battery
$cwd
$heap
$ip
$millis
$seconds
$wifi
```

## Interactive Example

```text
# /sd/apps/ask.dapp
COLOR pink
PRINT "tiny prompt"

:again
INPUT reply "say> "
IFEQ $reply "/quit" GOTO done
PRINT "you said: $reply"
GOTO again

:done
PRINT "bye"
EXIT
```

## Limits

| | |
|---|---|
| lines per app | 160 |
| labels | 32 |
| numeric variables | 16 |
| string variables | 8, 128 characters each |
| steps before abort | 4000 |

## Stopping an app

A running app owns `loop()`. It is stopped two ways:

- **`Fn+Q`**, sampled wherever an app pauses — during `WAIT`/`SLEEP` and at an
  `INPUT` prompt. The app stops with `run: cancelled`.
- **the step budget**, for an app with neither. A loop of nothing but `GOTO`
  never yields to the keyboard, so it stops itself after 4000 steps rather than
  hanging the shell.

Nothing else reads the keyboard while an app runs — deliberately, so a keystroke
can't land in the shell's command line mid-app and get submitted the moment the
app exits.

`SET`, `ADD`, `IF` and `RAND` are integers only; `SETSTR`, `APPEND`, `INPUT`,
`IFEQ` and `IFNE` are the string half. `$cwd` and `$ip` are strings: they work in
`PRINT` and `IFEQ`, but evaluate to 0 in `IF`/`SET`/`ADD`. String variables are
truncated at 128 characters (DS allows 512 — it has PSRAM and this doesn't).

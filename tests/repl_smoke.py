#!/usr/bin/env python3
"""types into the `embr` REPL through a pseudo-terminal, so its line editor runs the way it does for a person
(piped input skips the editor). checks echo of expressions, backspace, the arrow keys, history, a multi-line
function, an error that doesn't end the session, Ctrl-C and Ctrl-D.

  repl_smoke.py <embr binary>
"""
import os, pty, re, select, sys, time

UP, DOWN, LEFT, RIGHT = "\x1b[A", "\x1b[B", "\x1b[D", "\x1b[C"
BACKSPACE, CTRL_C, CTRL_D, ENTER = "\x7f", "\x03", "\x04", "\r"

pid, fd = pty.fork()
if pid == 0:
    os.execv(sys.argv[1], [sys.argv[1]])

out = b""

def read_for(seconds):
    global out
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                return
            if not chunk:
                return
            out += chunk

def wait_for(text, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if text in plain():
            return True
        read_for(0.1)
    return False

def plain():
    # the editor redraws the line with escape codes; keep only what a person would read
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out.decode("utf-8", "replace")).replace("\r", "")

def typed(keys):
    for k in keys:              # one key at a time, the way a person types (an escape sequence is one key)
        os.write(fd, k.encode())
        time.sleep(0.01)

def fail(what):
    print("FAIL: " + what)
    print("---- transcript ----")
    print(plain())
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    sys.exit(1)

if not wait_for(">>> "):
    fail("no prompt")

def step(keys, expect, what, absent=None):
    # expect must show up in what this step printed; absent must not
    start = len(plain())
    typed(keys)
    end = time.time() + 10
    while expect not in plain()[start:] and time.time() < end:
        read_for(0.1)
    read_for(0.2)
    new = plain()[start:]
    if expect not in new or (absent and absent in new):
        fail(what)

step(list("1 + 2") + [ENTER], "3\n", "a bare expression prints its value")
step(list("print(12") + [BACKSPACE] + list("3)") + [ENTER], "13\n", "backspace removes the character before the cursor")
step(list("print(2)") + [LEFT, LEFT] + list("4") + [RIGHT] + list("5") + [ENTER], "425\n",
     "left and right move the cursor, typing inserts there")
step([UP, ENTER], "425\n", "up recalls the last line")
step(list("x = 7") + [ENTER] + [UP, UP, DOWN] + [ENTER], "x = 7\n>>> ", "down goes back toward the newest line", absent="425\n")
# the prompt is already on screen, so waiting for ">>> " would pass before the lines are read. wait until the
# continuation prompts have shown up and the last line is a fresh prompt
start = len(plain())
typed(["fn sq(n)", ENTER, "    return n * n", ENTER, "end", ENTER, ENTER])
end = time.time() + 10
while time.time() < end:
    new = plain()[start:]
    if new.count("... ") >= 3 and new.rstrip().endswith(">>>"):
        break
    read_for(0.1)
else:
    fail("a function typed over several lines")
step(list("sq(9)") + [ENTER], "81\n", "the multi-line function was defined")
step(list("nope(1)") + [ENTER], "undefined variable", "an error is reported")
step(list("print(x)") + [ENTER], "7\n", "the session survives an error and keeps its variables")
step(list("print(99") + [CTRL_C] + list("print(5)") + [ENTER], "5\n", "Ctrl-C drops the line", absent="99\n")
typed([CTRL_D])
read_for(1)
try:
    _, status = os.waitpid(pid, 0)
except ChildProcessError:
    status = 0
if os.WIFSIGNALED(status) or os.WEXITSTATUS(status) != 0:
    fail("Ctrl-D should end the REPL with status 0, got %r" % status)
print("repl smoke OK")

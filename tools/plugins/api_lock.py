#!/usr/bin/env python3
# keeps the version of every plugin and shipped module honest. tools/plugins/api.lock.json records, for each one, its version
# and the signature of each function. the check fails when the code and the lock disagree; the update command refuses to write
# the lock unless the version moved far enough for what changed.
#
#   api_lock.py check  EMBR     # EMBR is the embr binary. exit 1 and say what to do when the lock is out of date
#   api_lock.py update EMBR     # rewrite the lock from the build, after checking the version bump
#
# the version rule (MAJOR.MINOR.PATCH, about what a script can see)
#   a function removed, or its signature changed   -> raise MAJOR   (a script that worked may stop working)
#   a function added                               -> raise MINOR
#   anything else (a fix, a doc line, speed)        -> raise PATCH, or leave it
# before the first release the lock is "_released": false: anything may change and `update` does not look at the version. set it to
# true when plugins are first published (pkg), and the rule below starts to be enforced.
# a plugin's C++ build id and plugin interface number are checked by the loader, not by this version.
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
LOCK = os.path.join(HERE, "api.lock.json")


def run(embr, *args):
    p = subprocess.run([embr, *args], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit("%s %s failed: %s" % (embr, " ".join(args), p.stderr.strip() or p.stdout.strip()))
    return json.loads(p.stdout)


def current(embr):
    """{"plugin:NAME" or "module:NAME": {"version": "1.0.0", "functions": {name: signature}}} for what the source tree ships."""
    out = {}
    for pl in run(embr, "plugins", "--json"):
        if not os.path.isdir(os.path.join(ROOT, "plugins", pl["name"])):
            continue            # a test plugin or an example, not part of the interface embr ships
        full = run(embr, "plugin", pl["name"], "--json")
        out["plugin:" + pl["name"]] = {"version": full["version"], "functions": {f["name"]: f["signature"] for f in full["functions"] if f["bound"]}}
    for m in run(embr, "modules", "--json"):
        if not os.path.isfile(os.path.join(ROOT, "modules", m["name"] + ".embr")):
            continue
        full = run(embr, "module", m["name"], "--json")
        out["module:" + m["name"]] = {"version": full["version"], "functions": {f["name"]: f["signature"] for f in full["functions"]}}
    return out


def parts(v):
    try:
        a = [int(x) for x in v.split(".")]
    except ValueError:
        return None
    return a if len(a) == 3 else None


def diff(old, new):
    removed = sorted(set(old) - set(new))
    added = sorted(set(new) - set(old))
    changed = sorted(n for n in set(old) & set(new) if old[n] != new[n])
    return removed, added, changed


def bump_problem(name, old, new):
    """None when the version moved far enough, else what is wrong."""
    removed, added, changed = diff(old["functions"], new["functions"])
    a, b = parts(old["version"]), parts(new["version"])
    if a is None or b is None:
        return "%s: the version must look like 1.2.3 (was %s, is %s)" % (name, old["version"], new["version"])
    if b < a:
        return "%s: the version went down (%s -> %s)" % (name, old["version"], new["version"])
    if removed or changed:
        if b[0] <= a[0]:
            what = ["removed " + ", ".join(removed)] if removed else []
            what += ["changed the signature of " + ", ".join(changed)] if changed else []
            return "%s: %s, so raise the MAJOR version (still %s)" % (name, " and ".join(what), new["version"])
    elif added:
        if b[:2] <= a[:2]:
            return "%s: added %s, so raise the MINOR version (still %s)" % (name, ", ".join(added), new["version"])
    return None


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("check", "update"):
        sys.exit(__doc__.split("\n\n")[0] + "\n" + "usage: api_lock.py check|update EMBR")
    cmd, embr = sys.argv[1], sys.argv[2]
    now = current(embr)
    try:
        with open(LOCK) as f:
            lock = json.load(f)
    except (OSError, ValueError):
        lock = {}
    released = lock.get("_released", False)
    lock = {k: v for k, v in lock.items() if not k.startswith("_")}
    if cmd == "update":
        bad = []
        for name, new in now.items():
            if released and name in lock:
                p = bump_problem(name, lock[name], new)
                if p:
                    bad.append(p)
        if bad:
            print("not updating the lock:\n  " + "\n  ".join(bad))
            return 1
        with open(LOCK, "w") as f:
            json.dump(dict(now, _released=released), f, indent=1, sort_keys=True)
            f.write("\n")
        print("wrote %s (%d entries)" % (os.path.relpath(LOCK, ROOT), len(now)))
        return 0
    problems = []
    for name in sorted(set(now) | set(lock)):
        if name not in lock:
            problems.append("%s is new: run `tools/plugins/api_lock.py update` to record it" % name)
        elif name not in now:
            problems.append("%s is in the lock but not in the build: run `tools/plugins/api_lock.py update`" % name)
        elif now[name] != lock[name]:
            removed, added, changed = diff(lock[name]["functions"], now[name]["functions"])
            bits = []
            if removed: bits.append("removed " + ", ".join(removed))
            if added: bits.append("added " + ", ".join(added))
            if changed: bits.append("changed " + ", ".join(changed))
            if lock[name]["version"] != now[name]["version"]:
                bits.append("version %s -> %s" % (lock[name]["version"], now[name]["version"]))
            problems.append("%s differs from the lock (%s): %s" % (name, "; ".join(bits), bump_problem(name, lock[name], now[name]) or "run `tools/plugins/api_lock.py update` to record it"))
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

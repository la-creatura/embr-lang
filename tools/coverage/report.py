#!/usr/bin/env python3
"""turns the .gcda files of a --coverage build into a coverage report (see run.sh, which calls this).

  report.py --build BUILD_DIR --src SOURCE_DIR --out REPORT_DIR

writes REPORT_DIR/summary.txt (one line per file, worst first), REPORT_DIR/index.html and one page per file with
the lines no test reached marked. only embr's own code is counted: include/, src/, plugins/, without the bundled
third-party headers. a header is compiled
into many programs, so a line counts as reached when any of them ran it.
"""
import argparse, html, json, os, subprocess, sys
from collections import defaultdict

COUNTED = ("include/", "src/", "plugins/")
VENDORED = ("include/httplib/", "include/openssl/", "include/libffi/")   # other people's code, bundled for the build


def gcov_json(gcda_files, cwd):
    """yields gcov's json for each .gcda file, in batches (one gcov process per few hundred files)"""
    for i in range(0, len(gcda_files), 200):
        batch = gcda_files[i:i + 200]
        r = subprocess.run(["gcov", "--json-format", "--stdout", *batch], cwd=cwd,
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():   # one json document per line
            line = line.strip()
            if line.startswith("{"):
                yield json.loads(line)


def collect(build, src):
    gcdas = []
    for d, _, files in os.walk(build):
        gcdas += [os.path.join(d, f) for f in files if f.endswith(".gcda")]
    if not gcdas:
        sys.exit("report.py: no .gcda files under %s (did the tests run?)" % build)
    src = os.path.realpath(src)
    hits = defaultdict(dict)                 # repo-relative file -> {line: count}
    for doc in gcov_json(gcdas, build):
        base = doc.get("current_working_directory", build)
        for f in doc.get("files", []):
            path = os.path.realpath(os.path.join(base, f["file"]))
            if not path.startswith(src + os.sep):
                continue
            rel = os.path.relpath(path, src)
            if not rel.startswith(COUNTED) or rel.startswith(VENDORED):
                continue
            lines = hits[rel]
            for ln in f.get("lines", []):
                n = ln["line_number"]
                lines[n] = lines.get(n, 0) + ln["count"]
    return src, hits, len(gcdas)


def pct(a, b):
    return 100.0 * a / b if b else 100.0


def write_report(src, hits, out, ngcda):
    os.makedirs(os.path.join(out, "files"), exist_ok=True)
    rows = []
    for rel, lines in hits.items():
        total = len(lines)
        reached = sum(1 for c in lines.values() if c > 0)
        rows.append((rel, reached, total))
    rows.sort(key=lambda r: (pct(r[1], r[2]), r[0]))
    all_reached = sum(r[1] for r in rows)
    all_total = sum(r[2] for r in rows)

    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write("embr coverage: %d of %d lines reached (%.1f%%), %d files, from %d .gcda files\n\n"
                % (all_reached, all_total, pct(all_reached, all_total), len(rows), ngcda))
        f.write("%7s  %13s  %s\n" % ("reached", "lines", "file"))
        for rel, reached, total in rows:
            f.write("%6.1f%%  %6d/%6d  %s\n" % (pct(reached, total), reached, total, rel))

    style = ("<style>body{font:14px system-ui,sans-serif;margin:16px;background:#fff;color:#222}"
             "table{border-collapse:collapse}td,th{padding:2px 10px;text-align:left}"
             "tr:nth-child(even){background:#f4f4f4}.bar{display:inline-block;height:8px;background:#4a8}"
             "pre{font:12px ui-monospace,monospace;margin:0}.miss{background:#fdd}.hit{background:#efe}"
             ".n{color:#999;display:inline-block;width:5em;text-align:right;margin-right:1em}"
             "@media(prefers-color-scheme:dark){body{background:#181818;color:#ddd}tr:nth-child(even){background:#222}"
             ".miss{background:#512}.hit{background:#132}}</style>")
    page = ["<!doctype html><meta charset=utf-8><title>embr coverage</title>", style,
            "<h1>embr coverage</h1><p>%d of %d lines reached (%.1f%%). worst first. a line counts as reached when any test ran it.</p>"
            % (all_reached, all_total, pct(all_reached, all_total)),
            "<table><tr><th>reached</th><th></th><th>lines</th><th>file</th></tr>"]
    for rel, reached, total in rows:
        name = rel.replace("/", "__") + ".html"
        p = pct(reached, total)
        page.append("<tr><td>%.1f%%</td><td><span class=bar style='width:%dpx'></span></td><td>%d / %d</td>"
                    "<td><a href='files/%s'>%s</a></td></tr>" % (p, int(p), reached, total, name, html.escape(rel)))
        write_file_page(src, rel, hits[rel], os.path.join(out, "files", name), style)
    page.append("</table>")
    with open(os.path.join(out, "index.html"), "w") as f:
        f.write("\n".join(page))
    return all_reached, all_total


def write_file_page(src, rel, lines, path, style):
    try:
        with open(os.path.join(src, rel), encoding="utf-8", errors="replace") as f:
            text = f.read().split("\n")
    except OSError:
        return
    out = ["<!doctype html><meta charset=utf-8><title>%s</title>" % html.escape(rel), style,
           "<p><a href='../index.html'>all files</a></p><h1>%s</h1><pre>" % html.escape(rel)]
    for i, t in enumerate(text, 1):
        c = lines.get(i)
        cls = "" if c is None else (" class=hit" if c > 0 else " class=miss")
        out.append("<span%s><span class=n>%s</span>%s</span>" % (cls, "" if c is None else c, html.escape(t)))
    out.append("</pre>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", required=True)
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    src, hits, ngcda = collect(a.build, a.src)
    reached, total = write_report(src, hits, a.out, ngcda)
    print("coverage: %d of %d lines reached (%.1f%%), see %s" % (reached, total, pct(reached, total),
                                                                 os.path.join(a.out, "summary.txt")))


if __name__ == "__main__":
    main()

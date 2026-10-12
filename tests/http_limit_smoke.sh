#!/usr/bin/env bash
# the http plugin refuses a response bigger than its limit instead of filling memory, and http_download streams
# to the file. needs python3 for a local server (skips with exit 77 without it).
#
#   http_limit_smoke.sh <embr binary> [--backend=NAME]
set -u
embr="$1"; shift
skip() { echo "SKIP: $*"; exit 77; }
command -v python3 >/dev/null || skip "python3 not found"

work="$(mktemp -d)"
server_pid=""
cleanup() { [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT

python3 - "$work/port" > /dev/null 2> "$work/server.err" <<'PY' &
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302); self.send_header("Location", "/big"); self.send_header("Content-Length", "0"); self.end_headers(); return
        body = b"x" * (3 * 1024 * 1024) if self.path == "/big" else b"hello"
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(sys.argv[1], "w").write(str(s.server_address[1]))
s.serve_forever()
PY
server_pid=$!
for tries in $(seq 1 300); do [[ -s "$work/port" ]] && break; sleep 0.1; done
[[ -s "$work/port" ]] || { echo "server did not start:"; cat "$work/server.err"; exit 1; }
port="$(cat "$work/port")"

cat > "$work/t.embr" <<EMBR
import "http"
import "embrlib"
import "str"
import "fs"
base = "http://127.0.0.1:$port"
fn fails(f)
    try
        f()
    catch e
        return str_find(e["message"], "larger than") >= 0
    end
    return 0
end
print(len(http_get(base + "/big")["body"]))
print(fails(fn() return http_request("GET", base + "/big", {}, "", {"max_bytes": 1000}) end))
print(len(http_request("GET", base + "/big", {}, "", {"max_bytes": 0})["body"]))
print(len(http_request("GET", base + "/redirect")["body"]))
print(fails(fn() return http_request("GET", base + "/redirect", {}, "", {"max_bytes": 1000}) end))
print(http_get(base + "/small")["body"])
r = http_download(base + "/big", "$work/out.bin")
print(r["bytes"])
print(fails(fn() return http_download(base + "/big", "$work/out2.bin", {}, {"max_bytes": 1000}) end))
EMBR
out="$("$embr" "$@" "$work/t.embr" 2>"$work/err")"
want="3145728
1
3145728
3145728
1
hello
3145728
1"
[[ "$out" == "$want" ]] || { echo "FAIL: got:"; echo "$out"; cat "$work/err"; exit 1; }
[[ "$(stat -c %s "$work/out.bin" 2>/dev/null || stat -f %z "$work/out.bin")" == 3145728 ]] || { echo "FAIL: downloaded file has the wrong size"; exit 1; }
echo "http limits OK"

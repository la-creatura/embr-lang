#!/usr/bin/env bash
# end-to-end https:// test for the http plugin against a local TLS server with a freshly generated
# self-signed certificate (CTest's C++ harness can't host a server, so this is a shell-driven test,
# a smoke test, but automated).
#
#   https_smoke.sh <embr binary> [--backend=NAME]
#
# needs: python3, the openssl CLI, and an http plugin built WITH TLS (skips with exit 77 -> CTest
# "skipped" if any are missing, so a machine without OpenSSL isn't a failure).
set -u
embr="$1"; shift
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
skip() { echo "SKIP: $*"; exit 77; }
command -v python3 >/dev/null || skip "python3 not found"
command -v openssl >/dev/null || skip "openssl CLI not found"

work="$(mktemp -d)"
server_pid=""
cleanup() { [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$work/key.pem" -out "$work/cert.pem" -days 1 \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 \
    || skip "could not generate a test certificate"

python3 "$here/https_server.py" "$work/cert.pem" "$work/key.pem" > "$work/port" 2> "$work/server.err" &
server_pid=$!
for tries in $(seq 1 600); do [[ -s "$work/port" ]] && break; sleep 0.1; done   # up to 60s, python can be slow on its first start (24s on the macos runner)
[[ -s "$work/port" ]] || { echo "server did not start ($(python3 --version 2>&1), $(openssl version 2>&1)):"; cat "$work/server.err"; exit 1; }
port="$(head -1 "$work/port")"

cat > "$work/t.embr" <<EMBR
import "http"
import "embrlib"
import "os"
if !http_tls_available()
    print("NO-TLS")
    return 0
end
base = "https://127.0.0.1:" + os_getenv("HTTPS_PORT")
ca = os_getenv("HTTPS_CA")

# 1. default: self-signed certificate is rejected
try
    http_get(base + "/hello")
    print("FAIL default accepted a self-signed certificate")
catch e
    print("ok rejected")
end

# 2. verify 0: accepted
r = http_request("GET", base + "/hello", {}, "", {"verify": 0})
print(r["status"] + 0)
print(r["body"])

# 3. ca_file pinning: accepted WITHOUT disabling verification
r = http_request("GET", base + "/hello", {}, "", {"ca_file": ca})
print(r["body"])

# 4. POST with headers + body over TLS
r = http_request("POST", base + "/echo", {"X-Test": "hdr", "Content-Type": "text/plain"}, "payload", {"ca_file": ca})
print(r["body"])

# 5. redirects: followed within the cap, refused beyond it, visible with follow 0
print(http_request("GET", base + "/redir/5", {}, "", {"ca_file": ca})["body"])
try
    http_request("GET", base + "/redir/15", {}, "", {"ca_file": ca})
    print("FAIL redirect cap not enforced")
catch e
    print("ok redirect cap")
end
r = http_request("GET", base + "/redir/3", {}, "", {"ca_file": ca, "follow": 0})
print(r["status"] + 0)

# 6. bad ca_file path raises; wrong CA rejects
try
    http_request("GET", base + "/hello", {}, "", {"ca_file": "/definitely/not/here.pem"})
    print("FAIL missing ca_file accepted")
catch e
    print("ok missing ca")
end
EMBR

expected="ok rejected
200
hi
hi
POST|hdr|payload
end
ok redirect cap
302
ok missing ca"

export HTTPS_PORT="$port" HTTPS_CA="$work/cert.pem"
export EMBR_UNBUFFERED=1   # so a crash or hang still leaves the output of the steps that ran
actual="$("$embr" "$@" "$work/t.embr" 2>"$work/embr.err")"; rc=$?
if [[ "$actual" == "NO-TLS" ]]; then skip "http plugin was built without OpenSSL"; fi
if [[ "$actual" != "$expected" ]]; then
    echo "MISMATCH"; echo "--- expected"; echo "$expected"; echo "--- actual"; echo "$actual"; echo "--- exit status $rc (128+N means killed by signal N)"; echo "--- stderr"; cat "$work/embr.err"
    echo "--- the server was ready after about $(( tries / 10 ))s"
    if (( rc >= 128 )); then
        # the client crashed: show where. the server is still running, so the same command can be rerun under a debugger
        if command -v lldb >/dev/null; then
            echo "--- backtrace (lldb)"; lldb -b -o run -o "bt 30" -- "$embr" "$@" "$work/t.embr" 2>&1 | tail -45
        fi
        crash="$(ls -t "$HOME"/Library/Logs/DiagnosticReports/embr* 2>/dev/null | head -1)"
        [[ -n "$crash" ]] && { echo "--- crash report $crash"; head -c 6000 "$crash"; }
    fi
    exit 1
fi
echo "https smoke OK"

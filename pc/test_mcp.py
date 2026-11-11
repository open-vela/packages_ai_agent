#!/usr/bin/env python3
"""Local verification client for remote_ctrl MCP server.

Speaks the same JSON-RPC-over-HTTP dialect as ai_agent's mcp_client.c:
initialize, tools/list, then tools/call, checking results.

Two modes:
  * default: SELF-CONTAINED — builds a fake workspace (git repo + fake
    build/envsetup.sh) in a temp dir, starts the server on a free port,
    runs every check including the async vela_build lifecycle (fake build,
    nothing real is compiled), then tears everything down.
  * `test_mcp.py <url> [token]`: run against an ALREADY-RUNNING server
    (e.g. the one on your real workspace). Only read-only / guaranteed-
    rejected calls are made — never vela_build (it would start a REAL
    build on that machine).
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

CALC_PY = "def add(a, b):\n    return a + b\n\n\ndef sub(a, b):\n    return a - b\n"

# fake build/envsetup.sh: lunch() picks the target from the config path,
# m() fakes a 2-second build (fail-ap fails). Matches the real flow the
# server drives:  unset CROSSDEV; source build/envsetup.sh && lunch <cfg> && m -jN
ENVSETUP_SH = """\
# fake envsetup for test_mcp.py
lunch() {
  VELA_CFG="$1"
  VELA_TARGET="$(basename "$1")"
  echo "lunch: $VELA_TARGET (cfg $VELA_CFG)"
}
m() {
  echo "m $*"
  case "$VELA_TARGET" in
    fail-ap) echo "error: fake compile failure in $VELA_TARGET" >&2; return 1 ;;
  esac
  mkdir -p "out/openvela_vela_$VELA_TARGET"
  printf 'FAKEFW' > "out/openvela_vela_$VELA_TARGET/vela_ap.bin"
  sleep 2   # keep state=running long enough to test concurrent rejection
  echo "build completed successfully (fake $VELA_TARGET)"
}
"""


_id = 0
_pass = 0
_fail = 0


def rpc(url, token, method, params=None):
    global _id
    _id += 1
    body = json.dumps({"jsonrpc": "2.0", "method": method, "id": _id,
                       "params": params or {}}).encode()
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Accept", "application/json, text/event-stream")
    req.add_header("Connection", "close")
    if token:
        req.add_header("x-user-token", token)
    with urllib.request.urlopen(req, timeout=130) as r:
        return json.loads(r.read().decode())


class Client:
    def __init__(self, url, token=None):
        self.url, self.token = url, token

    def call(self, name, arguments):
        resp = rpc(self.url, self.token, "tools/call",
                   {"name": name, "arguments": arguments})
        content = resp.get("result", {}).get("content", [])
        return content[0]["text"] if content else json.dumps(resp)


def check(desc, cond, detail=""):
    global _pass, _fail
    mark = "PASS" if cond else "FAIL"
    print("  [%s] %s%s" % (mark, desc, ("  <- " + detail[:160]) if (detail and not cond) else ""))
    if cond:
        _pass += 1
    else:
        _fail += 1


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def make_fake_workspace(root):
    """Fake openvela workspace: git repo + calc.py + fake build system."""
    os.makedirs(os.path.join(root, "build"))
    os.makedirs(os.path.join(root, "vendor/fake/configs/fake-ap"))
    os.makedirs(os.path.join(root, "vendor/fake/configs/fail-ap"))
    with open(os.path.join(root, "calc.py"), "w") as f:
        f.write(CALC_PY)
    with open(os.path.join(root, "build/envsetup.sh"), "w") as f:
        f.write(ENVSETUP_SH)
    for t in ("fake-ap", "fail-ap"):
        with open(os.path.join(root, "vendor/fake/configs/%s/defconfig" % t), "w") as f:
            f.write("# fake defconfig %s\n" % t)
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.email", "t@t"], cwd=root, check=True)
    subprocess.run(["git", "config", "user.name", "t"], cwd=root, check=True)
    subprocess.run(["git", "add", "-A"], cwd=root, check=True)
    subprocess.run(["git", "commit", "-qm", "init"], cwd=root, check=True)


def wait_server_up(url, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url.rstrip("/mcp") or url, timeout=2) as r:
                if r.status == 200:
                    return True
        except OSError:
            time.sleep(0.1)
    return False


def run_tests(c, full=True):
    print("=== initialize ===")
    r = rpc(c.url, c.token, "initialize",
            {"capabilities": {}, "clientInfo": {"name": "agent", "version": "1.0.0"},
             "protocolVersion": "2025-03-26"})["result"]
    check("protocolVersion 2025-*", str(r.get("protocolVersion", "")).startswith("2025-"), str(r))

    print("\n=== tools/list ===")
    tools = rpc(c.url, c.token, "tools/list")["result"]["tools"]
    names = [t["name"] for t in tools]
    print("  tools: %s" % ", ".join(names))
    for expect in ("rc_repo_open", "rc_read_file", "rc_write_patch", "rc_run",
                   "rc_git", "rc_summary", "vela_build", "vela_build_status"):
        check("tool %s present" % expect, expect in names)

    if not full:
        # live-server mode: only safe / guaranteed-rejected calls
        print("\n=== rc_run: BLOCKED command (rm) ===")
        check("rm blocked", "ERROR" in c.call("rc_run", {"cmd": "rm -rf /"}))
        print("\n=== path escape guard ===")
        check("escape blocked", "ERROR" in c.call("rc_read_file", {"path": "../../../../etc/passwd"}))
        print("\n=== rc_git push (should be blocked) ===")
        check("push blocked", "ERROR" in c.call("rc_git", {"op": "push"}))
        return

    print("\n=== rc_repo_open (branch feat/demo) ===")
    out = c.call("rc_repo_open", {"repo_path": ".", "branch": "feat/demo"})
    check("opened + branch", "feat/demo" in out, out)

    print("\n=== rc_read_file calc.py ===")
    out = c.call("rc_read_file", {"path": "calc.py"})
    check("read calc.py", "def add" in out, out)

    print("\n=== rc_run: python3 -c (baseline) ===")
    out = c.call("rc_run", {"cmd": "python3 -c \"import calc; print(calc.add(2,3))\""})
    check("add(2,3)=5", "5" in out and "rc=0" in out, out)

    print("\n=== rc_write_patch: add mul() ===")
    out = c.call("rc_write_patch", {"path": "calc.py",
                                    "content": CALC_PY + "\n\ndef mul(a, b):\n    return a * b\n"})
    check("wrote mul", "wrote" in out, out)

    print("\n=== rc_run: verify mul ===")
    out = c.call("rc_run", {"cmd": "python3 -c \"import calc; print(calc.mul(6,7))\""})
    check("mul(6,7)=42", "42" in out, out)

    print("\n=== rc_run: BLOCKED command (rm) ===")
    check("rm blocked", "ERROR" in c.call("rc_run", {"cmd": "rm -rf /"}))

    print("\n=== path escape guard ===")
    check("escape blocked", "ERROR" in c.call("rc_read_file", {"path": "../../../../etc/passwd"}))

    print("\n=== rc_git add+commit ===")
    out = c.call("rc_git", {"op": "add", "paths": ["-A"]})
    check("git add", "rc=0" in out, out)
    out = c.call("rc_git", {"op": "commit", "message": "demo: add mul()"})
    check("git commit", "rc=0" in out, out)

    print("\n=== rc_git push (should be blocked) ===")
    check("push blocked", "ERROR" in c.call("rc_git", {"op": "push"}))

    print("\n=== rc_summary ===")
    out = c.call("rc_summary", {})
    check("summary mentions branch", "branch:" in out, out)

    print("\n=== vela_build: unknown target rejected ===")
    out = c.call("vela_build", {"target": "definitely-not-a-target"})
    check("unknown target ERROR", "ERROR" in out and "unknown target" in out, out)

    print("\n=== vela_build fake-ap (async start) ===")
    out = c.call("vela_build", {"target": "fake-ap"})
    check("started immediately", "started" in out, out)

    print("\n=== vela_build again while running (rejected) ===")
    out = c.call("vela_build", {"target": "fake-ap"})
    check("concurrent rejected", "ERROR" in out and "already running" in out, out)

    print("\n=== vela_build_status: running reply is tiny (no log_tail) ===")
    out = c.call("vela_build_status", {})
    check("running one-liner", "state: running" in out and "log_tail" not in out, out)

    print("\n=== vela_build_status {\"wait\": 60}: holds until terminal ===")
    import time as _t
    t0 = _t.time()
    state, out = poll_terminal(c, timeout=30, wait=60)
    check("state=success", state == "success", out)
    check("wait actually held (~2s fake build)",
          1.0 <= (_t.time() - t0), "%.1fs" % (_t.time() - t0))
    check("artifact reported", "vela_ap.bin" in out, out)
    check("success has no log_tail (keep it small)",
          "log_tail" not in out, out)

    print("\n=== vela_build fail-ap -> failed with log_tail ===")
    out = c.call("vela_build", {"target": "fail-ap"})
    check("fail-ap started", "started" in out, out)
    state, out = poll_terminal(c, timeout=30, wait=60)
    check("state=failed", state == "failed", out)
    check("log_tail has error", "fake compile failure" in out, out)


def poll_terminal(c, timeout=30, wait=0):
    """Poll vela_build_status (optionally with server-side wait) until
    success/failed or timeout."""
    deadline = time.time() + timeout
    out = ""
    while time.time() < deadline:
        out = c.call("vela_build_status", {"wait": wait} if wait else {})
        for st in ("success", "failed"):
            if "state: %s" % st in out:
                print("  %s" % out.splitlines()[0])
                return st, out
        time.sleep(0.3)
    return "timeout", out


def main():
    if len(sys.argv) > 1:
        # external-server mode (safe subset only)
        url = sys.argv[1]
        token = sys.argv[2] if len(sys.argv) > 2 else None
        run_tests(Client(url, token), full=False)
    else:
        with tempfile.TemporaryDirectory(prefix="rcmcp_test_") as root:
            make_fake_workspace(root)
            port = free_port()
            url = "http://127.0.0.1:%d/mcp" % port
            proc = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "remote_ctrl_mcp_server.py"),
                 "--root", root, "--host", "127.0.0.1", "--port", str(port),
                 "--log-dir", os.path.join(root, "logs"),
                 "--target", "fake-ap=vendor/fake/configs/fake-ap",
                 "--target", "fail-ap=vendor/fake/configs/fail-ap"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                if not wait_server_up(url):
                    print(proc.stdout.read().decode())
                    sys.exit("server did not come up on %s" % url)
                print("server up (fake root=%s)" % root)
                run_tests(Client(url), full=True)
            finally:
                proc.terminate()
                proc.wait(timeout=5)
    print("\n===== %d passed, %d failed =====" % (_pass, _fail))
    sys.exit(1 if _fail else 0)


if __name__ == "__main__":
    main()

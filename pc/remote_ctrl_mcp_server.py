#!/usr/bin/env python3
"""
remote_ctrl MCP server  (PC-side tool provider for ai_agent)

Exposes "in-workspace coding / execute / commit" and "vela firmware build"
as MCP tools so the openvela ai_agent's ReAct loop (running in QEMU/PC) can
drive coding work on the PC, exactly the way it already calls the Jira /
Gerrit MCP tools.

Protocol (matched to ai_agent packages/ai_agent/src/tools/mcp_client.c):
  * HTTP POST, single endpoint (default /mcp), JSON-RPC 2.0 body.
  * Methods: initialize, tools/list, tools/call.
  * Response: plain application/json  {"jsonrpc":"2.0","id":<id>,"result":{...}}
              (client also accepts SSE, but plain JSON is simplest).
  * Optional auth: header  x-user-token: <token>  (if --token given).
  * Stateless (no Mcp-Session round-trip needed).
  * tools/call params = {name, arguments}; result = {content:[{type:text,text}]}.
    The client reads result.content[0].text as the tool output.

Tools:  rc_repo_open  rc_read_file  rc_write_patch  rc_run  rc_git  rc_summary
        vela_build  vela_build_status

vela build design note: the device-side HTTP client times out a single
request at ~120 s (AGENT_LLM_SOCKET_TIMEOUT_SEC), but a vela build takes
35 s (incremental) to many minutes (clean).  So vela_build only STARTS the
build in a background thread and returns immediately; the agent polls
vela_build_status until state is success/failed.  vela_build_status
accepts an optional wait (<=90 s) that holds the call while the build is
running — the LLM cannot sleep between turns, so this is how it paces
its polling; the running-state response is kept tiny (no log tail) so
repeated polls cannot bloat the agent context.

Safety guards:
  * ALLOWED_ROOT: every path must resolve INSIDE the workspace root.
  * rc_run: first token must be in the command allow-list; shell=False
    (no pipes/&&/injection); hard timeout.
  * rc_git: no 'push' (pushing to Gerrit is a separate, confirmed step).
  * vela_build: target must be in the target table (validated lunch path,
    server-built command line — the agent never supplies raw shell); only
    one build at a time; build output dir is derived from the target name.

Usage:
  python3 remote_ctrl_mcp_server.py --root /path/to/workspace \
      --port 8760 [--token SECRET] [--host 0.0.0.0] \
      [--target name=config_path ...] [--jobs N] [--log-dir /tmp]
"""

import argparse
import datetime
import json
import os
import shlex
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL_VERSION = "2025-03-26"
SERVER_NAME = "remote_ctrl"
SERVER_VERSION = "0.2.0"

# ---- runtime config (set in main) ----
CFG = {
    "root": None,       # absolute workspace root; all paths confined here
    "token": None,      # optional shared secret (x-user-token)
    "run_timeout": 120, # seconds
    "max_out": 8000,    # max chars returned from a tool
    "jobs": None,       # vela build parallelism (None = os.cpu_count())
    "log_dir": "/tmp",  # where vela build logs are written
}

# vela build targets: name -> lunch config path (relative to workspace root).
# Extend/override at startup with repeated --target name=path arguments.
VELA_TARGETS = {
    "goldfish-arm64-v8a-ap":
        "vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap",
    "qemu-arm64-v8a-ap":
        "vendor/openvela/boards/vela/configs/qemu-arm64-v8a-ap",
}

# ---- vela build state (mutated only under VELA_LOCK) ----
VELA = {
    "state": "idle",    # idle | running | success | failed
    "target": None,
    "start": None,      # time.monotonic()
    "end": None,
    "rc": None,
    "log_path": None,
}
VELA_LOCK = threading.Lock()

LOG_TAIL_CHARS = 2500   # log tail returned by vela_build_status

# rc_run: only the FIRST token of a command is checked against this set.
# shell=False + argv split means no pipes / && / redirection are possible.
CMD_ALLOWLIST = {
    "git", "make", "cmake", "ninja", "ctest", "meson",
    "python", "python3", "pytest", "pip", "pip3",
    "gcc", "g++", "clang", "clang++", "ld", "objdump", "nm", "size",
    "ls", "cat", "head", "tail", "wc", "grep", "find", "file", "stat",
    "echo", "pwd", "diff", "sed", "awk", "sort", "uniq", "tr", "cut",
    "node", "npm", "npx", "go", "cargo", "rustc", "java", "javac",
    "true", "false", "env", "which", "date",
}


# ------------------------- path safety -------------------------

def _safe_path(rel):
    """Resolve rel under ALLOWED_ROOT; raise if it escapes."""
    root = CFG["root"]
    if rel is None:
        rel = "."
    # treat absolute inputs as relative to root (strip leading /)
    rel = str(rel)
    if rel.startswith("/"):
        rel = rel.lstrip("/")
    p = os.path.realpath(os.path.join(root, rel))
    if p != root and not p.startswith(root + os.sep):
        raise ValueError("path escapes workspace root: %s" % rel)
    return p


def _clip(s):
    s = s if isinstance(s, str) else str(s)
    m = CFG["max_out"]
    if len(s) > m:
        return s[:m] + "\n...[truncated %d chars]" % (len(s) - m)
    return s


def _run_argv(argv, cwd=None, timeout=None):
    """Run argv (shell=False) with timeout; return (rc, out)."""
    try:
        cp = subprocess.run(
            argv, cwd=cwd or CFG["root"],
            capture_output=True, text=True,
            timeout=timeout or CFG["run_timeout"],
        )
        out = (cp.stdout or "") + (cp.stderr or "")
        return cp.returncode, out
    except subprocess.TimeoutExpired:
        return 124, "[timeout after %ss]" % (timeout or CFG["run_timeout"])
    except FileNotFoundError:
        return 127, "command not found: %s" % argv[0]
    except Exception as e:  # noqa
        return 1, "exec error: %s" % e


# ------------------------- tools -------------------------

def tool_rc_repo_open(args):
    """Select workspace + (optionally) create/checkout a work branch."""
    repo = args.get("repo_path", ".")
    branch = args.get("branch")
    p = _safe_path(repo)
    if not os.path.isdir(p):
        return "ERROR: not a directory: %s" % repo
    is_git = os.path.isdir(os.path.join(p, ".git"))
    lines = ["repo: %s" % p, "git: %s" % ("yes" if is_git else "no")]
    if branch and is_git:
        rc, out = _run_argv(["git", "checkout", "-B", branch], cwd=p)
        lines.append("checkout -B %s: rc=%d %s" % (branch, rc, out.strip()))
    if is_git:
        rc, out = _run_argv(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=p)
        lines.append("current branch: %s" % out.strip())
    return "\n".join(lines)


def tool_rc_read_file(args):
    path = args.get("path")
    if not path:
        return "ERROR: 'path' required"
    p = _safe_path(path)
    if not os.path.isfile(p):
        return "ERROR: not a file: %s" % path
    with open(p, "r", errors="replace") as f:
        return _clip(f.read())


def tool_rc_write_patch(args):
    """Write full new content to a file (creates dirs). diff-mode is future."""
    path = args.get("path")
    content = args.get("content")
    mode = args.get("mode", "overwrite")  # overwrite | append
    if not path or content is None:
        return "ERROR: 'path' and 'content' required"
    p = _safe_path(path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    flag = "a" if mode == "append" else "w"
    with open(p, flag) as f:
        f.write(content)
    return "wrote %d bytes to %s (mode=%s)" % (len(content), path, mode)


def tool_rc_run(args):
    cmd = args.get("cmd")
    if not cmd:
        return "ERROR: 'cmd' required"
    try:
        argv = shlex.split(cmd)
    except ValueError as e:
        return "ERROR: bad command: %s" % e
    if not argv:
        return "ERROR: empty command"
    if argv[0] not in CMD_ALLOWLIST:
        return ("ERROR: command '%s' not in allow-list. Allowed: %s"
                % (argv[0], ", ".join(sorted(CMD_ALLOWLIST))))
    cwd = _safe_path(args.get("cwd", "."))
    rc, out = _run_argv(argv, cwd=cwd)
    return "rc=%d\n%s" % (rc, _clip(out))


def tool_rc_git(args):
    op = args.get("op")
    if not op:
        return "ERROR: 'op' required (status|diff|add|commit|log|branch)"
    cwd = _safe_path(args.get("cwd", "."))
    if op == "status":
        rc, out = _run_argv(["git", "status", "--short", "--branch"], cwd=cwd)
    elif op == "diff":
        extra = ["--stat"] if args.get("stat") else []
        rc, out = _run_argv(["git", "diff"] + extra, cwd=cwd)
    elif op == "log":
        rc, out = _run_argv(["git", "log", "--oneline", "-10"], cwd=cwd)
    elif op == "branch":
        rc, out = _run_argv(["git", "branch", "--show-current"], cwd=cwd)
    elif op == "add":
        paths = args.get("paths", ["-A"])
        if isinstance(paths, str):
            paths = [paths]
        rc, out = _run_argv(["git", "add"] + paths, cwd=cwd)
        if rc == 0 and not out.strip():
            out = "staged: %s" % " ".join(paths)
    elif op == "commit":
        msg = args.get("message") or "remote_ctrl: automated change"
        rc, out = _run_argv(["git", "commit", "-m", msg], cwd=cwd)
    elif op == "push":
        return ("ERROR: 'push' is disabled here. Push to Gerrit via the "
                "gerrit MCP tool after explicit confirmation.")
    else:
        return "ERROR: unknown op '%s'" % op
    return "rc=%d\n%s" % (rc, _clip(out))


def tool_rc_summary(args):
    cwd = _safe_path(args.get("cwd", "."))
    parts = []
    rc, out = _run_argv(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=cwd)
    parts.append("branch: %s" % out.strip())
    rc, out = _run_argv(["git", "status", "--short"], cwd=cwd)
    parts.append("working tree:\n%s" % (out.strip() or "(clean)"))
    rc, out = _run_argv(["git", "diff", "--stat"], cwd=cwd)
    if out.strip():
        parts.append("unstaged diffstat:\n%s" % out.strip())
    rc, out = _run_argv(["git", "diff", "--cached", "--stat"], cwd=cwd)
    if out.strip():
        parts.append("staged diffstat:\n%s" % out.strip())
    return _clip("\n\n".join(parts))


# ------------------------- vela build -------------------------

def _vela_out_dir(target):
    """Build output dir of the envsetup/lunch/m flow for this target."""
    return os.path.join(CFG["root"], "out", "openvela_vela_" + target)


def _vela_build_thread(target, cfg_path, jobs, clean, log_path):
    """Runs in the background; drives the real build and records the result."""
    try:
        if clean:
            out_dir = _vela_out_dir(target)
            import shutil
            shutil.rmtree(out_dir, ignore_errors=True)
        # Server-built command line only — target/cfg_path come from the
        # validated target table, never from agent-supplied raw shell.
        # unset CROSSDEV: user shells often export CROSSDEV=<32-bit prefix>,
        # which leaks into the arm64 ffmpeg build via Toolchain.defs ?=.
        script = ("unset CROSSDEV; "
                  "source build/envsetup.sh && lunch %s && m -j%d"
                  % (cfg_path, jobs))
        with open(log_path, "w") as log_fh:
            log_fh.write("# %s\n" % script)
            log_fh.flush()
            cp = subprocess.run(
                ["bash", "-c", script], cwd=CFG["root"],
                stdout=log_fh, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
            )
        rc = cp.returncode
    except Exception as e:  # noqa
        rc = 1
        try:
            with open(log_path, "a") as log_fh:
                log_fh.write("\n# server error: %s\n" % e)
        except OSError:
            pass
    with VELA_LOCK:
        VELA["state"] = "success" if rc == 0 else "failed"
        VELA["end"] = time.monotonic()
        VELA["rc"] = rc
    sys.stderr.write("[remote_ctrl] vela build target=%s rc=%d log=%s\n"
                     % (target, rc, log_path))


def tool_vela_build(args):
    target = args.get("target", "goldfish-arm64-v8a-ap")
    if target not in VELA_TARGETS:
        return ("ERROR: unknown target '%s'. Available: %s"
                % (target, ", ".join(sorted(VELA_TARGETS))))
    clean = bool(args.get("clean", False))
    jobs = args.get("jobs") or CFG["jobs"] or (os.cpu_count() or 8)
    try:
        jobs = max(1, min(int(jobs), 64))
    except (TypeError, ValueError):
        return "ERROR: 'jobs' must be an integer"

    with VELA_LOCK:
        if VELA["state"] == "running":
            return ("ERROR: a build is already running (target=%s, started %.0fs ago). "
                    "Poll vela_build_status instead."
                    % (VELA["target"], time.monotonic() - VELA["start"]))
        VELA["state"] = "running"
        VELA["target"] = target
        VELA["start"] = time.monotonic()
        VELA["end"] = None
        VELA["rc"] = None
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        VELA["log_path"] = os.path.join(
            CFG["log_dir"], "vela_build_%s_%s.log" % (target, stamp))

    t = threading.Thread(
        target=_vela_build_thread,
        args=(target, VELA_TARGETS[target], jobs, clean, VELA["log_path"]),
        daemon=True)
    t.start()
    return ("started: target=%s clean=%s jobs=%d log=%s\n"
            "The build runs on the PC in the background and takes 35 s "
            "(incremental) to many minutes (clean). Poll vela_build_status "
            "until state is success or failed."
            % (target, clean, jobs, VELA["log_path"]))


def _log_tail(path, limit=LOG_TAIL_CHARS):
    try:
        with open(path, "r", errors="replace") as f:
            f.seek(0, 2)             # end
            size = f.tell()
            f.seek(max(0, size - limit * 4))   # generous for CJK/UTF-8
            return f.read()[-limit:]
    except OSError:
        return "(log not readable yet)"


def tool_vela_build_status(args):
    # Optional server-side wait: while the build is running, hold the
    # request (polling every second) until the state changes or `wait`
    # seconds pass. The agent-side LLM cannot sleep between turns, so
    # without this it fires status calls back-to-back and bloats its own
    # context with repeated log tails. Keep wait <= 90 s: the device-side
    # HTTP client times out a read at ~120 s.
    try:
        wait = int(args.get("wait", 0))
    except (TypeError, ValueError):
        wait = 0
    wait = max(0, min(wait, 90))
    if wait > 0:
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            with VELA_LOCK:
                if VELA["state"] != "running":
                    break   # idle or terminal: waiting is pointless
            time.sleep(1)

    with VELA_LOCK:
        state = VELA["state"]
        target = VELA["target"]
        start = VELA["start"]
        end = VELA["end"]
        rc = VELA["rc"]
        log_path = VELA["log_path"]
    if state == "idle":
        return "state: idle (no build started yet)"

    now = end if end is not None else time.monotonic()
    elapsed = int(now - start)
    lines = ["state: %s" % state, "target: %s" % target,
             "elapsed: %ds" % elapsed]
    if state == "running":
        # Keep the running response tiny: the agent re-asks this every
        # wait-interval, and a full log tail per call is what blew up its
        # context. The log matters for diagnosing a failure, not progress.
        lines.append("still building — call again with {\"wait\": 60}")
        return "\n".join(lines)
    if state == "success":
        out_dir = _vela_out_dir(target)
        arts = []
        for name in ("vela_ap.bin", "nuttx"):
            p = os.path.join(out_dir, name)
            if os.path.isfile(p):
                arts.append("%s (%.1f MB)" % (name, os.path.getsize(p) / 1e6))
        lines.append("artifact: %s" % (", ".join(arts) or
                                       "out dir: %s" % out_dir))
    if state == "failed":
        lines.append("return code: %d" % rc)
        if log_path:
            lines.append("log: %s" % log_path)
            lines.append("log_tail:")
            lines.append(_log_tail(log_path))
    return _clip("\n".join(lines))


TOOLS = [
    {
        "handler": tool_rc_repo_open,
        "name": "rc_repo_open",
        "description": "Select the workspace repo and optionally create/checkout a work branch.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "repo_path": {"type": "string", "description": "repo path relative to workspace root"},
                "branch": {"type": "string", "description": "work branch to create/checkout (optional)"},
            },
        },
    },
    {
        "handler": tool_rc_read_file,
        "name": "rc_read_file",
        "description": "Read a text file inside the workspace.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
        },
    },
    {
        "handler": tool_rc_write_patch,
        "name": "rc_write_patch",
        "description": "Write full new content to a file (mode overwrite|append). Creates parent dirs.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string"},
                "content": {"type": "string"},
                "mode": {"type": "string", "enum": ["overwrite", "append"]},
            },
            "required": ["path", "content"],
        },
    },
    {
        "handler": tool_rc_run,
        "name": "rc_run",
        "description": "Run a build/test/lint command (allow-listed first token, shell=False, timeout). No pipes/&&.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "cmd": {"type": "string", "description": "e.g. 'python3 -m pytest -q'"},
                "cwd": {"type": "string", "description": "dir relative to workspace root (optional)"},
            },
            "required": ["cmd"],
        },
    },
    {
        "handler": tool_rc_git,
        "name": "rc_git",
        "description": "Git ops within the workspace: status|diff|add|commit|log|branch. Push is disabled (use gerrit MCP).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "op": {"type": "string", "enum": ["status", "diff", "add", "commit", "log", "branch"]},
                "message": {"type": "string"},
                "paths": {"type": "array", "items": {"type": "string"}},
                "stat": {"type": "boolean"},
            },
            "required": ["op"],
        },
    },
    {
        "handler": tool_rc_summary,
        "name": "rc_summary",
        "description": "Summarize current branch + working-tree changes (for voice feedback).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "handler": tool_vela_build,
        "name": "vela_build",
        "description": ("START a vela firmware build on this PC and return immediately "
                        "(the build itself takes 35 s incremental to many minutes clean; "
                        "the device-side HTTP client would time out waiting). "
                        "Poll vela_build_status until state is success or failed."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "target": {"type": "string",
                           "description": "target name (default goldfish-arm64-v8a-ap)"},
                "clean": {"type": "boolean",
                          "description": "delete out/openvela_vela_<target> first (full rebuild)"},
                "jobs": {"type": "integer", "description": "parallel jobs (-j)"},
            },
        },
    },
    {
        "handler": tool_vela_build_status,
        "name": "vela_build_status",
        "description": ("Poll the vela build started by vela_build. Pass "
                        "{\"wait\": 60} to hold the call until the state "
                        "changes (up to 90 s) instead of re-asking in a tight "
                        "loop. Returns state idle|running|success|failed, "
                        "target, elapsed; log_tail (last ~2.5 KB) and return "
                        "code on failure; artifact presence on success."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "wait": {"type": "integer",
                         "description": "seconds to hold while running (0-90)"},
            },
        },
    },
]

TOOL_BY_NAME = {t["name"]: t for t in TOOLS}


# ------------------------- JSON-RPC dispatch -------------------------

def jsonrpc_result(req_id, result):
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def jsonrpc_error(req_id, code, message):
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


def handle_rpc(obj):
    method = obj.get("method")
    req_id = obj.get("id")
    params = obj.get("params") or {}

    if method == "initialize":
        return jsonrpc_result(req_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })

    if method in ("notifications/initialized", "initialized"):
        return None  # notification: no response

    if method == "tools/list":
        tools = [{"name": t["name"], "description": t["description"],
                  "inputSchema": t["inputSchema"]} for t in TOOLS]
        return jsonrpc_result(req_id, {"tools": tools})

    if method == "tools/call":
        name = params.get("name")
        arguments = params.get("arguments") or {}
        _arg_preview = json.dumps(arguments, ensure_ascii=False)
        if len(_arg_preview) > 200:
            _arg_preview = _arg_preview[:200] + "..."
        sys.stderr.write("[remote_ctrl] TOOL_CALL %s args=%s\n"
                         % (name, _arg_preview))
        t = TOOL_BY_NAME.get(name)
        if not t:
            return jsonrpc_result(req_id, {
                "content": [{"type": "text", "text": "ERROR: unknown tool '%s'" % name}],
                "isError": True,
            })
        try:
            text = t["handler"](arguments)
        except Exception as e:  # noqa
            text = "ERROR: %s" % e
        sys.stderr.write("[remote_ctrl] TOOL_DONE %s -> %s\n"
                         % (name, _clip(text)[:200].replace("\n", " ")))
        return jsonrpc_result(req_id, {"content": [{"type": "text", "text": _clip(text)}]})

    return jsonrpc_error(req_id, -32601, "Method not found: %s" % method)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        sys.stderr.write("[remote_ctrl] " + (fmt % a) + "\n")

    def _send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # simple health check
        self._send_json({"ok": True, "server": SERVER_NAME, "root": CFG["root"]})

    def do_POST(self):
        # optional auth
        if CFG["token"]:
            if self.headers.get("x-user-token") != CFG["token"]:
                self._send_json({"jsonrpc": "2.0", "id": None,
                                 "error": {"code": -32001, "message": "unauthorized"}}, status=401)
                return
        length = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            obj = json.loads(raw.decode("utf-8"))
        except Exception as e:  # noqa
            self._send_json(jsonrpc_error(None, -32700, "parse error: %s" % e), status=400)
            return
        # single request (batch not needed by this client)
        resp = handle_rpc(obj)
        if resp is None:
            # notification: 202 no body
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        self._send_json(resp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, help="workspace root (all paths confined here)")
    ap.add_argument("--host", default="127.0.0.1", help="bind host (0.0.0.0 to reach from QEMU via 10.0.2.2)")
    ap.add_argument("--port", type=int, default=8760)
    ap.add_argument("--token", default=None, help="optional shared secret (x-user-token)")
    ap.add_argument("--run-timeout", type=int, default=120)
    ap.add_argument("--target", action="append", default=[], metavar="NAME=CONFIG_PATH",
                    help="extra build target, e.g. "
                         "--target bk7258-devkit-ap=vendor/beken/boards/bk7258/bk7258-devkit/configs/ap "
                         "(repeatable; extends the built-in goldfish/qemu-arm64 targets)")
    ap.add_argument("--jobs", type=int, default=None,
                    help="default -j for vela_build (default: os.cpu_count())")
    ap.add_argument("--log-dir", default="/tmp", help="where build logs are written")
    args = ap.parse_args()

    root = os.path.realpath(args.root)
    if not os.path.isdir(root):
        sys.exit("root not a directory: %s" % root)
    CFG["root"] = root
    CFG["token"] = args.token
    CFG["run_timeout"] = args.run_timeout
    CFG["jobs"] = args.jobs
    CFG["log_dir"] = args.log_dir
    os.makedirs(args.log_dir, exist_ok=True)
    for spec in args.target:
        name, sep, path = spec.partition("=")
        if not sep or not name or not path:
            sys.exit("--target expects NAME=CONFIG_PATH, got: %s" % spec)
        VELA_TARGETS[name] = path

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print("remote_ctrl MCP server on http://%s:%d/mcp  root=%s  auth=%s  targets=%s"
          % (args.host, args.port, root, "on" if args.token else "off",
             ", ".join(sorted(VELA_TARGETS))), flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Eigen-Experimental BusyBox + curl QEMU rundown.

Boots eigen.iso in QEMU (e1000, user networking, serial stdio), drives the
`eigen#` kernel shell, and runs a battery of BusyBox applet + curl tests,
one `run ...` line at a time. Each test waits for the next shell prompt and
checks expected output substrings.

Usage:
    python3 tools/rundown_qemu.py [--group core|write|pipe|net|all]
                                  [--iso eigen.iso] [--www /tmp/eigen_www]
                                  [--no-serve] [--boot-timeout 90]
                                  [--test-timeout 30]

Exit code: 0 if no FAIL/HANG, 1 otherwise (INFO/XFAIL never fail the run).

Notes / known kernel realities this script documents (not works around):
  * TarFS has no symlink support -> /bin/<applet> symlinks resolve to
    empty files; always invoke via `/bin/busybox <applet>`.
  * No writable filesystem (O_CREAT unsupported, TarFS read-only) ->
    `>` to new files, cp, wget-to-file, curl -o all fail.
  * Pipes are non-blocking with a 64KB buffer; fd table is global and
    shared across fork; fds are never closed on process exit.
"""

import argparse
import hashlib
import http.server
import os
import queue
import re
import subprocess
import sys
import threading
import time

PROMPT = "eigen# "
HELLO_PATH = "initramfs_root/hello.txt"

# --------------------------------------------------------------------------
# Test battery: (name, shell line, expects[], absent[], timeout, info_only)
# expects/absent are matched against output AFTER the echoed command line.
# --------------------------------------------------------------------------

def build_battery(www_dir, hello_md5):
    core = [
        # kernel shell itself
        ("ksh-mem",      "mem", [], ["Physical Memory"], 15, False),
        ("ksh-lspci",    "lspci", [], ["Intel", "Ethernet"], 15, False),
        ("ksh-ifconfig", "ifconfig", [], ["IPv4 Address"], 15, False),
        ("ksh-cat",      "cat /hello.txt", [], ["Hello from Eigen-Experimental"], 15, False),
        # busybox coreutils
        ("bb-echo",      "run /bin/busybox echo HELLO-RUNDOWN-1",
         [], ["HELLO-RUNDOWN-1", "exited with status 0"], 25, False),
        ("bb-cat",       "run /bin/busybox cat /hello.txt",
         [], ["Hello from Eigen-Experimental"], 25, False),
        ("bb-ls-root",   "run /bin/busybox ls /",
         [], ["bin", "hello.txt", "etc"], 25, False),
        ("bb-ls-bin",    "run /bin/busybox ls /bin",
         [], ["busybox", "curl"], 25, False),
        ("bb-uname",     "run /bin/busybox uname -a", [], ["Eigen"], 25, False),
        ("bb-pwd",       "run /bin/busybox pwd", [], ["\n/\n", "exited with status 0"], 25, False),
        ("bb-head",      "run /bin/busybox head -n 1 /hello.txt",
         [], ["Hello from Eigen-Experimental"], 25, False),
        ("bb-wc",        "run /bin/busybox wc /hello.txt", [], ["/hello.txt"], 25, False),
        ("bb-sort",      "run /bin/busybox sort /hello.txt",
         [], ["Hello from Eigen-Experimental"], 25, False),
        ("bb-grep",      "run /bin/busybox grep Eigen /hello.txt",
         [], ["Hello from Eigen-Experimental"], 25, False),
        ("bb-seq",       "run /bin/busybox seq 1 3", [], ["1", "2", "3"], 25, False),
        ("bb-true",      "run /bin/busybox true", [], ["exited with status 0"], 25, False),
        ("bb-false",     "run /bin/busybox false", [], ["exited with status 1"], 25, False),
        ("bb-env",       "run /bin/busybox env", [], ["PATH=/bin", "TERM=linux"], 25, False),
        ("bb-date",      "run /bin/busybox date -u", [], ["1970"], 25, False),
        ("bb-stat",      "run /bin/busybox stat /hello.txt", [], ["File:"], 25, False),
        ("bb-md5sum",    "run /bin/busybox md5sum /hello.txt", [], [hello_md5], 25, False),
        ("bb-sh-basic",  'run /bin/busybox sh -c "echo SH-WORKS"',
         [], ["SH-WORKS"], 25, False),
        ("bb-sh-vars",   'run /bin/busybox sh -c "X=42; echo VAR-$X"',
         [], ["VAR-42"], 25, False),
        ("bb-sh-andor",  'run /bin/busybox sh -c "echo A && echo B"',
         [], ["A", "B"], 25, False),
        ("bb-sh-or",     'run /bin/busybox sh -c "false || echo OR-OK"',
         [], ["OR-OK"], 25, False),
        ("bb-sh-for",    'run /bin/busybox sh -c "for i in 1 2 3; do echo LOOP-$i; done"',
         [], ["LOOP-1", "LOOP-2", "LOOP-3"], 25, False),
        ("bb-sh-glob",   'run /bin/busybox sh -c "echo /bin/busy*"',
         [], ["/bin/busybox"], 25, False),
        ("bb-sh-redirin",'run /bin/busybox sh -c "cat < /hello.txt"',
         [], ["Hello from Eigen-Experimental"], 25, False),
        ("bb-find",      "run /bin/busybox find / -name hello.txt",
         [], ["/hello.txt"], 30, False),
        ("bb-sed",       "run /bin/busybox sed s/Hello/Hi/ /hello.txt",
         [], ["Hi from Eigen-Experimental"], 25, False),
        ("bb-awk",       'run /bin/busybox awk "{print $2}" /hello.txt',
         [], ["from"], 25, False),
        ("bb-cut",       "run /bin/busybox cut -d ' ' -f 1 /hello.txt",
         [], ["Hello"], 25, False),
        ("bb-tr-redir",  'run /bin/busybox sh -c "tr a-z A-Z < /hello.txt"',
         [], ["HELLO FROM EIGEN-EXPERIMENTAL"], 25, False),
        ("bb-xargs",     'run /bin/busybox sh -c "xargs echo GOT < /hello.txt"',
         [], ["GOT Hello"], 25, False),
        ("bb-expr",      "run /bin/busybox expr 6 '*' 7", [], ["42"], 25, False),
        ("bb-basename",  "run /bin/busybox basename /bin/busybox",
         [], ["busybox"], 25, False),
        ("bb-dirname",   "run /bin/busybox dirname /bin/busybox", [], ["/bin"], 25, False),
        ("bb-cksum",     "run /bin/busybox cksum /hello.txt", [], ["/hello.txt"], 25, False),
        ("bb-dd",        "run /bin/busybox dd if=/hello.txt bs=1 count=5",
         [], ["Hello"], 25, False),
        ("bb-sleep",     "run /bin/busybox sleep 1",
         [], ["exited with status 0"], 25, False),
        # symlink gap: /bin/ls is a TarFS symlink with no target support
        ("bb-symlink-gap", "run /bin/ls /",
         [], ["Failed to launch executable"], 25, False),
        # busybox-internal PATH resolution hits the same gap (ENOEXEC -> empty script no-op)
        ("bb-path-silent-noop", 'run /bin/busybox sh -c "ls /"',
         ["hello.txt"], ["exited with status 0"], 25, False),
    ]

    write = [
        ("wr-redir-out",  'run /bin/busybox sh -c "echo X > /tmp/o"',
         [], ["exited with status 0"], 25, True),
        ("wr-cat-missing", "run /bin/busybox cat /tmp/o",
         [], ["No such file"], 25, False),
        ("wr-cp",         "run /bin/busybox cp /hello.txt /tmp/c",
         [], ["can't open"], 25, True),
        ("wr-mkdir",      "run /bin/busybox mkdir /tmp/d",
         [], ["exited with status"], 25, True),
        ("wr-touch",      "run /bin/busybox touch /tmp/t",
         [], ["exited with status"], 25, True),
        ("wr-rm-missing", "run /bin/busybox rm /nonexistent",
         [], ["No such file"], 25, True),
        ("wr-chmod",      "run /bin/busybox chmod 644 /hello.txt",
         [], ["exited with status"], 25, True),
    ]

    pipe = [
        ("pipe-basic",   'run /bin/busybox sh -c "echo PIPE-1 | cat"',
         [], ["PIPE-1"], 25, False),
        ("pipe-head",    'run /bin/busybox sh -c "echo PIPE-2 | head -n 1"',
         [], ["PIPE-2"], 25, False),
        ("pipe-multi",   'run /bin/busybox sh -c "seq 1 5 | sort -r | head -n 2"',
         [], ["5", "4"], 25, False),
        # ~108KB through a 64KB non-blocking pipe buffer: short-write behavior
        ("pipe-big",     'run /bin/busybox sh -c "seq 1 20000 | wc -l"',
         [], ["20000"], 30, False),
    ]

    srv = os.path.join(www_dir, "hello.txt")
    first_line = open(srv).readline().strip() if os.path.exists(srv) else ""
    net = [
        ("net-curl-direct",
         "run /bin/curl -s http://10.0.2.2:8083/hello.txt",
         [], [first_line, "exited with status 0"], 45, False),
        ("net-wget-stdout",
         "run /bin/busybox wget -O - http://10.0.2.2:8083/hello.txt",
         [], [first_line], 45, False),
        # the exact reported failure: curl piped into busybox head
        ("net-curl-pipe-head",
         'run /bin/busybox sh -c "/bin/curl -s http://10.0.2.2:8083/hello.txt | /bin/busybox head -n 1"',
         [], [first_line], 60, False),
        ("net-ping-raw",
         "run /bin/busybox ping -c 1 10.0.2.2",
         [], ["exited with status"], 45, True),
        ("net-nslookup",
         "run /bin/busybox nslookup google.com",
         [], ["Address"], 60, True),
    ]
    return {"core": core, "write": write, "pipe": pipe, "net": net}


# --------------------------------------------------------------------------
# QEMU driver
# --------------------------------------------------------------------------

class QemuSession:
    def __init__(self, iso, boot_timeout):
        self.iso = iso
        self.boot_timeout = boot_timeout
        self.proc = None
        self.q = queue.Queue()
        self.buf = []
        self.alive = False

    def _reader(self):
        try:
            while True:
                data = self.proc.stdout.read(1)
                if not data:
                    break
                self.q.put(data)
        except Exception:
            pass

    def start(self):
        cmd = ["qemu-system-x86_64", "-m", "256M",
               "-cdrom", self.iso,
               "-netdev", "user,id=n0",
               "-device", "e1000,netdev=n0",
               "-serial", "stdio",
               "-display", "none",
               "-no-reboot", "-no-shutdown"]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        threading.Thread(target=self._reader, daemon=True).start()
        self.alive = True
        if not self.wait_for(PROMPT, self.boot_timeout):
            return False
        return True

    def _drain(self, timeout):
        end = time.time() + timeout
        out = []
        while time.time() < end:
            try:
                out.append(self.q.get(timeout=max(0.1, end - time.time())))
            except queue.Empty:
                break
        text = "".join(out)
        self.buf.append(text)
        return text

    def wait_for(self, marker, timeout):
        end = time.time() + timeout
        text = ""
        while time.time() < end:
            text += self._drain(min(1.0, max(0.1, end - time.time())))
            if marker in text:
                return text
        return None

    def run_cmd(self, line, timeout):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        text = self.wait_for(PROMPT, timeout)
        hung = text is None
        if hung:
            text = self._drain(1.0)
        # evaluate only what came after our echoed command line
        idx = text.find(line)
        body = text[idx + len(line):] if idx >= 0 else text
        return body, hung

    def stop(self):
        try:
            if self.proc and self.proc.poll() is None:
                try:
                    self.proc.stdin.write("halt\n")
                    self.proc.stdin.flush()
                except Exception:
                    pass
                time.sleep(2)
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
        except Exception:
            pass
        self.alive = False
        return "".join(self.buf)


def serve_www(www_dir, port=8083):
    handler = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(
        *a, directory=www_dir, **kw)
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--group", default="all",
                    help="core|write|pipe|net|all (comma-separated ok)")
    ap.add_argument("--iso", default="eigen.iso")
    ap.add_argument("--www", default="/tmp/eigen_www")
    ap.add_argument("--no-serve", action="store_true")
    ap.add_argument("--boot-timeout", type=int, default=90)
    args = ap.parse_args()

    groups = [g.strip() for g in args.group.split(",")]
    if groups == ["all"]:
        groups = ["core", "write", "pipe", "net"]

    hello_md5 = ""
    if os.path.exists(HELLO_PATH):
        hello_md5 = hashlib.md5(open(HELLO_PATH, "rb").read()).hexdigest()

    battery = None  # built after www fixture exists (net expects need it)

    srv = None
    if "net" in groups and not args.no_serve:
        os.makedirs(args.www, exist_ok=True)
        if not os.path.exists(os.path.join(args.www, "hello.txt")):
            with open(os.path.join(args.www, "hello.txt"), "w") as f:
                f.write("RUNDOWN-WWW-LINE1\nRUNDOWN-WWW-LINE2\n"
                        "RUNDOWN-WWW-LINE3\nRUNDOWN-WWW-LINE4\n")
        srv = serve_www(args.www)
        print(f"[host] HTTP serving {args.www} on :8083", flush=True)

    battery = build_battery(args.www, hello_md5)

    results = []
    for group in groups:
        tests = battery[group]
        print(f"\n===== GROUP {group} ({len(tests)} tests, fresh boot) =====",
              flush=True)
        ses = QemuSession(args.iso, args.boot_timeout)
        if not ses.start():
            print(f"[{group}] BOOT FAILED (no prompt in {args.boot_timeout}s)")
            results += [(group, n, "BOOTFAIL", "") for n, _, _, _, _, _ in tests]
            ses.stop()
            continue
        print(f"[{group}] booted to prompt", flush=True)
        for name, line, absent, expects, timeout, info in tests:
            body, hung = ses.run_cmd(line, timeout)
            if hung:
                verdict = "HANG"
            else:
                missing = [e for e in expects if e not in body]
                present_bad = [e for e in absent if e in body]
                if info:
                    verdict = "INFO"
                elif not missing and not present_bad:
                    verdict = "PASS"
                else:
                    verdict = "FAIL"
            results.append((group, name, verdict, body))
            tag = {"PASS": "ok", "FAIL": "FAIL", "HANG": "HANG",
                   "INFO": "info", "BOOTFAIL": "BOOTFAIL"}[verdict]
            print(f"[{group}/{name}] {tag}", flush=True)
            if verdict in ("FAIL", "HANG"):
                print(f"--- output ({name}) ---")
                print(body[-3000:])
                print(f"--- end ({name}) ---")
        ses.stop()

    if srv:
        srv.shutdown()

    npass = sum(1 for r in results if r[2] == "PASS")
    nfail = sum(1 for r in results if r[2] == "FAIL")
    nhang = sum(1 for r in results if r[2] == "HANG")
    ninfo = sum(1 for r in results if r[2] == "INFO")
    print(f"\n===== SUMMARY: {npass} PASS, {nfail} FAIL, "
          f"{nhang} HANG, {ninfo} INFO =====", flush=True)
    for group, name, verdict, _ in results:
        if verdict != "PASS":
            print(f"  {verdict:6s} {group}/{name}")
    return 1 if (nfail or nhang) else 0


if __name__ == "__main__":
    sys.exit(main())

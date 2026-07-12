#!/usr/bin/env python3
"""Automated half of scripts/windows-smoke.md: native Windows live-path
validation for stretto.exe. Run from Git Bash at the repo root:

    python scripts/windows_smoke.py

Spawn rule (see the runbook, learned twice): EVERY synth spawn here has
a hard deadline, and a finally-block taskkill guarantees no stretto.exe
survives this script on any path - a leaked process is audible sound in
the operator's room, not silent debris.

pywinpty (pip install pywinpty) enables the real-console checks (6, 7);
without it they SKIP.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import time

EXE = os.path.join(os.path.dirname(__file__), "..", "stretto.exe")
EXE = os.path.abspath(EXE)
FAIL = 0


def report(name, ok, detail=""):
    global FAIL
    tag = "ok" if ok else "FAIL"
    print(f"  {name}: {tag}" + (f" - {detail}" if detail and not ok else ""))
    if not ok:
        FAIL = 1


def run(args, timeout=30, **kw):
    return subprocess.run([EXE] + args, capture_output=True, timeout=timeout, **kw)


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    if not os.path.isfile(EXE):
        print(f"FAIL: {EXE} not found - run 'make win' in WSL first")
        return 1

    print("=== 1. version / help ===")
    r = run(["--version"])
    report("--version", r.returncode == 0 and r.stdout.startswith(b"stretto "))
    r = run(["--help"])
    report("--help", r.returncode == 0 and r.stdout.startswith(b"usage:"))

    print("=== 2. native-FS render determinism ===")
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, "a.wav"), os.path.join(td, "b.wav")
        ra = run(["--render", "4", a, "--seed", "42"], timeout=120)
        rb = run(["--render", "4", b, "--seed", "42"], timeout=120)
        ok = ra.returncode == 0 and rb.returncode == 0 and sha256(a) == sha256(b)
        report("two seed-42 renders byte-identical", ok)
        # Cross-platform re-check when a WSL-built synth render exists:
        # the caller may drop reference.sha256 next to this script.
        ref = os.path.join(os.path.dirname(__file__), "reference.sha256")
        if os.path.isfile(ref):
            want = open(ref).read().split()[0]
            report("matches Linux reference hash", sha256(a) == want)
        else:
            print("  (no scripts/reference.sha256 - cross-platform hash check skipped;"
                  " tests/test_crossplatform.sh covers it)")

        print("=== 3. stdout-dash piping ===")
        r = run(["--render", "2", "-", "--seed", "42"], timeout=120)
        c = os.path.join(td, "c.wav")
        rc = run(["--render", "2", c, "--seed", "42"], timeout=120)
        ok = (r.returncode == 0 and rc.returncode == 0
              and hashlib.sha256(r.stdout).hexdigest() == sha256(c))
        report("stdout render == file render", ok)

    print("=== 4. winmm MIDI paths (host input-device count applies) ===")
    r = run(["--midi-list-devices"])
    devices = [ln for ln in r.stdout.decode(errors="replace").splitlines() if ln.strip()]
    report("--midi-list-devices exit 0", r.returncode == 0)
    if devices:
        print(f"  ({len(devices)} input device(s) present - open-failure checks skipped)")
    else:
        report("zero-device notice",
               b"no MIDI input devices found" in r.stderr)
        r = run(["--midi", "--no-ui"])
        report("wildcard fails loud",
               r.returncode == 1 and b"no MIDI input devices found" in r.stderr)
        r = run(["--midi", "5", "--no-ui"])
        report("--midi 5 fails loud",
               r.returncode == 1 and b"index 5 unavailable" in r.stderr)
        r = run(["--midi-default", "--no-ui"])
        report("--midi-default fails loud",
               r.returncode == 1 and b"index 0 unavailable" in r.stderr)

    print("=== 5. waveOut live smoke (6 s, harness-terminated) ===")
    p = subprocess.Popen([EXE, "--no-ui"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(6)
        alive = p.poll() is None
        report("alive after 6 s (waveOut playing)", alive,
               f"exited early rc={p.returncode}")
    finally:
        p.kill()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass

    try:
        import winpty  # type: ignore
    except ImportError:
        print("=== 6/7. SKIP: pywinpty not installed (pip install pywinpty) ===")
        return FAIL

    def pty_session(env=None, keys=("s", "q"), settle=3.0, deadline=25.0):
        """Run the exe in a real ConPTY; return (pre-key output, tail,
        exitstatus). HARD deadline: kills the pty process regardless."""
        proc = winpty.PtyProcess.spawn([EXE], dimensions=(30, 100), env=env)
        t0 = time.time()
        buf, tail = "", ""
        try:
            # Accumulate across the settle window: a single read can
            # return a partial early chunk without a full status-row
            # frame (ConPTY delivers in bursts).
            while time.time() - t0 < settle:
                try:
                    c = proc.read(4096)
                    buf += c if c else ""
                except Exception:
                    break
                if not c:
                    time.sleep(0.05)
            for k in keys:
                proc.write(k)
                time.sleep(0.8)
            # Drain until EOF (read raises) or the hard deadline -
            # NOT a fixed iteration count: empty reads are common in
            # the window between the quit key and process exit, and
            # the resume line lands at the very end of the stream.
            while time.time() - t0 < deadline:
                try:
                    c = proc.read(4096)
                except Exception:
                    break              # EOF: stream complete
                if c:
                    tail += c
                else:
                    time.sleep(0.05)
        finally:
            if proc.isalive():
                proc.terminate(force=True)
        return buf, tail, proc.exitstatus

    print("=== 6. real-console UI (ConPTY) ===")
    buf, tail, code = pty_session()
    report("ANSI UI drawn", "\x1b[" in buf)
    # ConPTY dims are (30, 100) -> the 074 rich panel path. "stretto"
    # sits in panel L1, "scale" in L2 (each contiguous inside one
    # literal, so SGR runs can never split the word).
    report("status panel rendered", "stretto" in buf and "scale" in buf)
    report("q quits clean (exit 0)", code == 0, f"exitstatus={code}")
    report("resume line with touched scale",
           "resume with: --seed" in tail and "--scale lydian" in tail)

    print("=== 7. NO_COLOR (ConPTY) ===")
    env = dict(os.environ)
    env["NO_COLOR"] = "1"
    buf, tail, code = pty_session(env=env, keys=("q",))
    import re
    sgr = re.findall(r"\x1b\[[0-9;]*m", buf)
    report("status panel without SGR", ("scale" in buf) and len(sgr) == 0,
           f"sgr={len(sgr)}")
    report("q quits clean (exit 0)", code == 0, f"exitstatus={code}")

    return FAIL


if __name__ == "__main__":
    try:
        rc = main()
    finally:
        # The spawn rule: nothing named stretto.exe survives this script.
        subprocess.run(["taskkill", "/F", "/IM", "stretto.exe"],
                       capture_output=True)
    print("PASS" if rc == 0 else "FAIL")
    sys.exit(rc)

#!/usr/bin/env python3
"""End-to-end suite for Jamtaba's NinjamZap video, against local NINJAM servers only.

Each scenario starts a fresh ninjamzap-server container and a fresh Jamtaba (isolated HOME, audio
on BlackHole 2ch, synthetic video, so no camera is used), drives it through JT_TEST_CONTROL, and
checks what reaches the wire (Sniffer) and what ninjamzap-core's real receiver does with it (peers/).

  make                                  # build the peers
  python3 -I suite.py                   # all scenarios
  python3 -I suite.py wire sync         # scenarios whose name contains a word
  python3 -I suite.py --list
"""
import array
import hashlib
import json
import math
import os
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
APP = os.environ.get("JT_APP", os.path.join(ROOT, ".deps/build/JamtabaZap.app/Contents/MacOS/JamtabaZap"))
PEERS = os.path.join(HERE, ".build")
OUT = os.path.join(HERE, ".out", time.strftime("%Y%m%d-%H%M%S"))
IMAGE = os.environ.get("JT_SERVER_IMAGE", "ninjamzap-server-test:latest")
AUDIO_DEVICE = "BlackHole 2ch"
BPM, BPI = 120, 16
INTERVAL = 60.0 / BPM * BPI
USER = "jamtaba"


class Failed(Exception):
    pass


class KnownGap(Exception):
    """A case the fork doesn't handle yet: reported, doesn't fail the run."""


def check(ok, what):
    print(f"    {'ok  ' if ok else 'FAIL'} {what}", flush=True)
    if not ok:
        raise Failed(what)


class Server:
    count = 0

    def __init__(self, workdir, video=True, extra=""):
        Server.count += 1
        self.name = f"nz-jtsuite-{os.getpid()}-{Server.count}"
        cfg = os.path.join(workdir, "server.cfg")
        with open(cfg, "w") as f:
            f.write(f"Port 2049\nMaxUsers 8\nMaxChannels 32 8\nAnonymousUsers yes\nAnonymousUsersCanChat yes\n"
                    f"AnonymousMaskIP no\nAllowVideoChannels {'yes' if video else 'no'}\nSendBufferKB 2048\n"
                    f"RecvBufferKB 1024\nACL 0.0.0.0/0 allow\nDefaultTopic suite\nDefaultBPM {BPM}\nDefaultBPI {BPI}\n"
                    f"SetVotingThreshold 1\nSetVotingVoteTimeout 60\n{extra}")
        subprocess.run(["docker", "run", "--rm", "-d", "--name", self.name, "-p", "0:2049", "-w", "/opt/ninjam",
                        "--entrypoint", "ninjamsrv", "-v", f"{cfg}:/opt/ninjam/test.cfg:ro", IMAGE, "test.cfg"],
                       check=True, capture_output=True)
        out = ""
        for _ in range(50):  # the port mapping isn't always there right after docker run
            out = subprocess.run(["docker", "port", self.name, "2049/tcp"], capture_output=True, text=True).stdout
            if out:
                break
            time.sleep(0.1)
        if not out:
            logs = subprocess.run(["docker", "logs", self.name], capture_output=True, text=True)
            self.stop()
            raise Failed(f"no port mapping for the server: {logs.stdout[-300:]}{logs.stderr[-300:]}")
        self.port = int(out.splitlines()[0].rsplit(":", 1)[1])
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                time.sleep(0.3)
                return
            except OSError:
                time.sleep(0.1)
        raise Failed("server didn't start")

    def stop(self):
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)


class Sniffer(threading.Thread):
    """A passive NINJAM client: records channels, interval begins, writes and payloads."""

    def __init__(self, port, name="sniff"):
        super().__init__(daemon=True)
        self.port, self.name2 = port, name
        self.begins, self.writes, self.channels, self.configs = [], [], {}, []
        self.payload, self.ended = {}, {}
        self.caps = None
        self.running = True
        self.lock = threading.Lock()
        self.ready = threading.Event()
        self.start()
        if not self.ready.wait(10):
            raise Failed("sniffer couldn't log in")

    def _send(self, t, b=b""):
        self.sock.sendall(bytes([t]) + struct.pack("<I", len(b)) + b)

    def chat(self, text):
        self._send(0xC0, b"MSG\0" + text.encode() + b"\0")

    def run(self):
        self.sock = socket.create_connection(("127.0.0.1", self.port))
        self.sock.settimeout(1)
        buf, subs, last = b"", set(), time.time()
        def next_msg():
            nonlocal buf
            while True:
                if len(buf) >= 5:
                    t, n = buf[0], struct.unpack("<I", buf[1:5])[0]
                    if len(buf) >= 5 + n:
                        p = buf[5:5 + n]
                        buf = buf[5 + n:]
                        return t, p
                try:
                    d = self.sock.recv(1 << 16)
                except socket.timeout:
                    return None
                if not d:
                    self.running = False
                    return None
                buf += d
        while self.running:
            m = next_msg()
            if time.time() - last > 5:
                self._send(0xFD)
                last = time.time()
            if m is None:
                continue
            t, p = m
            now = time.time()
            if t == 0x00:
                self.caps = struct.unpack("<I", p[8:12])[0]
                user = f"anonymous:{self.name2}"
                h = hashlib.sha1(hashlib.sha1(f"{user}:".encode()).digest() + p[:8]).digest()
                self._send(0x80, h + user.encode() + b"\0" + struct.pack("<II", 1, 0x00020000))
            elif t == 0x01:
                self._send(0x82, struct.pack("<H", 4))
                self.ready.set()
            elif t == 0x02:
                self.configs.append((now, struct.unpack("<H", p[:2])[0], struct.unpack("<H", p[2:4])[0]))
            elif t == 0x03:
                o = 0
                while o + 6 < len(p):
                    act, idx, fl = p[o], p[o + 1], p[o + 5]
                    o += 6
                    e = p.index(b"\0", o)
                    u = p[o:e].decode(errors="replace")
                    o = e + 1
                    e = p.index(b"\0", o)
                    nm = p[o:e].decode(errors="replace")
                    o = e + 1
                    with self.lock:
                        self.channels[(u.split("@")[0], idx)] = dict(active=act, flags=fl, name=nm)
                    if u not in subs and not u.startswith(self.name2):
                        subs.add(u)
                        self._send(0x81, u.encode() + b"\0" + struct.pack("<I", 0xFFFFFFFF))
            elif t == 0x04:
                fc = p[20:24]
                if any(fc):
                    b = dict(t=now, guid=p[:16], fourcc=fc.decode(errors="replace"), ch=p[24],
                             user=p[25:].split(b"\0")[0].decode(errors="replace").split("@")[0])
                    with self.lock:
                        self.begins.append(b)
                        self.payload[p[:16]] = []
            elif t == 0x05:
                g, fl, d = p[:16], p[16], p[17:]
                with self.lock:
                    self.writes.append(dict(t=now, guid=g, n=len(d), end=fl & 1))
                    if g in self.payload:
                        self.payload[g].append(d)
                    if fl & 1:
                        self.ended[g] = now

    def stop(self):
        self.running = False
        self.join(3)
        try:
            self.sock.close()
        except OSError:
            pass

    def of(self, user, fourcc, ch=None):
        with self.lock:
            return [b for b in self.begins if b["user"] == user and b["fourcc"] == fourcc and (ch is None or b["ch"] == ch)]

    def wait_intervals(self, user, fourcc, count, seconds=None):
        deadline = time.time() + (seconds if seconds is not None else (count + 3) * INTERVAL)
        while time.time() < deadline:
            if len(self.of(user, fourcc)) >= count:
                return True
            time.sleep(0.1)
        return False


class Jamtaba:
    def __init__(self, workdir, name=USER, marks=True, extra_env=None):
        self.dir = os.path.join(workdir, name)
        data = os.path.join(self.dir, "home/Library/Application Support/JamTaba 2")
        os.makedirs(data)
        cfg = dict(masterGain=1, userName=name, translation="en", theme="Navy_nm",
                   audio=dict(sampleRate=48000, bufferSize=256, firstIn=0, lastIn=1, firstOut=0, lastOut=1,
                              audioInputDevice=AUDIO_DEVICE, audioOutputDevice=AUDIO_DEVICE))
        for f in ("Jamtaba64.json", "Jamtaba.json"):
            with open(os.path.join(data, f), "w") as out:
                json.dump(cfg, out)
        self.control = os.path.join(self.dir, "control.txt")
        open(self.control, "w").close()
        self.log_path = os.path.join(workdir, f"{name}.log")
        env = dict(os.environ, HOME=os.path.join(self.dir, "home"), CFFIXED_USER_HOME=os.path.join(self.dir, "home"),
                   JT_TEST_CONTROL=self.control)
        if marks:
            env["JT_TEST_AVMARKS"] = marks if isinstance(marks, str) else "1"
        env.update(extra_env or {})
        self.proc = subprocess.Popen([APP], cwd=self.dir, env=env, stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT)
        self.name = name
        time.sleep(6)
        if self.proc.poll() is not None:
            check(False, f"Jamtaba exited at startup (code {self.proc.returncode}, log tail: {self.log()[-120:]!r})")

    def cmd(self, line):
        with open(self.control, "a") as f:
            f.write(line + "\n")

    def connect(self, port):
        self.cmd(f"connect localhost {port} {self.name}")

    def log(self):
        with open(self.log_path, errors="replace") as f:
            return re.sub(r"\x1b\[[0-9;]*m", "", f.read())

    def errors(self):
        return re.findall(r"(?:Could not open video codec|Error encoding video frame|Can't open the video decoder|"
                          r"GUID is not in map|error decoding video frame)[^\n]*", self.log())

    def alive(self):
        return self.proc.poll() is None

    def rss_mb(self):
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(self.proc.pid)], capture_output=True, text=True).stdout.strip()
        return int(out) / 1024 if out else 0

    def bytes_in(self):
        out = subprocess.run(["nettop", "-P", "-L", "1", "-n", "-x", "-p", str(self.proc.pid)],
                             capture_output=True, text=True).stdout.strip().splitlines()
        for line in reversed(out):
            f = line.split(",")
            if len(f) > 5 and f[1].startswith("JamtabaZap"):
                return int(f[4])
        return 0

    def stop(self):
        if self.alive():
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ── Parsing what's on the wire ─────────────────────────────────────────────
def chunks(data):
    out, o = [], 0
    while o + 4 <= len(data):
        n = struct.unpack(">I", data[o:o + 4])[0]
        if o + 4 + n > len(data):
            break
        out.append(data[o + 4:o + 4 + n])
        o += 4 + n
    return out, o == len(data)


def avcc_nals(frame):
    nals, o = [], 0
    while o + 4 <= len(frame):
        n = struct.unpack(">I", frame[o:o + 4])[0]
        if n == 0 or o + 4 + n > len(frame):
            return None
        nals.append(frame[o + 4:o + 4 + n])
        o += 4 + n
    return nals if o == len(frame) else None


def frame_types(path, fmt="h264"):
    r = subprocess.run(["ffprobe", "-v", "error", "-f", fmt, "-show_entries", "frame=pict_type", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    return "".join(l.split(",")[0] for l in r.stdout.splitlines())


def guided(sn):
    with sn.lock:
        return {g: b"".join(v) for g, v in sn.payload.items()}


class Sync:
    """ninjamzap-core's real receiver, recording what it plays."""

    def __init__(self, workdir, port, seconds, sender=USER, stall_at=0, stall_ms=0):
        self.dir = os.path.join(workdir, "sync")
        os.makedirs(self.dir, exist_ok=True)
        args = [os.path.join(PEERS, "nzsync"), "127.0.0.1", str(port), str(seconds), self.dir, sender, str(stall_at), str(stall_ms)]
        self.proc = subprocess.Popen(args, stdout=open(os.path.join(self.dir, "out.txt"), "w"), stderr=subprocess.STDOUT)

    def wait(self, timeout):
        self.proc.wait(timeout)
        with open(os.path.join(self.dir, "synclog.txt"), errors="replace") as f:
            return f.read()


def offsets(sync_dir, sr=48000):
    """Per mark, ms between the white frame shown and the 1 kHz tone heard (+ = video late)."""
    frames = [tuple(map(int, l.split(","))) for l in open(os.path.join(sync_dir, "frames.csv"))]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-f", "h264", "-i", os.path.join(sync_dir, "v.h264"), "-vf", "scale=8:6",
                          "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True).stdout
    count = len(raw) // 48
    lum = [sum(raw[i * 48:(i + 1) * 48]) / 48 for i in range(count)]
    n = min(count, len(frames))
    video = [frames[i][1] for i in range(1, n) if lum[i] > 128 and lum[i - 1] <= 128 and frames[i][1] > 0]
    a = array.array("f")
    with open(os.path.join(sync_dir, "out.f32"), "rb") as f:
        data = f.read()
    a.frombytes(data[:len(data) // 4 * 4])
    video = [v for v in video if v < len(a) - sr // 2]  # its tone may fall past the end of the recording
    w = 480
    env = []
    for k in range(len(a) // w):
        s = a[k * w:(k + 1) * w]
        re_, im = 0.0, 0.0
        for i, x in enumerate(s):
            t = 2 * math.pi * 1000 * i / sr
            re_ += x * math.cos(t)
            im += x * math.sin(t)
        env.append(math.hypot(re_, im) / w)
    th = max(env) * 0.3 if env else 1
    audio = [k * w for k in range(1, len(env)) if env[k] > th and env[k - 1] <= th]
    diffs = [(v - min(audio, key=lambda q: abs(q - v))) / sr * 1000 for v in video] if audio else []
    return diffs, len(video), len(audio), n


# ── Scenarios ──────────────────────────────────────────────────────────────
def join(ctx, camera=True, camera_first=False, **server_args):
    ctx.server = Server(ctx.dir, **server_args)
    ctx.sniff = Sniffer(ctx.server.port)
    ctx.jt = Jamtaba(ctx.dir)
    if camera_first:
        ctx.jt.cmd("camera on")
        time.sleep(1)
    ctx.jt.connect(ctx.server.port)
    deadline = time.time() + 15
    while time.time() < deadline and (USER, 0) not in ctx.sniff.channels:
        time.sleep(0.1)
    check((USER, 0) in ctx.sniff.channels, "Jamtaba joins the local server")
    if camera and not camera_first:
        ctx.jt.cmd("camera on")


def s_wire_format(ctx):
    join(ctx)
    check(ctx.sniff.caps & 2 != 0, "the server advertises video (server_caps bit 1)")
    check(ctx.sniff.wait_intervals(USER, "H264", 5), "5 H264 video intervals")
    time.sleep(1)
    ch = ctx.sniff.channels
    check(ch[(USER, 1)]["flags"] == 0x10, f"video channel 1 has flag 0x10 ({ch[(USER, 1)]})")
    check(ch[(USER, 0)]["flags"] == 0, "audio channel 0 has no video flag")
    check(not ctx.sniff.of(USER, "JTBv"), "no JTBv on a server that relays video")
    audio = ctx.sniff.of(USER, "OGGv", 0)
    video = ctx.sniff.of(USER, "H264")
    check(all(v["ch"] == 1 for v in video), "video travels on channel 1")
    check(not ctx.sniff.of(USER, "OGGv", 1), "no audio on the video channel")
    counters, payloads = [], guided(ctx.sniff)
    tested = 0
    for v in video[:-1]:
        if v["guid"] not in ctx.sniff.ended:
            continue
        parts, whole = chunks(payloads[v["guid"]])
        check(whole, "every chunk has its 4-byte length")
        check(len(parts[0]) == 20, "first chunk is the 20-byte marker")
        counters.append(struct.unpack(">I", parts[0][:4])[0])
        guid = parts[0][4:]
        prior = [a for a in audio if a["t"] <= v["t"] + 0.2]
        check(len(prior) >= 2 and prior[-2]["guid"] == guid, "marker names the audio interval that just ended")
        check(abs(prior[-1]["t"] - v["t"]) < 0.2, f"video BEGIN with the audio BEGIN ({abs(prior[-1]['t'] - v['t']) * 1000:.0f} ms)")
        sl = struct.unpack(">H", parts[1][:2])[0]
        check(parts[1][2] & 0x1f == 7 and parts[1][2 + sl + 2] & 0x1f == 8, "second chunk is SPS + PPS")
        nals = [avcc_nals(fr) for fr in parts[2:]]
        check(all(x is not None for x in nals), f"all {len(nals)} frame chunks are AVCC")
        types = [x[0][0] & 0x1f for x in nals]
        check(types and types[0] == 5, "the interval starts with an IDR")
        check(5 not in types[1:], "only one IDR per interval")
        check(len(types) >= 60, f"about 10 fps ({len(types)} frames in {INTERVAL:.0f} s)")
        tested += 1
    check(tested >= 3, f"{tested} complete intervals checked")
    check(all(b - a == 1 for a, b in zip(counters, counters[1:])), "interval counter increases by one")
    with ctx.sniff.lock:
        sizes = [w["n"] for w in ctx.sniff.writes if w["guid"] in {v["guid"] for v in video}]
    check(max(sizes) <= 9216, f"writes fit MAX_ENC_BLOCKSIZE (max {max(sizes)})")
    check(not ctx.jt.errors(), "no encoder errors in Jamtaba's log")


def s_camera_before_join(ctx):
    join(ctx, camera_first=True)
    check(ctx.sniff.wait_intervals(USER, "H264", 3), "video starts with the camera already on")
    time.sleep(1)
    check(ctx.sniff.channels[(USER, 1)]["flags"] == 0x10, "video channel 1 has flag 0x10")
    check(not ctx.sniff.of(USER, "OGGv", 1), "no audio on the video channel")
    audio = {a["guid"] for a in ctx.sniff.of(USER, "OGGv", 0)}
    payload = guided(ctx.sniff)
    for v in ctx.sniff.of(USER, "H264")[:-1]:
        parts, _ = chunks(payload[v["guid"]])
        check(parts[0][4:] in audio, "marker names real audio")
    check(not ctx.jt.errors(), "no encoder errors")


def s_camera_after_prepared(ctx):
    join(ctx, camera=False)
    check(ctx.sniff.wait_intervals(USER, "OGGv", 2), "audio is transmitting")
    ctx.jt.cmd("camera on")
    n = len(ctx.sniff.of(USER, "OGGv", 0))
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "video starts in the middle of the session")
    check(not ctx.sniff.of(USER, "OGGv", 1), "no audio on the video channel")
    check(len(ctx.sniff.of(USER, "OGGv", 0)) >= n + 2, "audio keeps going")


def s_receiver_core(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 1), "video started")
    log = Sync(ctx.dir, ctx.server.port, 50).wait(70)
    plays = re.findall(r"video PLAY: key=jamtaba:1 match=(\w+)", log)
    check(len(plays) >= 4, f"the core plays the video matched by GUID ({len(plays)}: {set(plays)})")
    bad = [l for l in log.splitlines() if re.search(r"DROP-RESYNC|video BURST", l) and "jamtaba" in l]
    check(not bad, f"no drops once playing ({bad[:2]})")
    types = frame_types(os.path.join(ctx.dir, "sync/v.h264"))
    frames = sum(1 for _ in open(os.path.join(ctx.dir, "sync/frames.csv")))
    check(frames >= 200, f"delivered frames decode ({frames})")
    check(len(types) >= frames * 0.95, f"ffmpeg decodes them ({len(types)} of {frames})")
    check("B" not in types, "no B-frames")


def s_av_sync(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 1), "video started")
    Sync(ctx.dir, ctx.server.port, 70).wait(90)
    diffs, nv, na, n = offsets(os.path.join(ctx.dir, "sync"))
    check(nv >= 15 and na >= 15, f"marks found: {nv} in video, {na} in audio ({n} frames)")
    # Skip the first interval: the receiver has just joined.
    settled = diffs[4:]
    med, sd = statistics.median(settled), statistics.pstdev(settled)
    print(f"    offsets ms: {[round(d) for d in settled]}")
    print(f"    median {med:.0f} ms, spread {sd:.0f} ms, min {min(settled):.0f}, max {max(settled):.0f}")
    ctx.metrics["median_ms"], ctx.metrics["spread_ms"] = round(med), round(sd)
    check(abs(med) <= ctx.sync_limit_ms, f"video within {ctx.sync_limit_ms} ms of the audio")
    check(sd <= 250, "stable over the session")


def s_legacy_server(ctx):
    join(ctx, video=False)
    check(ctx.sniff.caps & 2 == 0, "the server doesn't advertise video")
    check(ctx.sniff.wait_intervals(USER, "JTBv", 4), "falls back to JTBv")
    check(not ctx.sniff.of(USER, "H264"), "no H264 on a server without video")
    check(ctx.sniff.channels[(USER, 1)]["flags"] == 0, "channel 1 carries no 0x10 flag there")
    time.sleep(1)
    done = [b for b in ctx.sniff.of(USER, "JTBv") if b["guid"] in ctx.sniff.ended]
    payload = guided(ctx.sniff)[done[-1]["guid"]]
    path = os.path.join(ctx.dir, "jtbv.h264")
    with open(path, "wb") as f:
        f.write(payload)
    types = frame_types(path)
    check(len(types) >= 40, f"the Annex-B stream decodes ({len(types)} frames)")
    check(not ctx.jt.errors(), "no encoder errors in Jamtaba's log")


def flash_offsets(log):
    events = [(m.group(1), int(m.group(2))) for m in re.finditer(r"JT_TEST (tone|toneoff|flash) (\d+)", log)]
    tones, start = [], None
    for kind, t in events:
        if kind == "tone":
            start = t
        elif kind == "toneoff" and start is not None:
            if 100 <= t - start <= 500:  # metronome clicks are much shorter
                tones.append(start)
            start = None
    flashes = [t for kind, t in events if kind == "flash"]
    return [f - min(tones, key=lambda x: abs(x - f)) for f in flashes] if tones else [], len(flashes), len(tones)


def s_voice_chat_sync(ctx):
    ctx.server = Server(ctx.dir)
    ctx.sniff = Sniffer(ctx.server.port)
    ctx.jt = Jamtaba(ctx.dir, name="receiver", marks=False)
    sender = Jamtaba(ctx.dir, name=USER, marks="sparse")
    ctx.cleanup.append(sender.stop)
    ctx.jt.connect(ctx.server.port)
    sender.connect(ctx.server.port)
    sender.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "the sender transmits video")
    sender.cmd("voice on")
    time.sleep(INTERVAL * 2)
    check(ctx.sniff.channels[(USER, 0)]["flags"] == 2, "its audio is in voice chat")
    mark = len(ctx.jt.log())
    core = Sync(ctx.dir, ctx.server.port, 60)
    time.sleep(60)
    core.proc.wait(20)
    log = ctx.jt.log()[mark:]
    diffs, flashes, tones = flash_offsets(log)
    print(f"    Jamtaba receiver: {flashes} flashes, {tones} tones, offsets ms {diffs}")
    check(flashes >= 4 and tones >= 4, "marks in both")
    med = statistics.median(diffs[1:])
    ctx.metrics["jamtaba_median_ms"] = round(med)
    core_diffs, nv, na, _ = offsets(os.path.join(ctx.dir, "sync"))
    print(f"    core receiver: {nv} flashes, {na} tones, offsets ms {[round(d) for d in core_diffs]}")
    if len(core_diffs) > 2:
        ctx.metrics["core_median_ms"] = round(statistics.median(core_diffs[1:]))
    check(abs(med) <= ctx.sync_limit_ms, f"Jamtaba shows it within {ctx.sync_limit_ms} ms of the audio ({med:.0f})")
    check(not ctx.jt.errors() and not sender.errors(), "no errors")


def s_receives_ninjamzap_video(ctx):
    join(ctx, camera=False)
    # HD: frames of every size, including lengths that look like the start of an SPS/PPS block.
    peer = subprocess.Popen([os.path.join(PEERS, "nzpeer"), "127.0.0.1", str(ctx.server.port), "nzvideo", ctx.hd_frames, ctx.hd_sps,
                             "24", "1", "70"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    ctx.cleanup.append(peer.terminate)
    check(ctx.sniff.wait_intervals("nzvideo", "H264", 7), "a NinjamZap peer sends HD video")
    time.sleep(1)
    log = ctx.jt.log()
    plays = re.findall(r"JT_TEST video play match (\S+) frames (\d+)", log.replace('"', ""))
    print(f"    plays: {plays}")
    check(len(plays) >= 3, f"Jamtaba plays the video matched by GUID ({len(plays)})")
    check({m for m, _ in plays} <= {"PREV", "DS"}, "through the audio's GUID")
    check("JT_TEST video drop" not in log, "without drops")
    check(not ctx.jt.errors(), f"no decoder errors ({ctx.jt.errors()[:1]})")
    bad = log.count("Error decoding video")
    check(bad == 0 and "Could not open the H.264" not in log, f"H.264 decodes ({bad} errors)")
    check(ctx.jt.alive(), "still running")
    time.sleep(INTERVAL)
    check(("nzvideo", 1) in ctx.sniff.channels and ctx.jt.alive(), "and keeps going")


def s_frame_lengths_like_sps(ctx):
    join(ctx, camera=False)
    frames, sps = sps_lookalike_fixture()
    peer = subprocess.Popen([os.path.join(PEERS, "nzpeer"), "127.0.0.1", str(ctx.server.port), "nzvideo", frames, sps,
                             "10", "1", "50"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    ctx.cleanup.append(peer.terminate)
    check(ctx.sniff.wait_intervals("nzvideo", "H264", 5), "a peer sends frames with those lengths")
    time.sleep(1)
    log = ctx.jt.log()
    rows = [tuple(int(x) for x in m) for m in re.findall(r"JT_TEST video play match \S+ frames (\d+) previous (\d+) requested (\d+) decoded (\d+)", log.replace('"', ""))]
    bad = log.count("Error decoding video")
    check(len(rows) >= 2, f"Jamtaba plays them ({len(rows)})")
    check(bad == 0, f"H.264 decodes ({bad} errors)")
    full = [r for r in rows if r[1] > 0]
    check(full and all(r[3] >= r[1] * 0.8 for r in full), f"and the frames really come out ({[(r[3], r[1]) for r in full]})")
    images = [int(x) for x in re.findall(r"JT_TEST shown (\d+)", log)]
    check(images and max(images) > 0, f"as images on the screen ({images[-3:]})")


def jamtaba_to_jamtaba(ctx, seconds):
    ctx.server = Server(ctx.dir)
    ctx.sniff = Sniffer(ctx.server.port)
    ctx.jt = Jamtaba(ctx.dir, name="receiver", marks=False)
    sender = Jamtaba(ctx.dir, name=USER)
    ctx.cleanup.append(sender.stop)
    ctx.jt.connect(ctx.server.port)
    sender.connect(ctx.server.port)
    sender.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "the sender transmits video")
    time.sleep(seconds)
    return sender


def s_jamtaba_to_jamtaba_sync(ctx):
    sender = jamtaba_to_jamtaba(ctx, 60)
    log = ctx.jt.log()
    plays = re.findall(r"JT_TEST video play match (\S+)", log.replace('"', ""))
    check(len(plays) >= 4, f"the receiver plays the video ({len(plays)}: {set(plays)})")
    check(not ctx.jt.errors() and not sender.errors(), "no errors on either side")
    events = [(m.group(1), int(m.group(2))) for m in re.finditer(r"JT_TEST (tone|toneoff|flash) (\d+)", log)]
    tones, start = [], None
    for kind, t in events:
        if kind == "tone":
            start = t
        elif kind == "toneoff" and start is not None:
            if 100 <= t - start <= 400:  # metronome clicks are much shorter
                tones.append(start)
            start = None
    flashes = [t for kind, t in events if kind == "flash"]
    check(len(tones) >= 10 and len(flashes) >= 10, f"marks: {len(flashes)} flashes, {len(tones)} tones")
    diffs = [f - min(tones, key=lambda x: abs(x - f)) for f in flashes][2:]
    diffs = [d for d in diffs if abs(d) < 1000]
    med, sd = statistics.median(diffs), statistics.pstdev(diffs)
    print(f"    offsets ms: {diffs}")
    print(f"    median {med:.0f} ms, spread {sd:.0f} ms, min {min(diffs)}, max {max(diffs)}")
    ctx.metrics["median_ms"], ctx.metrics["spread_ms"] = round(med), round(sd)
    check(abs(med) <= ctx.sync_limit_ms, f"the receiver shows video within {ctx.sync_limit_ms} ms of the audio")
    check(sd <= 250, "stable over the session")


def s_legacy_jtbv_receive(ctx):
    join(ctx, camera=False, video=False)
    peer = subprocess.Popen([os.path.join(PEERS, "nzjtbv"), "127.0.0.1", str(ctx.server.port), "oldjt", ctx.jtbv, "10", "50"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    ctx.cleanup.append(peer.terminate)
    check(ctx.sniff.wait_intervals("oldjt", "JTBv", 3), "an old-format sender transmits JTBv")
    time.sleep(INTERVAL)
    found = [int(x) for x in re.findall(r"JT_TEST decoded images (\d+)", ctx.jt.log())]
    check(len(found) >= 2 and max(found) >= 40, f"Jamtaba still decodes and shows JTBv ({found})")
    check(not ctx.jt.errors(), "no decoder errors")


def s_camera_toggle(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 3), "video running")
    ctx.jt.cmd("camera off")
    time.sleep(INTERVAL * 1.5)
    before = len(ctx.sniff.of(USER, "H264"))
    last = ctx.sniff.of(USER, "H264")[-1]
    check(last["guid"] in ctx.sniff.ended, "camera off closes the open video interval")
    time.sleep(INTERVAL * 2)
    check(len(ctx.sniff.of(USER, "H264")) == before, "no more video while it's off")
    ctx.jt.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "H264", before + 2), "video comes back")
    time.sleep(1)
    check(ctx.sniff.channels[(USER, 1)]["flags"] == 0x10, "the channel is video again")
    resumed = [b for b in ctx.sniff.of(USER, "H264")[before:] if b["guid"] in ctx.sniff.ended]
    check(resumed, "a complete interval after resuming")
    parts, _ = chunks(guided(ctx.sniff)[resumed[0]["guid"]])
    check(len(parts) >= 3 and avcc_nals(parts[2])[0][0] & 0x1f == 5, "and it starts with an IDR")
    check(not ctx.jt.errors(), "no encoder errors")


def s_bpm_change(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "video running")
    ctx.sniff.chat("!vote bpm 160")
    deadline = time.time() + INTERVAL * 3
    while time.time() < deadline and not any(c[1] == 160 for c in ctx.sniff.configs):
        time.sleep(0.2)
    check(any(c[1] == 160 for c in ctx.sniff.configs), "the room changes to 160 BPM")
    n = len(ctx.sniff.of(USER, "H264"))
    check(ctx.sniff.wait_intervals(USER, "H264", n + 4), "video keeps going")
    video = ctx.sniff.of(USER, "H264")
    gaps = [b["t"] - a["t"] for a, b in zip(video[-3:], video[-2:])]
    check(all(g > 5.5 for g in gaps) and min(gaps) < 7, f"video follows the new interval ({[round(g, 2) for g in gaps]} s)")
    starts = [a["t"] for a in ctx.sniff.of(USER, "OGGv", 0)]
    check(all(min(abs(v["t"] - t) for t in starts) < 0.2 for v in video), "each video interval starts with its audio one")
    audio = {a["guid"] for a in ctx.sniff.of(USER, "OGGv", 0)}
    payload = guided(ctx.sniff)
    for v in video[-3:-1]:
        parts, _ = chunks(payload[v["guid"]])
        check(parts[0][4:] in audio, "markers still name real audio")


def s_rejoin(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "video running")
    ctx.jt.cmd("leave")
    time.sleep(3)
    n = len(ctx.sniff.of(USER, "H264"))
    time.sleep(INTERVAL)
    check(len(ctx.sniff.of(USER, "H264")) == n, "no video after leaving")
    ctx.jt.connect(ctx.server.port)
    check(ctx.sniff.wait_intervals(USER, "H264", n + 2), "video resumes after joining again")
    time.sleep(1)
    check(ctx.sniff.channels[(USER, 1)]["flags"] == 0x10, "the channel keeps its flag")
    check(ctx.jt.alive() and not ctx.jt.errors(), "no crash or encoder errors")


def s_receiver_stall(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 1), "video started")
    log = Sync(ctx.dir, ctx.server.port, 70, stall_at=24, stall_ms=10000).wait(90)
    lines = log.splitlines()
    plays = [i for i, l in enumerate(lines) if "video PLAY: key=jamtaba:1" in l]
    await_seen = any("AWAIT" in l and "jamtaba" in l for l in lines)
    print(f"    {len(plays)} plays, AWAIT seen: {await_seen}")
    check(len(plays) >= 3, "the core plays video around the stall")
    late = [l for l in lines[-40:] if "video PLAY: key=jamtaba:1" in l]
    check(late, "and is still playing at the end")


def s_transmit_off(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "video running")
    ctx.jt.cmd("transmit off")
    time.sleep(INTERVAL * 1.5)
    n, a = len(ctx.sniff.of(USER, "H264")), len(ctx.sniff.of(USER, "OGGv", 0))
    check(ctx.sniff.wait_intervals(USER, "H264", n + 3, INTERVAL * 4), "video keeps its intervals with audio off")
    check(len(ctx.sniff.of(USER, "OGGv", 0)) <= a + 1, "audio has stopped")
    time.sleep(1)
    payload = guided(ctx.sniff)
    done = [v for v in ctx.sniff.of(USER, "H264")[n:] if v["guid"] in ctx.sniff.ended]
    check(done, "complete intervals with audio off")
    for v in done:
        parts, _ = chunks(payload[v["guid"]])
        check(parts[0][4:] == bytes(16), "marker names no audio")
        check(len(parts) >= 3 and avcc_nals(parts[2])[0][0] & 0x1f == 5, "the interval starts with an IDR")
    ctx.jt.cmd("transmit on")
    m = len(ctx.sniff.of(USER, "H264"))
    check(ctx.sniff.wait_intervals(USER, "OGGv", a + 3, INTERVAL * 5), "audio comes back")
    check(ctx.sniff.wait_intervals(USER, "H264", m + 2, INTERVAL * 4), "video keeps going")
    time.sleep(1)
    audio = {x["guid"] for x in ctx.sniff.of(USER, "OGGv", 0)}
    last = [v for v in ctx.sniff.of(USER, "H264") if v["guid"] in ctx.sniff.ended][-1]
    check(chunks(guided(ctx.sniff)[last["guid"]])[0][0][4:] in audio, "markers name audio again")
    check(not ctx.jt.errors(), "no encoder errors")


def s_voice_chat(ctx):
    join(ctx)
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "video running")
    ctx.jt.cmd("voice on")
    time.sleep(INTERVAL * 2)
    n = len(ctx.sniff.of(USER, "H264"))
    check(ctx.sniff.channels[(USER, 0)]["flags"] == 2, "audio is in voice chat")
    check(ctx.sniff.wait_intervals(USER, "H264", n + 3, INTERVAL * 4), "video keeps its intervals in voice chat")
    log = Sync(ctx.dir, ctx.server.port, 30).wait(50)
    frames = sum(1 for _ in open(os.path.join(ctx.dir, "sync/frames.csv")))
    check(frames >= 100, f"the core delivers the video in voice chat ({frames} frames)")
    check(not ctx.jt.errors(), "no encoder errors")


PRESETS = [("SD", 320, 240, 10, 50), ("VGA", 640, 480, 15, 200), ("HD", 1280, 720, 24, 1200)]


def cpu_percent(pid):
    out = subprocess.run(["ps", "-o", "pcpu=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return float(out.replace(",", ".")) if out else 0.0


def interval_stream(ctx, video, name):
    """Writes a video interval's frames as Annex-B; returns (path, frame chunks, bytes)."""
    payload = guided(ctx.sniff)[video["guid"]]
    parts, _ = chunks(payload)
    sc = b"\0\0\0\1"
    out = b""
    sl = struct.unpack(">H", parts[1][:2])[0]
    pl = struct.unpack(">H", parts[1][2 + sl:4 + sl])[0]
    out += sc + parts[1][2:2 + sl] + sc + parts[1][4 + sl:4 + sl + pl]
    for fr in parts[2:]:
        for nal in avcc_nals(fr):
            out += sc + nal
    path = os.path.join(ctx.dir, name)
    with open(path, "wb") as f:
        f.write(out)
    return path, parts[2:], len(payload)


def stream_size(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-f", "h264", "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                       capture_output=True, text=True).stdout.strip().splitlines()[0]
    return tuple(int(x) for x in r.split(",")[:2])


def s_video_presets(ctx):
    for index, (name, w, h, fps, kbps) in enumerate(PRESETS):
        print(f"    -- {name}")
        ctx.server = Server(ctx.dir)
        ctx.sniff = Sniffer(ctx.server.port)
        ctx.jt = Jamtaba(os.path.join(ctx.dir, name))
        try:
            ctx.jt.cmd(f"video {index}")
            ctx.jt.connect(ctx.server.port)
            ctx.jt.cmd("camera on")
            check(ctx.sniff.wait_intervals(USER, "H264", 4), f"{name}: video running")
            time.sleep(1)
            done = [v for v in ctx.sniff.of(USER, "H264") if v["guid"] in ctx.sniff.ended]
            path, frames, size = interval_stream(ctx, done[-1], f"{name}.h264")
            check(stream_size(path) == (w, h), f"{name}: {w}x{h} ({stream_size(path)})")
            check(abs(len(frames) - fps * INTERVAL) <= fps * INTERVAL * 0.25, f"{name}: about {fps} fps ({len(frames) / INTERVAL:.1f})")
            real = size * 8 / INTERVAL / 1000
            check(real <= kbps * 1.5, f"{name}: within its {kbps} kbps ({real:.0f})")
            cpu = cpu_percent(ctx.jt.proc.pid)
            print(f"    {name}: {real:.0f} kbps, CPU {cpu:.0f}%, {ctx.jt.rss_mb():.0f} MB")
            ctx.metrics[f"{name}_cpu"] = round(cpu)
            check(not ctx.jt.errors(), f"{name}: no encoder errors")
        finally:
            ctx.sniff.stop()
            ctx.jt.stop()
            ctx.server.stop()
            ctx.sniff = ctx.jt = ctx.server = None


def s_video_preset_change(ctx):
    join(ctx, camera=False)
    ctx.jt.cmd("video 0")
    ctx.jt.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "video running in SD")
    ctx.jt.cmd("video 2")
    n = len(ctx.sniff.of(USER, "H264"))
    check(ctx.sniff.wait_intervals(USER, "H264", n + 3), "video keeps going after changing to HD")
    time.sleep(1)
    done = [v for v in ctx.sniff.of(USER, "H264") if v["guid"] in ctx.sniff.ended]
    path, frames, _ = interval_stream(ctx, done[-1], "after.h264")
    check(stream_size(path) == (1280, 720), f"now in HD ({stream_size(path)})")
    check(avcc_nals(frames[0])[0][0] & 0x1f == 5, "starting with an IDR")
    sizes = [stream_size(interval_stream(ctx, v, f"i{i}.h264")[0]) for i, v in enumerate(done)]
    print(f"    sizes per interval: {sizes}")
    check(all(a == b or b == (1280, 720) for a, b in zip(sizes, sizes[1:])), "the size changes at an interval boundary")
    log = Sync(ctx.dir, ctx.server.port, 30).wait(50)
    check(re.search(r"video PLAY: key=jamtaba:1", log) is not None, "the core plays it")
    check(not ctx.jt.errors(), "no encoder errors")


def s_legacy_server_stays_sd(ctx):
    join(ctx, camera=False, video=False)
    ctx.jt.cmd("video 2")
    ctx.jt.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "JTBv", 3), "falls back to JTBv")
    time.sleep(1)
    done = [b for b in ctx.sniff.of(USER, "JTBv") if b["guid"] in ctx.sniff.ended]
    path = os.path.join(ctx.dir, "jtbv.h264")
    with open(path, "wb") as f:
        f.write(guided(ctx.sniff)[done[-1]["guid"]])
    check(stream_size(path) == (320, 240), f"stays in SD even with HD chosen ({stream_size(path)})")


def vorbis_nominal(payload):
    i = payload.find(b"\x01vorbis")
    return struct.unpack("<i", payload[i + 7 + 4 + 1 + 4 + 4:i + 7 + 4 + 1 + 4 + 8])[0] if i >= 0 else None


def s_audio_presets(ctx):
    join(ctx, camera=False)
    check(ctx.sniff.wait_intervals(USER, "OGGv", 2), "audio running")
    time.sleep(1)
    first = guided(ctx.sniff)[ctx.sniff.of(USER, "OGGv", 0)[-1]["guid"]]
    check(vorbis_nominal(first) == 128000, f"Standard by default ({vorbis_nominal(first)})")
    for kbps in (64, 192, 256):
        ctx.jt.cmd(f"audio {kbps}")
        n = len(ctx.sniff.of(USER, "OGGv", 0))
        check(ctx.sniff.wait_intervals(USER, "OGGv", n + 2, INTERVAL * 4), f"audio keeps going at {kbps} kbps")
        time.sleep(0.5)
        latest = guided(ctx.sniff)[ctx.sniff.of(USER, "OGGv", 0)[-1]["guid"]]
        check(vorbis_nominal(latest) == kbps * 1000, f"{kbps} kbps in the Vorbis header ({vorbis_nominal(latest)})")


def s_hd_jamtaba_to_jamtaba(ctx):
    ctx.server = Server(ctx.dir)
    ctx.sniff = Sniffer(ctx.server.port)
    ctx.jt = Jamtaba(ctx.dir, name="receiver", marks=False)
    sender = Jamtaba(ctx.dir, name=USER)
    ctx.cleanup.append(sender.stop)
    ctx.jt.connect(ctx.server.port)
    sender.connect(ctx.server.port)
    sender.cmd("video 2")
    sender.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "the sender transmits HD")
    time.sleep(50)
    send_cpu, recv_cpu = cpu_percent(sender.proc.pid), cpu_percent(ctx.jt.proc.pid)
    log = ctx.jt.log()
    plays = re.findall(r"JT_TEST video play match (\S+)", log.replace('"', ""))
    check(len(plays) >= 3, f"the receiver plays it ({len(plays)})")
    check(not ctx.jt.errors() and not sender.errors(), "no errors on either side")
    print(f"    CPU sender {send_cpu:.0f}%, receiver {recv_cpu:.0f}%, receiver {ctx.jt.rss_mb():.0f} MB")
    ctx.metrics["sender_cpu"], ctx.metrics["receiver_cpu"] = round(send_cpu), round(recv_cpu)


def s_receive_video_off(ctx):
    join(ctx, camera=False)
    peer = subprocess.Popen([os.path.join(PEERS, "nzpeer"), "127.0.0.1", str(ctx.server.port), "nzvideo", ctx.frames, ctx.sps,
                             "15", "1", "60"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    ctx.cleanup.append(peer.terminate)
    check(ctx.sniff.wait_intervals("nzvideo", "H264", 2), "a peer sends video")
    ctx.jt.cmd("receivevideo off")
    time.sleep(INTERVAL + 2)  # the server finishes relaying the interval in progress
    start = ctx.jt.bytes_in()
    time.sleep(INTERVAL * 2)
    off = ctx.jt.bytes_in() - start
    check(off < 300 * 1024, f"with Receive video off it doesn't download it ({off / 1024:.0f} KB in 2 intervals)")
    ctx.jt.cmd("receivevideo on")
    time.sleep(INTERVAL + 2)
    start = ctx.jt.bytes_in()
    time.sleep(INTERVAL * 2)
    on = ctx.jt.bytes_in() - start
    check(on > 800 * 1024, f"on again, it does ({on / 1024:.0f} KB in 2 intervals)")


def s_encoder_keeps_up_hd(ctx):
    ctx.server = Server(ctx.dir)
    ctx.sniff = Sniffer(ctx.server.port)
    ctx.jt = Jamtaba(ctx.dir, marks="detail")
    ctx.jt.cmd("video 2")
    ctx.jt.connect(ctx.server.port)
    ctx.jt.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "H264", 3), "HD video running, with detail")
    samples, rss = [], []
    for _ in range(20):
        time.sleep(2)
        samples.append(cpu_percent(ctx.jt.proc.pid))
        rss.append(ctx.jt.rss_mb())
    done = [v for v in ctx.sniff.of(USER, "H264") if v["guid"] in ctx.sniff.ended]
    path, frames, size = interval_stream(ctx, done[-1], "hd.h264")
    real = size * 8 / INTERVAL / 1000
    print(f"    {len(frames) / INTERVAL:.1f} fps, {real:.0f} kbps, CPU {statistics.mean(samples[3:]):.0f}%, RSS {rss[2]:.0f} -> {rss[-1]:.0f} MB")
    ctx.metrics.update(fps=round(len(frames) / INTERVAL, 1), kbps=round(real), cpu=round(statistics.mean(samples[3:])), rss=round(max(rss)))
    check(stream_size(path) == (1280, 720), "HD")
    check(real <= 1200 * 1.5, f"within its bitrate ({real:.0f} kbps)")
    check(rss[-1] < rss[2] + 150, f"memory doesn't pile up ({rss[2]:.0f} -> {rss[-1]:.0f} MB)")


def s_slow_encoder_drops_frames(ctx):
    # Like a machine that can't encode HD at full rate: 100 ms per frame, about 10 fps of the 24 asked for.
    ctx.server = Server(ctx.dir)
    ctx.sniff = Sniffer(ctx.server.port)
    ctx.jt = Jamtaba(ctx.dir, marks="detail", extra_env={"JT_TEST_ENCODE_DELAY_MS": "100"})
    ctx.jt.cmd("video 2")
    ctx.jt.connect(ctx.server.port)
    ctx.jt.cmd("camera on")
    check(ctx.sniff.wait_intervals(USER, "H264", 2), "video running on a slow encoder")
    rss = []
    for _ in range(20):
        time.sleep(2)
        rss.append(ctx.jt.rss_mb())
    print(f"    RSS {rss[0]:.0f} -> {rss[-1]:.0f} MB")
    ctx.metrics.update(rss_start=round(rss[0]), rss_end=round(rss[-1]))
    check(rss[-1] < rss[0] + 150, f"frames don't pile up in memory ({rss[0]:.0f} -> {rss[-1]:.0f} MB)")
    done = [v for v in ctx.sniff.of(USER, "H264") if v["guid"] in ctx.sniff.ended]
    _, frames, _ = interval_stream(ctx, done[-1], "slow.h264")
    print(f"    {len(frames) / INTERVAL:.1f} fps")
    check(4 <= len(frames) / INTERVAL <= 14, f"video goes on at what the encoder can do ({len(frames) / INTERVAL:.1f} fps)")
    check("can't keep up" in ctx.jt.log(), "and the log says so")


def stress(ctx, remote_users):
    join(ctx, camera=False)
    ctx.jt.cmd("video 2")
    ctx.jt.cmd("camera on")
    peers = []
    for i in range(remote_users):
        peers.append(subprocess.Popen([os.path.join(PEERS, "nzpeer"), "127.0.0.1", str(ctx.server.port), f"hd{i}", ctx.hd_frames,
                                       ctx.hd_sps, "24", "1", "150"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT))
        ctx.cleanup.append(peers[-1].terminate)
    deadline = time.time() + 30
    while time.time() < deadline and len({b["user"] for b in ctx.sniff.begins if b["fourcc"] == "H264" and b["user"] != USER}) < remote_users:
        time.sleep(0.5)
    active = {b["user"] for b in ctx.sniff.begins if b["fourcc"] == "H264" and b["user"] != USER}
    check(len(active) == remote_users, f"{remote_users} remote users send HD ({len(active)})")
    samples, rss = [], []
    for _ in range(50):
        time.sleep(2)
        samples.append(cpu_percent(ctx.jt.proc.pid))
        rss.append(ctx.jt.rss_mb())
    check(ctx.jt.alive(), "Jamtaba survives")
    log = ctx.jt.log().replace('"', "")
    rows = [tuple(int(x) for x in m) for m in re.findall(r"JT_TEST video play match \S+ frames (\d+) previous (\d+) requested (\d+) decoded (\d+) tickgap (\d+)", log)]
    rows = [r for r in rows if r[1] > 0][remote_users * 2:]
    check(len(rows) >= remote_users * 3, f"it keeps playing every user ({len(rows)} intervals)")
    shown = sum(r[3] for r in rows) / max(1, sum(r[1] for r in rows))
    worst_gap = max(r[4] for r in rows)
    median_gap = statistics.median(r[4] for r in rows)
    audio = [b["t"] for b in ctx.sniff.of(USER, "OGGv", 0)]
    gaps = [y - x for x, y in zip(audio, audio[1:])]
    lost = max(0.0, sum(gaps) - INTERVAL * len(gaps))
    cpu = statistics.mean(samples[5:])
    print(f"    CPU avg {cpu:.0f}% max {max(samples):.0f}%, RSS {rss[5]:.0f} -> {rss[-1]:.0f} MB (max {max(rss):.0f})")
    print(f"    frames decoded {shown * 100:.0f}%, UI tick gap median {median_gap:.0f} ms, worst {worst_gap} ms")
    print(f"    audio clock lost {lost:.2f} s in {len(gaps)} intervals; off-nominal {[round(g - INTERVAL, 2) for g in gaps if abs(g - INTERVAL) > 0.15]}")
    ctx.metrics.update(cpu=round(cpu), rss=round(max(rss)), decoded_pct=round(shown * 100), tickgap_ms=worst_gap, audio_lost_s=round(lost, 2))
    check(not ctx.jt.errors(), f"no errors ({ctx.jt.errors()[:1]})")
    check("Error decoding video" not in log, f"no decoding errors ({log.count('Error decoding video')})")
    check(shown >= 0.9, "decodes at least 90% of the frames")
    check(lost <= 0.5, "the audio clock doesn't lose time")


def s_stress_six_hd(ctx):
    stress(ctx, 6)


SCENARIOS = [
    ("wire format", s_wire_format),
    ("camera before join", s_camera_before_join),
    ("camera after prepared", s_camera_after_prepared),
    ("receiver core", s_receiver_core),
    ("av sync", s_av_sync),
    ("legacy server", s_legacy_server),
    ("receives ninjamzap video", s_receives_ninjamzap_video),
    ("frame lengths like sps", s_frame_lengths_like_sps),
    ("jamtaba to jamtaba sync", s_jamtaba_to_jamtaba_sync),
    ("legacy jtbv receive", s_legacy_jtbv_receive),
    ("camera toggle", s_camera_toggle),
    ("bpm change", s_bpm_change),
    ("rejoin", s_rejoin),
    ("receiver stall", s_receiver_stall),
    ("transmit off", s_transmit_off),
    ("voice chat", s_voice_chat),
    ("voice chat sync", s_voice_chat_sync),
    ("video presets", s_video_presets),
    ("video preset change", s_video_preset_change),
    ("legacy server stays sd", s_legacy_server_stays_sd),
    ("audio presets", s_audio_presets),
    ("encoder keeps up hd", s_encoder_keeps_up_hd),
    ("slow encoder drops frames", s_slow_encoder_drops_frames),
    ("hd jamtaba to jamtaba", s_hd_jamtaba_to_jamtaba),
    ("receive video off", s_receive_video_off),
    ("stress six hd", s_stress_six_hd),
]


class Ctx:
    pass


def fixtures():
    fx = os.path.join(HERE, ".build", "fixtures")
    os.makedirs(fx, exist_ok=True)
    video, jtbv = os.path.join(fx, "video.h264"), os.path.join(fx, "jtbv.h264")
    if not os.path.exists(video):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=640x480:r=15", "-t", "8", "-c:v", "libx264",
                        "-preset", "veryfast", "-profile:v", "baseline", "-bf", "0", "-g", "120", "-b:v", "800k",
                        "-pix_fmt", "yuv420p", "-f", "h264", video], check=True)
    if not os.path.exists(jtbv):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=10", "-t", "8", "-c:v", "libx264",
                        "-preset", "veryfast", "-g", "30", "-b:v", "96k", "-maxrate", "96k", "-bufsize", "96k",
                        "-pix_fmt", "yuv420p", "-f", "h264", jtbv], check=True)
    data = open(video, "rb").read()
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    nals = [data[a + 3:(starts[i + 1] if i + 1 < len(starts) else len(data))].rstrip(b"\x00") for i, a in enumerate(starts)]
    sps = next(n for n in nals if n[0] & 31 == 7)
    pps = next(n for n in nals if n[0] & 31 == 8)
    inner = struct.pack(">H", len(sps)) + sps + struct.pack(">H", len(pps)) + pps
    spsf, frames = os.path.join(fx, "sps.bin"), os.path.join(fx, "frames.bin")
    with open(spsf, "wb") as f:
        f.write(struct.pack(">I", len(inner)) + inner)
    with open(frames, "wb") as f:
        for n in nals:
            if n[0] & 31 in (1, 5):
                f.write(struct.pack(">I", len(n) + 4) + struct.pack(">I", len(n)) + n)
    return frames, spsf, jtbv


def sps_lookalike_fixture():
    """Valid frames whose first NAL length (00 00 07 80) starts like a [2B SPS length][SPS] block."""
    fx = os.path.join(HERE, ".build", "fixtures")
    video = os.path.join(fx, "small.h264")
    if not os.path.exists(video):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=10", "-t", "8", "-c:v", "libx264",
                        "-preset", "veryfast", "-profile:v", "baseline", "-bf", "0", "-g", "80", "-b:v", "80k",
                        "-pix_fmt", "yuv420p", "-f", "h264", video], check=True)
    data = open(video, "rb").read()
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    nals = [data[a + 3:(starts[i + 1] if i + 1 < len(starts) else len(data))].rstrip(b"\x00") for i, a in enumerate(starts)]
    sps = next(n for n in nals if n[0] & 31 == 7)
    pps = next(n for n in nals if n[0] & 31 == 8)
    inner = struct.pack(">H", len(sps)) + sps + struct.pack(">H", len(pps)) + pps
    spsf, frames = os.path.join(fx, "small-sps.bin"), os.path.join(fx, "lookalike-frames.bin")
    with open(spsf, "wb") as f:
        f.write(struct.pack(">I", len(inner)) + inner)
    count = 0
    target = 0x0780
    with open(frames, "wb") as f:
        for n in nals:
            if n[0] & 31 not in (1, 5):
                continue
            if len(n) < target:  # one NAL, as an iPhone sends it; decoders ignore trailing zero words
                n = n + (b"\x00\x00\x03" * target)[:target - len(n)]
                count += 1
            frame = struct.pack(">I", len(n)) + n
            f.write(struct.pack(">I", len(frame)) + frame)
    assert count > 40, count
    return frames, spsf


def hd_fixture():
    """A camera-like HD stream at NinjamZap's HD preset: motion and noise, no B-frames, 24 fps."""
    fx = os.path.join(HERE, ".build", "fixtures")
    video = os.path.join(fx, "hd.h264")
    if not os.path.exists(video):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=1280x720:r=24", "-t", "8", "-vf", "noise=alls=12:allf=t",
                        "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "baseline", "-bf", "0", "-g", "192", "-b:v", "1200k",
                        "-maxrate", "1200k", "-bufsize", "1200k", "-pix_fmt", "yuv420p", "-f", "h264", video], check=True)
    data = open(video, "rb").read()
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    nals = [data[a + 3:(starts[i + 1] if i + 1 < len(starts) else len(data))].rstrip(b"\x00") for i, a in enumerate(starts)]
    sps = next(n for n in nals if n[0] & 31 == 7)
    pps = next(n for n in nals if n[0] & 31 == 8)
    inner = struct.pack(">H", len(sps)) + sps + struct.pack(">H", len(pps)) + pps
    spsf, frames = os.path.join(fx, "hd-sps.bin"), os.path.join(fx, "hd-frames.bin")
    with open(spsf, "wb") as f:
        f.write(struct.pack(">I", len(inner)) + inner)
    with open(frames, "wb") as f:
        for n in nals:
            if n[0] & 31 in (1, 5):
                f.write(struct.pack(">I", len(n) + 4) + struct.pack(">I", len(n)) + n)
    return frames, spsf


def main():
    words = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--list" in sys.argv:
        for name, _ in SCENARIOS:
            print(name)
        return 0
    chosen = [s for s in SCENARIOS if not words or any(w in s[0] for w in words)]
    frames, sps, jtbv = fixtures()
    hd_frames, hd_sps = hd_fixture()
    results = []
    for name, fn in chosen:
        ctx = Ctx()
        ctx.dir = os.path.join(OUT, name.replace(" ", "-"))
        os.makedirs(ctx.dir)
        ctx.frames, ctx.sps, ctx.jtbv = frames, sps, jtbv
        ctx.hd_frames, ctx.hd_sps = hd_frames, hd_sps
        ctx.server = ctx.sniff = ctx.jt = None
        ctx.cleanup, ctx.metrics = [], {}
        ctx.sync_limit_ms = int(os.environ.get("JT_SYNC_LIMIT_MS", "300"))
        print(f"== {name}", flush=True)
        t0 = time.time()
        try:
            fn(ctx)
            status = "PASS"
        except KnownGap as e:
            status = "GAP"
            print(f"    gap: {e}")
        except Failed:
            status = "FAIL"
        except Exception:
            traceback.print_exc()
            status = "FAIL"
        for f in ctx.cleanup:
            f()
        if ctx.sniff:
            ctx.sniff.stop()
            with open(os.path.join(ctx.dir, "wire.json"), "w") as f:
                json.dump(dict(begins=[dict(b, guid=b["guid"].hex()) for b in ctx.sniff.begins],
                               configs=ctx.sniff.configs,
                               channels={f"{k[0]}:{k[1]}": v for k, v in ctx.sniff.channels.items()}), f)
        if ctx.jt:
            ctx.jt.stop()
        if ctx.server:
            ctx.server.stop()
        results.append((name, status, time.time() - t0, ctx.metrics))
        print(f"   {status} ({time.time() - t0:.0f} s)\n", flush=True)
    print("== summary")
    for name, status, secs, metrics in results:
        print(f"{status:5} {name:28} {secs:5.0f} s {metrics or ''}")
    print(f"results in {OUT}")
    return 1 if any(s == "FAIL" for _, s, _, _ in results) else 0


if __name__ == "__main__":
    sys.exit(main())

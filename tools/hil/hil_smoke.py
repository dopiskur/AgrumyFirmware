#!/usr/bin/env python3
"""
Hardware-in-the-loop smoke test: one ESP32 on the bench, flashed with a release-style build,
then watched end to end against the real (test) API.

  build   -> pio run -e <env>                           (FIRMWARE_VERSION env or git describe, like release.yml)
  flash   -> pio run -e <env> -t upload --upload-port <port>
  boot    -> serial: "[Diag] post-boot" then a completed loop cycle, no crash/reset markers
  api     -> login, find the device by MAC, heartbeat after the flash, FirmwareVersion == built version
  data    -> a sensorData reading dated after the flash
  command -> POST /api/DeviceCommand ForceConfigSync, wait for Executed
  ota     -> upload this build to the catalog, arm it, ForceOTA, wait for the device to come back on it

  python tools/hil/hil_smoke.py --port COM6 --api https://api.agrumy.com --login admin@x --password ...
  (AGRUMY_HIL_LOGIN / AGRUMY_HIL_PASSWORD env vars work too; --skip-flash / --skip-ota narrow the run)

Exit 0 = every step passed, 1 = a step failed (the summary table says which), 2 = usage/setup problem.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    import requests
    import serial  # pyserial
except ImportError as ex:
    sys.exit(f"missing dependency ({ex.name}) - run: pip install -r tools/hil/requirements.txt")

REPO = Path(__file__).resolve().parents[2]
BAUD = 115200
CRASH_MARKERS = ("Guru Meditation", "abort() was called", "Backtrace:", "***ERROR*** A stack overflow", "rst:0x", "LoadProhibited", "StoreProhibited")

# CommandActionType / CommandTargetType / CommandStatus - api.Models.DeviceCommand.
ACTION_FORCE_OTA = 2
ACTION_FORCE_CONFIG_SYNC = 3
TARGET_DEVICE = 1
STATUS_EXECUTED = 2
STATUS_EXPIRED = 3


class SerialWatcher:
    """Reads the board's serial output on a thread so API polling and log matching run side by side."""

    def __init__(self, port: str, log_path: Path, reset_on_open: bool = False):
        self.port = port
        self.reset_on_open = reset_on_open
        self.lines: list[str] = []
        self.crash: str | None = None
        self._stop = threading.Event()
        self._log = open(log_path, "w", encoding="utf-8", errors="replace")
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)
        self._log.close()

    def _run(self) -> None:
        try:
            with serial.Serial(self.port, BAUD, timeout=1) as s:
                if self.reset_on_open:
                    # Same EN-pin pulse esptool ends a flash with, so a --skip-flash run still sees a fresh boot.
                    s.dtr = False
                    s.rts = True
                    time.sleep(0.1)
                    s.rts = False
                while not self._stop.is_set():
                    raw = s.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    self.lines.append(line)
                    self._log.write(line + "\n")
                    self._log.flush()
                    if self.crash is None and any(m in line for m in CRASH_MARKERS):
                        # "rst:0x1 (POWERON_RESET)" right after our own flash is the normal boot, not a crash.
                        if "rst:0x" in line and ("POWERON" in line or "SW_CPU_RESET" in line or "RTCWDT_RTC_RESET" in line):
                            continue
                        self.crash = line
        except serial.SerialException as ex:
            self.lines.append(f"<serial error: {ex}>")

    def wait_for(self, pattern: str, timeout: float, start_index: int = 0) -> int | None:
        """Index of the first line matching the regex at/after start_index, or None on timeout."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        i = start_index
        while time.monotonic() < deadline:
            while i < len(self.lines):
                if rx.search(self.lines[i]):
                    return i
                i += 1
            if self.crash:
                return None
            time.sleep(0.2)
        return None


class Api:
    def __init__(self, base: str, login: str, password: str):
        self.base = base.rstrip("/")
        self.s = requests.Session()
        r = self.s.post(f"{self.base}/api/User/Login", json={"login": login, "password": password}, timeout=30)
        r.raise_for_status()
        self.s.headers["Authorization"] = "Bearer " + r.json()["token"]

    def get(self, path: str, **params):
        r = self.s.get(f"{self.base}{path}", params=params or None, timeout=30)
        r.raise_for_status()
        return r.json() if r.content else None

    def post(self, path: str, body=None, **kwargs):
        r = self.s.post(f"{self.base}{path}", json=body, timeout=60, **kwargs)
        if r.status_code >= 400:
            raise RuntimeError(f"POST {path} -> {r.status_code}: {r.text[:300]}")
        return r.json() if r.content else None


def firmware_version() -> str:
    # Mirrors tools/firmware_version.py so the version we expect the board to report is the one the build embedded.
    if os.environ.get("FIRMWARE_VERSION"):
        return os.environ["FIRMWARE_VERSION"]
    try:
        return subprocess.check_output(["git", "describe", "--tags", "--always", "--dirty"], cwd=REPO, stderr=subprocess.DEVNULL).decode().strip().lstrip("v")
    except Exception:  # noqa: BLE001
        return "0.0.0-dev"


def pio(*args: str) -> str:
    exe = shutil.which("pio") or shutil.which("platformio")
    cmd = [exe, *args] if exe else [sys.executable, "-m", "platformio", *args] # penv python without pio on PATH
    print("+", " ".join(cmd), flush=True)
    out: list[str] = []
    with subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace") as proc:
        for line in proc.stdout:  # type: ignore[union-attr]
            print(line, end="", flush=True)
            out.append(line)
    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd)
    return "".join(out)


def parse_device_time(text: str | None) -> datetime | None:
    if not text:
        return None
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S"):
        try:
            return datetime.strptime(text[:19], fmt).replace(tzinfo=timezone.utc)
        except ValueError:
            continue
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


def wait_until(what: str, timeout: float, probe, interval: float = 10.0):
    deadline = time.monotonic() + timeout
    while True:
        result = probe()
        if result:
            return result
        if time.monotonic() >= deadline:
            raise TimeoutError(f"{what} did not happen within {int(timeout)}s")
        time.sleep(interval)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port the board is on, e.g. COM6 or /dev/ttyUSB0")
    ap.add_argument("--env", default="esp32dev", help="PlatformIO environment (default esp32dev)")
    ap.add_argument("--api", default=os.environ.get("AGRUMY_HIL_API", "https://api.agrumy.com"))
    ap.add_argument("--login", default=os.environ.get("AGRUMY_HIL_LOGIN"))
    ap.add_argument("--password", default=os.environ.get("AGRUMY_HIL_PASSWORD"))
    ap.add_argument("--mac", help="device MAC (12 hex chars) if the boot log doesn't reveal it")
    ap.add_argument("--skip-flash", action="store_true", help="reuse whatever is already running on the board")
    ap.add_argument("--skip-ota", action="store_true")
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--cycle-timeout", type=int, default=300, help="heartbeat/telemetry/command waits (>= 3 x sleepSeconds)")
    ap.add_argument("--ota-timeout", type=int, default=600)
    ap.add_argument("--log-dir", default=str(REPO / ".pio" / "hil"))
    args = ap.parse_args()
    if not args.login or not args.password:
        print("API login/password required (--login/--password or AGRUMY_HIL_LOGIN/AGRUMY_HIL_PASSWORD)", file=sys.stderr)
        return 2

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    version = firmware_version()
    results: list[tuple[str, str, str]] = []
    failed = False

    def record(step: str, ok: bool, detail: str = "") -> None:
        nonlocal failed
        failed = failed or not ok
        results.append((step, "PASS" if ok else "FAIL", detail))
        print(f"[{'PASS' if ok else 'FAIL'}] {step}{': ' + detail if detail else ''}", flush=True)

    # ---- build + flash -------------------------------------------------------------------------
    if not args.skip_flash:
        try:
            os.environ.setdefault("FIRMWARE_VERSION", version)
            pio("run", "-e", args.env)
            upload = pio("run", "-e", args.env, "-t", "upload", "--upload-port", args.port)
            m = re.search(r"MAC:\s*((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})", upload)  # esptool prints the chip MAC while flashing
            flashed_mac = m.group(1).replace(":", "").upper() if m else None
            record("build+flash", True, f"{args.env} v{version} -> {args.port}" + (f" (MAC {flashed_mac})" if flashed_mac else ""))
        except subprocess.CalledProcessError as ex:
            record("build+flash", False, f"exit {ex.returncode}")
            return finish(results)
    else:
        flashed_mac = None
    flash_time = datetime.now(timezone.utc)

    # ---- boot --------------------------------------------------------------------------------------
    watcher = SerialWatcher(args.port, log_dir / "serial.log", reset_on_open=args.skip_flash)
    watcher.start()
    try:
        boot = watcher.wait_for(r"\[Diag\] post-boot", args.boot_timeout)
        if boot is None:
            record("boot", False, watcher.crash or "no '[Diag] post-boot' line - see serial.log")
            return finish(results)
        cycle = watcher.wait_for(r"\[Loop\]-----> END", args.cycle_timeout, boot)
        if cycle is None or watcher.crash:
            record("boot", False, watcher.crash or "first loop cycle never completed")
            return finish(results)
        record("boot", True, f"post-boot + one full cycle, {len(watcher.lines)} serial lines")

        mac = (args.mac or flashed_mac or "").upper()
        boot_device_id = None
        for line in watcher.lines:
            m = re.search(r"Agrumy_([0-9A-Fa-f]{12})", line)
            if m and not mac:
                mac = m.group(1).upper()
            m = re.search(r'"deviceID":(\d+)', line)  # the config the board loaded at boot names its own id
            if m and boot_device_id is None:
                boot_device_id = int(m.group(1))
        if not mac and boot_device_id is None:
            record("device lookup", False, "neither a MAC nor a deviceID appeared (flash output / boot log) - pass --mac")
            return finish(results)

        # ---- api: device, heartbeat, version -----------------------------------------------------
        try:
            api = Api(args.api, args.login, args.password)
        except Exception as ex:  # noqa: BLE001
            record("api login", False, str(ex))
            return finish(results)
        devices = api.get("/api/Device/All") or []
        device = next((d for d in devices if (mac and (d.get("macAddress") or "").upper() == mac) or (boot_device_id is not None and d.get("idDevice") == boot_device_id)), None)
        if device is None:
            record("device lookup", False, f"MAC {mac or '?'} / deviceID {boot_device_id} not registered on {args.api}")
            return finish(results)
        id_device = device["idDevice"]
        record("device lookup", True, f"idDevice={id_device} ({device.get('deviceName')})")

        def heartbeat_ok():
            st = api.get("/api/Device/FleetStatus", idDevice=id_device) or {}
            seen = parse_device_time(st.get("lastSeenAt"))
            return st if seen and seen >= flash_time and st.get("firmwareVersion") == version else None
        try:
            st = wait_until("heartbeat with the built version", args.cycle_timeout, heartbeat_ok)
            record("heartbeat", True, f"lastSeenAt={st.get('lastSeenAt')} firmwareVersion={st.get('firmwareVersion')}")
        except TimeoutError as ex:
            st = api.get("/api/Device/FleetStatus", idDevice=id_device) or {}
            record("heartbeat", False, f"{ex}; server sees firmwareVersion={st.get('firmwareVersion')} lastSeenAt={st.get('lastSeenAt')}, expected v{version}")

        # ---- telemetry -----------------------------------------------------------------------------
        def telemetry_ok():
            raw = api.get("/api/SensorData", deviceID=id_device, timeRange=30) or {}
            rows = json.loads(raw) if isinstance(raw, str) else raw
            rows = rows.get("sensorData", []) if isinstance(rows, dict) else rows  # {"sensorData":[...]} envelope
            fresh = [r for r in rows if (parse_device_time(r.get("dateCreated")) or datetime.min.replace(tzinfo=timezone.utc)) >= flash_time]
            return fresh or None
        try:
            fresh = wait_until("a sensorData reading after the flash", args.cycle_timeout, telemetry_ok)
            record("telemetry", True, f"{len(fresh)} reading(s) since flash, newest {fresh[-1].get('dateCreated')}")
        except TimeoutError as ex:
            record("telemetry", False, str(ex))

        # ---- command round-trip --------------------------------------------------------------------
        def issue(action: int) -> int:
            ids = api.post("/api/DeviceCommand", {"targetType": TARGET_DEVICE, "targetId": id_device, "actionType": action})
            return int(ids[0]) if isinstance(ids, list) else int(ids)

        def executed(command_id: int):
            cmd = api.get(f"/api/DeviceCommand/{command_id}") or {}
            status = cmd.get("status")
            if status == STATUS_EXPIRED:
                raise RuntimeError(f"command {command_id} expired unexecuted")
            return cmd if status == STATUS_EXECUTED else None
        try:
            cid = issue(ACTION_FORCE_CONFIG_SYNC)
            cmd = wait_until(f"ForceConfigSync command {cid} executed", args.cycle_timeout, lambda: executed(cid))
            record("command round-trip", True, f"command {cid} executedAt={cmd.get('executedAt')}")
        except Exception as ex:  # noqa: BLE001
            record("command round-trip", False, str(ex))

        # ---- OTA to the same version ---------------------------------------------------------------
        if not args.skip_ota:
            try:
                bin_path = REPO / ".pio" / "build" / args.env / "firmware.bin"
                name = f"agrumy-{args.env}-v{version}.bin"
                with open(bin_path, "rb") as f:
                    api.post("/api/Firmware/Upload", files={"file": (name, f, "application/octet-stream")})
                api.post("/api/Device/FirmwareUpdate", {"idDevice": id_device, "version": version})
                ota_start = datetime.now(timezone.utc)
                serial_mark = len(watcher.lines)
                cid = issue(ACTION_FORCE_OTA)
                wait_until(f"ForceOTA command {cid} executed", args.ota_timeout, lambda: executed(cid))
                if watcher.wait_for(r"no firmware build available to force", 5, serial_mark) is not None:
                    raise RuntimeError("device got the ForceOTA without a firmwareUrl - the server confirmed the same-version offer on the heartbeat before the command was delivered (needs a server with the pending-ForceOTA guard in GetConfig)")
                reboot = watcher.wait_for(r"\[Diag\] post-boot", args.ota_timeout, serial_mark)
                if reboot is None:
                    raise RuntimeError(watcher.crash or "device never rebooted into the OTA image")

                def back_after_ota():
                    st = api.get("/api/Device/FleetStatus", idDevice=id_device) or {}
                    seen = parse_device_time(st.get("lastSeenAt"))
                    return st if seen and seen >= ota_start and st.get("firmwareVersion") == version else None
                st = wait_until("post-OTA heartbeat", args.cycle_timeout, back_after_ota)
                record("ota same version", True, f"command {cid}, back on v{st.get('firmwareVersion')} at {st.get('lastSeenAt')}")
            except Exception as ex:  # noqa: BLE001
                record("ota same version", False, str(ex))
        if watcher.crash:
            record("no crash during run", False, watcher.crash)
        else:
            record("no crash during run", True)
    finally:
        watcher.stop()
    return finish(results)


def finish(results: list[tuple[str, str, str]]) -> int:
    width = max(len(r[0]) for r in results) if results else 10
    print("\n" + "=" * 60)
    for step, status, detail in results:
        print(f"{step.ljust(width)}  {status}  {detail}")
    print("=" * 60)
    return 1 if any(r[1] == "FAIL" for r in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())

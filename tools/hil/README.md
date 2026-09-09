# HIL smoke test (`tools/hil/`)

One ESP32 on the bench, flashed with a release-style build and watched end to end against
the test API. Catches the class of failure nothing else here can: it compiles, every native
test passes, and the board still never boots, never registers, or dies on its first OTA.

```
python tools/hil/hil_smoke.py --port COM6 --api https://api.agrumy.com --login <global admin> --password ...
```

| Step | What must happen | Proves |
|---|---|---|
| build+flash | `pio run -e <env>` and `-t upload` succeed (version = `FIRMWARE_VERSION` env or `git describe`, like release.yml) | toolchain + bootloader path |
| boot | serial shows `[Diag] post-boot` and one `[Loop]-----> END` with no crash marker | the image runs at all |
| device lookup | the `Agrumy_<MAC>` from the boot log exists in `GET /api/Device/All` | the bench board is registered |
| heartbeat | `FleetStatus.lastSeenAt` after the flash and `firmwareVersion` == built version | config poll + version reporting |
| telemetry | a `GET /api/SensorData` reading dated after the flash | sensor read + push path |
| command round-trip | `POST /api/DeviceCommand` ForceConfigSync reaches `Executed` | pendingCommand delivery, inbox gate, ack |
| ota same version | upload this build to the catalog, arm it, ForceOTA, device reboots and heartbeats again on the same version | OTA download, SHA-256 check, flash, rollback guard |
| no crash during run | no `Guru Meditation`/`abort()`/`Backtrace`/stack-overflow line at any point | |

The OTA step needs an API built from AgrumyService `5baf10f` or later - before that, a
heartbeat reporting the target version cleared the offer before the ForceOTA could use it,
and the device logged "no firmware build available to force" (the script names that case).
The API's firmware store directory must be writable by the service user (`www-data` on
api.agrumy.com), or the catalog upload answers 500.

`--skip-flash` reuses what is already on the board (pulsing EN so it still boots fresh), `--skip-ota` drops the last step, and
the timeouts (`--boot-timeout`, `--cycle-timeout`, `--ota-timeout`) default to comfortably
more than three poll cycles at the default 60 s `sleepSeconds`. The full serial capture
lands in `.pio/hil/serial.log`.

The board has to be registered once by hand (captive portal or the web flasher) - the
test deliberately never registers or hard-resets anything, so the same device can be
reused for every run and its LittleFS state (registration, last processed command id)
carries over exactly like a field device's would.

## CI: `.github/workflows/hil-smoke.yml`

Runs after every successful "Firmware release" workflow and on `workflow_dispatch`, on a
self-hosted runner labelled `agrumy-hil`. One-time runner setup on the bench PC:

1. Plug the ESP32 in, note its port (`pio device list`).
2. Install Python 3 + `pip install -r tools/hil/requirements.txt` (PlatformIO included).
3. Register a GitHub Actions self-hosted runner for the AgrumyFirmware repo with the extra
   label `agrumy-hil` (Settings > Actions > Runners > New self-hosted runner), run it as
   a service so it survives reboots.
4. Repository variables `AGRUMY_HIL_PORT` (e.g. `COM6`) and `AGRUMY_HIL_API`
   (`https://api.agrumy.com` - the test/alfa server, there is no production), repository
   secrets `AGRUMY_HIL_LOGIN` / `AGRUMY_HIL_PASSWORD` for a Global admin account (firmware
   upload needs that role).

The `hil-bench` concurrency group serialises runs, so two releases in quick succession
queue instead of fighting over the one board.

# AgrumyFirmware

ESP32 firmware for the Agrumy greenhouse monitoring and control system.
This is the device side of the ecosystem; the backend API and admin UI live
in the separate [AgrumyService](https://github.com/dopiskur/AgrumyService) repository.

> Agrumy core (API, firmware, enclosures) is free and open source under the
> [Apache 2.0 license](LICENSE.txt); the mobile apps are under AGPL-3.0.
> If you use Agrumy, I'd genuinely love to hear about it — open an issue or
> drop me a line.

## Supported hardware

Built with PlatformIO. Ten environments (plus `native`, host-only, see Tests below):

| Environment | Board | Role |
| --- | --- | --- |
| `esp32dev` | ESP32-WROOM-32 dev board | Controller (relays + sensors) |
| `esp32s3usbotg` | ESP32-S3 | Controller |
| `kc868-a6` | ESP32-WROOM-32 (KC868-A6 kit) | Controller - six relays behind a PCF8574 I2C expander. Also carries an onboard SX1278 socket wired up as an optional LoRa Gateway relay (`LoRaGatewayRelayController`, `deviceConfig.loRaGatewayEnabled`) - a dual-role toggle on top of its normal WiFi/relay/sensor job, not a separate profile |
| `heltec-v3` | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V3) | Controller (relays + sensors) - the standard WiFi/HTTP build on Heltec LoRa hardware, not a LoRa transport profile itself |
| `heltec-v4` | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V4, no official PlatformIO board yet - reuses V3's board id) + external GPS module | Controller - same job as `heltec-v3` plus GPS-based device location (TinyGPSPlus over UART, `Controller/GpsController`), reported as `Latitude`/`Longitude` in the config-poll heartbeat |
| `esp32-s3-relay-6ch` | ESP32-S3 (Waveshare ESP32-S3-Relay-6CH kit) | Controller - six relays on direct GPIO |
| `esp32-lora` | ESP32-WROOM-32 + SX1276 (TTGO LoRa32 V2.1) | Profile B - no WiFi/HTTP, a separate setup()/loop() branch entirely |
| `esp32-lora-private` | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V3) | LoRa private-protocol sensor node (RadioLib raw PHY, no LoRaWAN/ChirpStack) - alternative to `esp32-lora`, paired with a Gateway running `GatewayProfile.LoRaPrivateProtocol`; each boot derives its own AES-128-GCM session key (HKDF from a fresh random bootNonce) instead of persisting an uplink counter to flash, so replay protection survives power loss with zero flash writes per uplink; a real two-board uplink (this node transmitting, `esp32-lora-gateway-bridge` receiving/decoding) is confirmed working over the air on hardware, downlink (Gateway -> bridge -> node) not yet exercised end to end |
| `esp32-lora-gateway-bridge` | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V3) | The LoRa private-protocol Gateway's own mains-powered radio-frontend board - bridges RadioLib frames to/from Agrumy.Gateway over USB serial; a real two-board uplink is confirmed working over the air (this board receiving/decoding an `esp32-lora-private` node's reading and forwarding it), downlink not yet exercised end to end |
| `heltec-lora-gateway` | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V3) | Standalone LoRa Gateway - a minimal WiFi-only device with no relays/sensors of its own, relaying nearby LoRa-only nodes' private-protocol uplinks over its own WiFi/HTTP connection (`LoRaGatewayRelayController`), no serial-attached bridge board or separate `Agrumy.Gateway` process needed |

Sensor readings include a `Battery` percentage for devices running on
battery - either a MAX17048 fuel gauge (I2C coulomb counting, recommended for
precision) or a plain resistor-divider `analogRead` against a piecewise-linear LiPo
voltage curve (`Logic/BatteryLogic.h`, necessity/fallback path). Relay functions
(ventilation, heating, water pump, lighting) each hold one or more rules, any of
which turning "on" wins (OR); within one rule, up to `MAX_CONDITIONS_PER_RULE` (8)
Threshold/Interval/Schedule conditions fold strictly left-to-right by AND/OR
(`RelayLogic::foldConditions`, `ActuatorController::evaluateRule`),
never nested or precedence-based. Several windows a day for the same function
are several Schedule-type rules/conditions, OR'd together like any other pair -
not a fixed per-function slot count. An admin can also override a relay's
rule-driven state directly with a Manual Actuate command (duration-based, or
until a target sensor value is reached) - `ManualOverride`/`ManualOverrideMode`
in `Model/DeviceModel.h`, applied in `ActuatorController::findManualOverride`
ahead of the normal rule evaluation.

Six relay functions exist (Ventilation, Light, Heating, Water pump, Screen,
Vent - the last two positional/percent-driven, e.g. a greenhouse screen or vent
flap, rather than plain on/off); every function folds through the same 0-100
`targetPercent`/MAX engine regardless of kind, so a positional function needs no
separate code path. Each physically-wired output slot (`RelaySlot`) declares one
of six `outputKind`s (`Model/DeviceModel.h`): plain Relay, RelayPair (two relays
driving an open/close motor, with a configurable travel time and a mandatory
dead-time pause on direction reversal), Pwm, Analog 0-10V (native ESP32 DAC,
classic ESP32 only, not S3), Servo, or LatchingPulse (a brief pulse on a coil
only on a 0/not-0 transition) - plus optional per-slot rate limiting and
min-on/min-off timing, all applied in `RelayLogic.h`/`ActuatorController`. A
relay function can also run in PID control mode instead of the ordinary
Threshold rule fold - `RelayLogic::pidCompute` derives `targetPercent` from a
setpoint/reading with configurable reverse-acting and derivative-on-measurement
behavior, still subject to the same rate-limit/min-on-off/outputKind dispatch
below it; a plain-Relay PID output can additionally time-proportion
(`computeTimeProportioningState`) to turn a percent into an on/off duty cycle.

## Offline resilience

A controller/sensor node keeps running its relay logic (thresholds, schedules)
against its own last-known-good `config.json`, loaded from local flash
(LittleFS) early in `setup()` before any network call - the server or the
internet being unreachable doesn't stop it from doing its job.

## Build

```
pio run -e esp32dev
```

The firmware ↔ API contract (JSON Schema) is generated in `AgrumyService` and
pulled into this repo's `contracts/device-api/` copy via
`tools/contract-check/sync_schemas.py`. CI (`contract-check.yml`) checks the
firmware's field lists against both that committed copy and a fresh pull from
`AgrumyService` master, so an upstream contract change surfaces here before
anyone remembers to resync.

## Tests

Twenty-five native suites (`test/test_native_*`) pull the platform-independent
logic out into plain C++ (no `Arduino.h`, no `digitalWrite`/`analogRead`) so it
can run as Unity tests on the host, without an ESP32 or any hardware attached:
relay-decision math (interval/schedule/threshold/comparison-operator,
`RelayLogic.*`), the AND/OR condition fold (`RelayLogic::foldConditions`) and
its rule-tree size limits, the outputKind dispatch primitives (rate limiting,
min-on-time, time-proportioning, PWM/servo/analog output mapping, RelayPair
motor step, PID controller) and the positional (Screen/Vent) percent fold,
battery-voltage/percentage conversion (`BatteryLogic.*`), safety limits,
discovery logic, sleep-schedule logic, manual-override evaluation, per-driver
sensor warm-up timing, config parse/apply logic, command-replay/dedup logic,
HTTP date parsing, network-request logic, and the LoRa-specific interval/
payload/private-frame/private-session/serial-frame logic each LoRa environment
needs. The threshold suite runs against
`test/test_native_threshold_logic/threshold_vectors.csv`, a fixture shared
verbatim with `AgrumyService`'s own test project so both repos agree on the
same inputs/outputs for the same evaluator:

```
pio test -e native
```

`build.yml` runs this on every push/PR as the fast regression brake, ahead of the
on-device builds. Every hardware environment excludes these native suites
(`test_ignore = test_native_*`) and `native` excludes any future
`test_embedded_*` suite the other way - no on-device tests exist yet, but the split
is already in place for when they do.

### Hardware-in-the-loop smoke test

`tools/hil/hil_smoke.py` flashes one bench ESP32 with a release-style build
and drives it end to end against the test API - boot, device-registration
lookup, heartbeat, a telemetry reading, a command round-trip, and a
same-version ForceOTA - catching the "compiles but doesn't boot" class no
native test can see:

```
python tools/hil/hil_smoke.py --port COM6 --api https://api.agrumy.com --login <global admin> --password ...
```

`.github/workflows/hil-smoke.yml` runs it on a self-hosted runner after every
release tag and on demand; see `tools/hil/README.md` for one-time runner setup.

### Version and board

The version string the device reports (and compares OTA offers against) is
**derived at build time**, never edited by hand:

1. `FIRMWARE_VERSION` environment variable (what the release workflow sets from the tag),
2. else `git describe --tags --always --dirty` (a dev build, e.g. `1.2.3-4-gabc1234-dirty`),
3. else `0.0.0-dev`.

See `tools/firmware_version.py` (a PlatformIO `extra_scripts` pre-script). Each
environment also carries `-D AGRUMY_BOARD="<env name>"`; the device sends that as
`Board` in its config-poll heartbeat, and it is the `<board>` in the release file
name below - so the API knows which `.bin` fits which hardware. A relay-kit
environment (`kc868-a6`, `esp32-s3-relay-6ch`) additionally carries `-D
AGRUMY_KIT="<kit name>"`, sent as a separate `Kit` field in the same heartbeat -
`Board` picks the OTA binary, `Kit` tells the server which relay-hardware
capability to look up (auto-registered into the kit catalog on first sight).

## Releases

A release is produced only by `.github/workflows/release.yml`, triggered by a semver tag:

```
git tag v1.2.3
git push origin v1.2.3
```

Today its build matrix only covers `esp32dev` and `esp32s3usbotg` - every
other environment still needs a manual `pio run -e <env>` build until it's
added to the matrix (the merge-bin step errors out for any environment it
doesn't have bootloader/partition offsets for yet). Built images are named by
the convention the API enforces on every import path -

```
agrumy-<board>-v<version>.bin     e.g. agrumy-esp32dev-v1.2.3.bin
```

- and published as a GitHub Release with those files plus `manifest.json`
(SHA-256 per file) and `SHA256SUMS.txt`. The AgrumyService admin UI (Firmware page)
reads these releases directly (default **GitHub** source), can pull them into its own
**Local** repository (needed for air-gapped installs or a pinned self-signed API
certificate), or can point at a **Custom** repository serving the same
`manifest.json` format.

### Offline USB repository

For a greenhouse server with no internet: on an online machine, either use the
**Build offline repo** button on the Firmware page (Chrome/Edge/Opera, HTTPS), or
run one of the scripts in `tools/offline-repo/`:

```
tools/offline-repo/prepare-offline-repo.sh /media/usb/agrumy-firmware
.\tools\offline-repo\prepare-offline-repo.ps1 -Target E:\agrumy-firmware
```

Both download the release `.bin` files and write a `manifest.json` with SHA-256
checksums. Plug the stick into the offline server and use **Import from a directory
on the server** on the Firmware page - the import verifies every checksum before a
file enters the catalog.

## License

Copyright 2016-2026 Domagoj Piškur

Licensed under the Apache License, Version 2.0 (the "License"); you may not
use this project except in compliance with the License. You may obtain a copy
of the License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.

The Android and iOS applications (AgrumyAndroid, AgrumyiOS) are separate
projects licensed under AGPL-3.0, not this Apache 2.0 license.

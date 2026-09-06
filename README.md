# AgrumyFirmware

ESP32 firmware for the Agrumy greenhouse monitoring and control system.
This is the device side of the ecosystem; the backend API and admin UI live
in the separate [AgrumyService](https://github.com/dopiskur/AgrumyService) repository.

> Agrumy core (API, firmware, enclosures) is free and open source under the
> [Apache 2.0 license](LICENSE.txt); the mobile apps are under AGPL-3.0.
> If you use Agrumy, I'd genuinely love to hear about it — open an issue or
> drop me a line.

## Supported hardware

Built with PlatformIO. Eight environments (plus `native`, host-only, see Tests below):

| Environment | Board | Role |
| --- | --- | --- |
| `esp32dev` | ESP32-WROOM-32 dev board | Controller (relays + sensors) |
| `esp32s3usbotg` | ESP32-S3 | Controller |
| `kc868-a6` | ESP32-WROOM-32 (KC868-A6 kit) | Controller - six relays behind a PCF8574 I2C expander, not physically verified. Also carries an onboard SX1278 socket wired up as an optional LoRa Gateway relay (`LoRaGatewayRelayController`, `deviceConfig.loRaGatewayEnabled`) - a dual-role toggle on top of its normal WiFi/relay/sensor job, not a separate profile |
| `esp32-s3-relay-6ch` | ESP32-S3 (Waveshare ESP32-S3-Relay-6CH kit) | Controller - six relays on direct GPIO, not physically verified |
| `esp32-lora` | ESP32-WROOM-32 + SX1276 (TTGO LoRa32 V2.1) | Profile B - no WiFi/HTTP, a separate setup()/loop() branch entirely, pin mapping and join/uplink cycle not verified against real hardware |
| `esp32-lora-private` | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V3) | LoRa private-protocol sensor node (RadioLib raw PHY, no LoRaWAN/ChirpStack) - alternative to `esp32-lora`, paired with a Gateway running `GatewayProfile.LoRaPrivateProtocol`; pin mapping + single-board cycle confirmed on real hardware, two-radio RF exchange not yet tested |
| `esp32-lora-gateway-bridge` | ESP32-S3 + SX1262 (Heltec WiFi LoRa 32 V3) | The LoRa private-protocol Gateway's own mains-powered radio-frontend board - bridges RadioLib frames to/from Agrumy.Gateway over USB serial; radio init + real serial link to Agrumy.Gateway confirmed on hardware, real over-the-air traffic not yet tested |
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

## Offline resilience

A controller/sensor node keeps running its relay logic (thresholds, schedules)
against its own last-known-good `config.json`, loaded from local flash
(LittleFS) early in `setup()` before any network call - the server or the
internet being unreachable doesn't stop it from doing its job.

## Build

```
pio run -e esp32dev
```

The firmware ↔ API contract is defined in `contracts/device-api/` (JSON
Schema) and enforced in CI on both repositories.

## Tests

Thirteen native suites (`test/test_native_*`) pull the platform-independent
logic out into plain C++ (no `Arduino.h`, no `digitalWrite`/`analogRead`) so it
can run as Unity tests on the host, without an ESP32 or any hardware attached:
relay-decision math (interval/schedule/threshold, `RelayLogic.*`), the AND/OR
condition fold (`RelayLogic::foldConditions`), battery-voltage/percentage
conversion (`BatteryLogic.*`), safety limits, discovery logic, sleep-schedule
logic, manual-override evaluation, and the LoRa-specific interval/payload/
private-frame/serial-frame logic each LoRa environment needs. The threshold
suite runs against `test/test_native_threshold_logic/threshold_vectors.csv`, a
fixture shared verbatim with `AgrumyService`'s own test project so both repos
agree on the same inputs/outputs for the same evaluator:

```
pio test -e native
```

`build.yml` runs this on every push/PR as the fast regression brake, ahead of the
on-device builds. Every hardware environment excludes these native suites
(`test_ignore = test_native_*`) and `native` excludes any future
`test_embedded_*` suite the other way - no on-device tests exist yet, but the split
is already in place for when they do.

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

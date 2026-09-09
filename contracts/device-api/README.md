# Device <-> API contract schemas (copied)

These 7 JSON Schema files are a **copy** of the source-of-truth in the API repo:

> **Source:** `dopiskur/AgrumyService`, path `contracts/device-api/`
> **Copied from commit:** `e5bf3ce` (`master`) - config.request.schema.json re-synced for the optional heap/stack diagnostic fields (MinFreeHeap, MaxAllocHeap, StackHighWaterMark, NetworkStackHighWaterMark, ConfigSchemaVersion); the other files were already current

We copy rather than submodule because the schemas change rarely (only when the
firmware<->API payloads change) and a submodule would add init/update friction to
every PlatformIO checkout and CI run. The trade-off is that this copy must be
**re-synced by hand** when the API-side schemas change.

## What enforces them here

`tools/contract-check/` - a hand-maintained list of the JSON keys this firmware
sends/expects for each endpoint (`firmware_fields.py`), checked against these
schemas by `check_contract.py`. Run in CI by `.github/workflows/contract-check.yml`.

This is the minimal stand-in until real firmware unit tests exist (roadmap #19).
It does **not** parse the C++ - it relies on the field list being kept in sync,
which is why `firmware_fields.py` has a loud "update me" header.

## Re-syncing after an API-side contract change

1. Copy the changed `*.schema.json` file(s) from `AgrumyService/contracts/device-api/`
   into this folder.
2. Update the **commit hash** line above.
3. Update the field list(s) in `tools/contract-check/firmware_fields.py` and, if the
   firmware's real payload/parsing changed, `src/Controller/*.cpp` / `src/Model/DeviceModel.h`.
4. Run `python tools/contract-check/check_contract.py` locally - it must pass.

See `AgrumyService/contracts/device-api/README.md` for the full description of each schema.

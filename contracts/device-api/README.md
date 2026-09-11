# Device <-> API contract schemas (copied)

These JSON Schema files are a **copy** of the source-of-truth in the API repo:

> **Source:** `dopiskur/AgrumyService`, path `contracts/device-api/`
> **Copied from commit:** `93480c2` (`master`) - synced by tools/contract-check/sync_schemas.py

Upstream, every file except `authenticate.request.schema.json` is generated from the C#
DTOs by `tools/Agrumy.ContractGen` (see that repo's `contracts/device-api/README.md`), so a
DTO change there is a diff in these files, not a surprise on a device.

We copy rather than submodule because the schemas change rarely (only when the
firmware<->API payloads change) and a submodule would add init/update friction to
every PlatformIO checkout and CI run. The copy is re-synced with one command:

```
python tools/contract-check/sync_schemas.py            # GitHub master -> this folder, README line above updated
python tools/contract-check/sync_schemas.py --ref <sha>
python tools/contract-check/sync_schemas.py --from ../AgrumyService/contracts/device-api
```

## What enforces them here

`tools/contract-check/` - a hand-maintained list of the JSON keys this firmware
sends/expects for each endpoint (`firmware_fields.py`), checked against these
schemas by `check_contract.py`. `.github/workflows/contract-check.yml` runs it twice:
against this committed copy, and against a fresh download of AgrumyService `master`
(`--schema-dir`), with a warning when the copy is behind upstream.

It does **not** parse the C++ - it relies on the field list being kept in sync,
which is why `firmware_fields.py` has a loud "update me" header.

## Re-syncing after an API-side contract change

1. `python tools/contract-check/sync_schemas.py`
2. Update the field list(s) in `tools/contract-check/firmware_fields.py` and, if the
   firmware's real payload/parsing changed, `src/Controller/*.cpp` / `src/Model/DeviceModel.h`.
3. Run `python tools/contract-check/check_contract.py` locally - it must pass.

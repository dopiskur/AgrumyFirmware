#!/usr/bin/env python3
"""
Firmware <-> API contract check (roadmap #5).

Validates the hand-maintained key lists in firmware_fields.py against the JSON
Schemas in ../../contracts/device-api/ (a copy of the AgrumyService source of truth).

It catches the drift that a C++ compile will not:
  * the API renames/removes a field the firmware reads     -> reads_subset fails
  * the firmware starts sending a field not in the contract -> sends_exact fails
  * the API adds a required field the firmware never sends  -> sends_exact fails
  * a schema file goes missing / stops being valid          -> load fails

Exit code 0 = contract holds, 1 = mismatch (details printed).
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

try:
    from jsonschema.validators import Draft7Validator
except ImportError:
    sys.exit("jsonschema not installed - run:  pip install -r tools/contract-check/requirements.txt")

HERE = Path(__file__).resolve().parent
# --schema-dir <path> checks the same field lists against another copy of the schemas (CI points it at a fresh download from AgrumyService master).
SCHEMA_DIR = Path(sys.argv[sys.argv.index("--schema-dir") + 1]) if "--schema-dir" in sys.argv else HERE.parent.parent / "contracts" / "device-api"

sys.path.insert(0, str(HERE))
from firmware_fields import CONTRACT  # noqa: E402

problems: list[str] = []


def fail(schema_name: str, msg: str) -> None:
    problems.append(f"[{schema_name}] {msg}")


def load_schema(name: str) -> dict:
    return json.loads((SCHEMA_DIR / name).read_text(encoding="utf-8"))


def dummy_for(prop_schema: dict):
    """A minimal value that satisfies a property sub-schema (types are lists like ["integer","null"])."""
    types = prop_schema.get("type", "string")
    if isinstance(types, str):
        types = [types]
    if "enum" in prop_schema:
        return prop_schema["enum"][0]
    if "pattern" in prop_schema:
        # only pattern in the contract is ^[0-9]+$
        return "1" if re.fullmatch(prop_schema["pattern"], "1") else "x"
    # A generated request schema lists "string" next to a number type (server accepts numeric strings) - the number is the real wire form.
    if "integer" in types:
        return 1
    if "number" in types:
        return 1.0
    if "string" in types:
        return "x"
    if "boolean" in types:
        return True
    if "array" in types:
        return []
    if "object" in types:
        return {}
    if "null" in types:
        return None
    return "x"


def build_object(obj_schema: dict, keys: list[str]) -> dict:
    props = obj_schema.get("properties", {})
    out = {}
    for k in keys:
        ps = props.get(k, {"type": "string"})
        # resolve a trivial oneOf(null, {...}) to its non-null branch for dummy purposes
        if "oneOf" in ps:
            branch = next((b for b in ps["oneOf"] if b.get("type") != "null"), ps["oneOf"][0])
            out[k] = {} if branch.get("$ref") or branch.get("type") == "object" else dummy_for(branch)
        else:
            out[k] = dummy_for(ps)
    return out


def check_sends_exact(name: str, spec: dict, schema: dict, item_level: bool) -> None:
    target = schema["items"] if item_level else schema
    required = set(target.get("required", []))
    props = set(target.get("properties", {}))
    fw = set(spec["keys"])

    if fw != required:
        fail(name, f"firmware sends {sorted(fw)} but schema.required is {sorted(required)}"
                   f"  (missing: {sorted(required - fw)}, extra: {sorted(fw - required)})")
    if not fw <= props:
        fail(name, f"firmware sends keys not defined in schema.properties: {sorted(fw - props)}")

    payload = build_object(target, spec["keys"])
    instance = [payload] if item_level else payload
    for e in Draft7Validator(schema).iter_errors(instance):
        fail(name, f"synthetic firmware payload rejected by schema: {list(e.path)} -> {e.message}")


def check_empty_body(name: str, schema: dict) -> None:
    v = Draft7Validator(schema)
    for candidate in (None, {}):
        errs = list(v.iter_errors(candidate))
        if errs:
            fail(name, f"empty-body value {candidate!r} rejected: {errs[0].message}")


def check_reads_subset(name: str, spec: dict, schema: dict) -> None:
    props = set(schema.get("properties", {}))
    fw = set(spec["keys"])
    if not fw <= props:
        fail(name, f"firmware reads keys the schema does not define: {sorted(fw - props)}"
                   f"  (schema properties: {sorted(props)})")

    for nested_key, nspec in spec.get("nested", {}).items():
        if nested_key not in props:
            fail(name, f"nested '{nested_key}' is read by firmware but not in schema.properties")
            continue
        defn = schema.get("definitions", {}).get(nspec["def"])
        if defn is None:
            fail(name, f"schema has no definitions/{nspec['def']} for nested '{nested_key}'")
            continue
        nprops = set(defn.get("properties", {}))
        nfw = set(nspec["keys"])
        if not nfw <= nprops:
            fail(name, f"nested '{nested_key}': firmware reads keys the schema does not define: "
                       f"{sorted(nfw - nprops)}")


def bodies_equal(a: str, b: str) -> bool:
    def norm(n):
        d = load_schema(n)
        d.pop("$id", None)
        d.pop("title", None)
        return json.dumps(d, sort_keys=True)
    return norm(a) == norm(b)


def main() -> int:
    if not SCHEMA_DIR.is_dir():
        return _die(f"schema dir not found: {SCHEMA_DIR}")

    on_disk = {p.name for p in SCHEMA_DIR.glob("*.schema.json")}
    expected = set(CONTRACT)
    if on_disk != expected:
        return _die(f"schema files on disk {sorted(on_disk)} != contract entries {sorted(expected)}")

    for name, spec in CONTRACT.items():
        try:
            schema = load_schema(name)
            Draft7Validator.check_schema(schema)
        except Exception as ex:  # noqa: BLE001
            fail(name, f"schema will not load / is not valid draft-07: {ex}")
            continue

        mode = spec["mode"]
        if mode == "sends_exact":
            check_sends_exact(name, spec, schema, item_level=False)
        elif mode == "sends_exact_array_item":
            check_sends_exact(name, spec, schema, item_level=True)
        elif mode == "empty_body":
            check_empty_body(name, schema)
        elif mode == "reads_subset":
            check_reads_subset(name, spec, schema)
        elif mode == "same_as":
            ref = spec["same_as"]
            if not bodies_equal(name, ref):
                fail(name, f"expected to be identical to {ref} (apart from $id/title) but differs")
            check_reads_subset(name, CONTRACT[ref], schema)
        else:
            fail(name, f"unknown mode {mode!r} in firmware_fields.py")

    if problems:
        print("CONTRACT CHECK FAILED:\n")
        for p in problems:
            print("  -", p)
        print(f"\n{len(problems)} problem(s). See contracts/device-api/README.md for how to re-sync.")
        return 1

    print(f"contract OK - {len(CONTRACT)} schemas, firmware field lists in sync")
    return 0


def _die(msg: str) -> int:
    print("CONTRACT CHECK FAILED:\n  -", msg)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Cross-checks firmware_fields.py's config.response.schema.json entry (top-level "keys"
plus the deviceConfigSensor/deviceConfigController "nested" key lists) against what
src/Controller/ConfigParser.cpp actually reads off the wire.

This is a regex extraction, not real C++ parsing: it looks for ["keyName"] subscripts
on the three JsonObject variables ConfigParser.cpp is known to read through - config,
deviceConfigSensor, deviceConfigController (see ConfigParser.cpp itself for the
JsonObject declarations). A field read through anything else (a helper function, a
renamed variable, a new container) will not be caught here and still needs a human to
notice - see firmware_fields.py's own header comment.

Exit code 0 = the manually-maintained key lists match extraction, 1 = mismatch.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG_PARSER_CPP = HERE.parent.parent / "src" / "Controller" / "ConfigParser.cpp"

sys.path.insert(0, str(HERE))
from firmware_fields import CONTRACT  # noqa: E402

# variable name -> where firmware_fields.py tracks its keys
TRACKED = {
    "config": CONTRACT["config.response.schema.json"]["keys"],
    "deviceConfigSensor": CONTRACT["config.response.schema.json"]["nested"]["deviceConfigSensor"]["keys"],
    "deviceConfigController": CONTRACT["config.response.schema.json"]["nested"]["deviceConfigController"]["keys"],
}


def extract_keys(text: str, varname: str) -> set[str]:
    return set(re.findall(rf'\b{re.escape(varname)}\["([A-Za-z0-9_]+)"\]', text))


def main() -> int:
    if not CONFIG_PARSER_CPP.is_file():
        print(f"CONTRACT CHECK FAILED:\n  - {CONFIG_PARSER_CPP} not found")
        return 1

    text = CONFIG_PARSER_CPP.read_text(encoding="utf-8")
    problems: list[str] = []

    for varname, tracked_keys in TRACKED.items():
        extracted = extract_keys(text, varname)
        tracked = set(tracked_keys)

        missing_from_tracking = extracted - tracked
        if missing_from_tracking:
            problems.append(
                f"ConfigParser.cpp reads {varname}[...] keys not in firmware_fields.py: "
                f"{sorted(missing_from_tracking)}"
            )

        missing_from_code = tracked - extracted
        if missing_from_code:
            problems.append(
                f"firmware_fields.py lists {varname}[...] keys ConfigParser.cpp no longer reads: "
                f"{sorted(missing_from_code)}"
            )

    if problems:
        print("CONTRACT CHECK FAILED (firmware_fields.py vs ConfigParser.cpp drift):\n")
        for p in problems:
            print("  -", p)
        print(f"\n{len(problems)} problem(s). Update firmware_fields.py's config.response.schema.json "
              "entry to match.")
        return 1

    print(f"extraction OK - firmware_fields.py matches ConfigParser.cpp for {', '.join(TRACKED)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

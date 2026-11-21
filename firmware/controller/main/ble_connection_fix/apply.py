"""Use one project-local CHIP source correction without changing the shared SDK."""
import hashlib
import json
import os
from pathlib import Path
import re
import sys
build, overlay, sdk = (Path(arg).resolve() for arg in sys.argv[1:4])
meta = json.loads(overlay.with_suffix(".json").read_text())
for name, expected in meta["upstream_sha256"].items():
    if hashlib.sha256((sdk/name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"BLE connection correction requires review after SDK change: {name}")
ninja = build / "obj/third_party/connectedhomeip/src/platform/ESP32/ESP32.ninja"
content = ninja.read_text()
pattern = r"(?m)^(build [^\r\n]*ESP32\.BLEManagerImpl\.cpp\.o: cxx )(\S+)(.*)$"
if len(list(re.finditer(pattern, content))) != 1:
    raise SystemExit("Expected exactly one CHIP BLEManagerImpl compilation rule")
replacement = os.path.relpath(overlay, build).replace("\\", "/")
replacement = replacement.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")
new = re.sub(pattern, lambda m: m[1] + replacement + m[3], content)
if new != content:
    ninja.write_text(new)
print("CHIP BLE uses the project-local per-connection error correction")

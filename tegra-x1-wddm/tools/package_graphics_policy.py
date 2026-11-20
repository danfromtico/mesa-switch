#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package the repository's promotion policy for the on-device boot tool."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def stage_graphics_policy(root: Path, output: Path) -> str:
    display = root / "tegra-x1-wddm"
    scripts = display / "tools/tgd3d-default"
    sources = [
        scripts / "Native-Boot.ps1",
        scripts / "Baseline-Promotion.ps1",
        scripts / "Baseline-Device.ps1",
        scripts / "Install-GraphicsBootPolicy.ps1",
        scripts / "Invoke-GraphicsBootPolicy.ps1",
        display / "docs/open-faults.json",
        display / "docs/graphics-baseline.json",
    ]
    output.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for source in sources:
        target = output / source.name
        shutil.copyfile(source, target)
        hashes[source.name] = hashlib.sha256(target.read_bytes()).hexdigest()
    manifest = output / "graphics-policy-manifest.json"
    body = json.dumps({"schema": 1, "files": hashes}, indent=2, sort_keys=True) + "\n"
    manifest.write_bytes(body.encode("utf-8"))
    return hashlib.sha256(manifest.read_bytes()).hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    print(stage_graphics_policy(Path(__file__).resolve().parents[2], args.out))

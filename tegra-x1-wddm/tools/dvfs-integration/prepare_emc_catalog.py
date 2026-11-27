#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage and sign the matched EMC platform drivers for native Inf2Cat.

Does not install anything. UMD and ICD are deliberately not package inputs.
The caller supplies the existing test certificate and key; neither is copied.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


def identity(path):
    return {"Length": path.stat().st_size,
            "Sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("drivers", "build", "kmd", "cert", "key", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--signer", default="osslsigncode")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    inputs = {}
    components = [("SwitchFan", "switchfan"), ("SwitchPowerButton", "switchpwrbtn"),
                  ("SwitchBattery", "switchbattery"), (None, "tegragpurender")]
    for source, name in components:
        dest = args.out / name
        dest.mkdir()
        if source:
            shutil.copyfile(args.drivers / "drivers" / source / (name + ".inf"),
                            dest / (name + ".inf"))
            binary = args.build / name / (name + ".sys")
        else:
            binary = args.kmd / (name + ".sys")
            if "0.1.0.158" not in (args.kmd / (name + ".inf")).read_text():
                raise ValueError("Expected the current .158 KMD package")
            for path in args.kmd.iterdir():
                if path.is_file() and path.suffix.lower() not in (".sys", ".cat", ".pdb"):
                    shutil.copyfile(path, dest / path.name)
        inputs[name] = identity(binary)
        with (args.out / (name + "-sign.log")).open("w") as log:
            subprocess.run([args.signer, "sign", "-certs", str(args.cert),
                            "-key", str(args.key), "-h", "sha256", "-n",
                            "Switch Lite EMC runtime " + name, "-in", str(binary),
                            "-out", str(dest / binary.name)], check=True,
                           stdout=log, stderr=subprocess.STDOUT)
    files = {p.relative_to(args.out).as_posix(): identity(p)
             for _, name in components for p in sorted((args.out / name).iterdir()) if p.is_file()}
    manifest = {"state": "SYS signed; native catalog generation and verification pending",
                "inputs": inputs, "files": files, "driver_names": [n for _, n in components]}
    (args.out / "input.json").write_text(json.dumps(manifest, indent=2) + "\n")
    with zipfile.ZipFile(args.out.with_suffix(".zip"), "w", zipfile.ZIP_DEFLATED) as archive:
        for relative in list(files) + ["input.json"]:
            archive.write(args.out / relative, relative)
    print(json.dumps({"directory": str(args.out), "zip": identity(args.out.with_suffix(".zip"))}))


if __name__ == "__main__":
    main()

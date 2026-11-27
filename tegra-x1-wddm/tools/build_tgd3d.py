#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the direct D3D10/Vulkan UMD in an isolated directory; never install it."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cross-file", type=Path, required=True)
    parser.add_argument("--sdk-include", type=Path, required=True)
    parser.add_argument("--wdk-include", type=Path, required=True)
    parser.add_argument("--cpu-sync-control", action="store_true",
                        help="diagnostic only: replace cross-context fences with synchronous Flush/Present")
    parser.add_argument("--red-primary-control", action="store_true",
                        help="diagnostic only: replace presentation rotation with a red 720x1280 primary")
    args = parser.parse_args()
    if args.cpu_sync_control and args.red_primary_control:
        parser.error("red-primary control retains GPU bridge; do not combine diagnostic changes")
    source = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    headers = out / "headers"
    headers.mkdir(parents=True, exist_ok=True)
    search = [args.wdk_include / sub for sub in ("um", "shared", "km")]
    search += [args.sdk_include / sub for sub in ("um", "shared")]
    for name in ("d3d10umddi.h", "d3d11TokenizedProgramFormat.hpp", "d3dkmddi.h",
                 "d3dkmdt.h", "d3dkmthk.h", "d3dukmdt.h", "d3dumddi.h",
                 "dxgiddi.h", "dxmini.h", "wmidata.h", "minidumpapiset.h", "dxva.h"):
        header = next((d / name for d in search if (d / name).is_file()), None)
        if header is None:
            parser.error(f"SDK/WDK header not found: {name}")
        shutil.copyfile(header, headers / name)
    build = out / "build"
    reconfigure = ["--reconfigure"] if (build / "meson-private/coredata.dat").is_file() else []
    includes = json.dumps(["-I" + str(headers)])
    cpp_args = ["-I" + str(headers)]
    if args.cpu_sync_control:
        cpp_args.append("-DTGD3D_CPU_SYNC_CONTROL")
    if args.red_primary_control:
        cpp_args.append("-DTGD3D_RED_PRIMARY_CONTROL")
    command = [
        "meson", "setup", *reconfigure, str(build), str(source),
        "--cross-file", str(args.cross_file.resolve()),
        "-Dplatforms=windows", "-Dvulkan-drivers=[]", "-Dgallium-drivers=[]",
        "-Dtgd3d=enabled", "-Dglx=disabled", "-Degl=disabled", "-Dgbm=disabled",
        "-Dllvm=disabled", "-Dbuild-tests=true", "-Dvideo-codecs=[]",
        "-Dvalgrind=disabled", "-Dlibunwind=disabled", "-Dgles1=disabled",
        "-Dgles2=disabled", "-Dopengl=false", "-Dshader-cache=enabled",
        "-Dzlib=disabled", "-Dzstd=disabled", "-Dexpat=disabled",
        "-Dbuildtype=release", f"-Dc_args={includes}", f"-Dcpp_args={json.dumps(cpp_args)}",
    ]
    subprocess.run(command, cwd=source, check=True)
    artifacts = {"TegraGpuD3DVk.dll": "src/nouveau/tgd3d/TegraGpuD3DVk.dll",
                 "tgd3d-contracts.exe": "src/nouveau/tgd3d/tests/tgd3d-contracts.exe"}
    for name in ("tgd3d-runtime-check.exe", "tgd3d-trace-export.exe", "tgd3d-trace-control.exe",
                 "tgd3d-dump-capture.exe", "tgd3d-display-timing.exe"):
        artifacts[name] = "src/nouveau/tgd3d/tests/" + name
    subprocess.run(["ninja", "-C", str(build), *artifacts.values()],
                   cwd=source, check=True)
    names = list(artifacts)
    for name, path in artifacts.items():
        shutil.copyfile(build / path, out / name)
    hashes = {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in names}
    source_status = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=normal"], cwd=source, text=True).splitlines()
    (out / "manifest.json").write_text(json.dumps({
        "component": "tgd3d", "mesa": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
        "source_dirty": bool(source_status), "source_status": source_status,
        "sync_mode": "cpu-control" if args.cpu_sync_control else "gpu-bridge",
        "red_primary_control": args.red_primary_control,
        "configure": command, "files": hashes,
        "validation": "cross-built only; native rendering and display acceptance pending",
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the NVK D3D UMD, controlled Windows tests and explicit native probe.

Requires Meson/Ninja, an LLVM-MinGW cross file, Mesa's Python dependencies,
and native mesa_clc/vtn_bindgen2 on PATH, as for build_icd.py. SDK/WDK headers
are copied to the output directory, not into Mesa or the compiler sysroot.
This does not install the UMD or enable KMD native submissions.
"""
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
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    headers = out / "headers"
    headers.mkdir(exist_ok=True)
    search = [args.wdk_include / sub for sub in ("um", "shared", "km")]
    search += [args.sdk_include / sub for sub in ("um", "shared")]
    for name in ("d3d10umddi.h", "d3d11TokenizedProgramFormat.hpp", "d3dkmddi.h",
                 "d3dkmdt.h", "d3dkmthk.h", "d3dukmdt.h", "d3dumddi.h",
                 "dxgiddi.h", "dxmini.h", "wmidata.h"):
        header = next((directory / name for directory in search
                       if (directory / name).is_file()), None)
        if header is None:
            parser.error(f"SDK/WDK header not found: {name}")
        destination = headers / name
        if not destination.exists() or destination.read_bytes() != header.read_bytes():
            shutil.copyfile(header, destination)

    build = out / "build"
    reconfigure = ["--reconfigure"] if (build / "meson-private/coredata.dat").is_file() else []
    includes = json.dumps(["-I" + str(headers)])
    subprocess.run([
        "meson", "setup", *reconfigure, str(build), str(source), "--cross-file", str(args.cross_file.resolve()),
        "-Dplatforms=windows", "-Dvulkan-drivers=[]", "-Dgallium-drivers=zink",
        "-Dtgd3d=disabled",
        "-Dgallium-d3d10umd=true", "-Dgallium-d3d10-backend=nvk",
        "-Dgallium-d3d10-dll-name=TegraGpuD3D", "-Dglx=disabled", "-Degl=disabled",
        "-Dgbm=disabled", "-Dllvm=disabled", "-Dbuild-tests=true", "-Dvideo-codecs=[]",
        "-Dvalgrind=disabled", "-Dlibunwind=disabled", "-Dgles1=disabled", "-Dgles2=disabled",
        "-Dopengl=false", "-Dshader-cache=enabled", "-Dzlib=disabled", "-Dzstd=disabled",
        "-Dexpat=disabled", "-Dmesa-clc=system", "-Dbuildtype=release",
        f"-Dc_args={includes}", f"-Dcpp_args={includes}",
    ], check=True)
    target = "src/gallium/targets/d3d10umd/"
    names = ["TegraGpuD3D.dll", "test_d3d10_nvk_adapter.exe", "test_d3d10_nvk_entry.exe",
             "test_d3d10_nvk_device.exe", "nvk_d3d_check.exe", "nvk_d3d_runtime_check.exe",
             "d3d10-trace-export.exe", "test_d3d10_runtime_primary.exe",
             "test_d3d10_runtime_present.exe", "test_d3d10_runtime_shared.exe"]
    sample_test = "src/gallium/auxiliary/gallium-aux.exe"
    binding_test = "src/gallium/drivers/zink/nir_to_spirv/test_zink_descriptor_binding.exe"
    cache_test = "src/util/test-win32-disk-cache.exe"
    sync_test = "src/gallium/drivers/zink/test-zink-win32-sync.exe"
    subprocess.run(["ninja", "-C", str(build), *(target + name for name in names), sample_test, binding_test, cache_test, sync_test], check=True)
    for name in names:
        shutil.copyfile(build / target / name, out / name)
    shutil.copyfile(build / sample_test, out / "test_tgsi_to_nir.exe")
    names.append("test_tgsi_to_nir.exe")
    shutil.copyfile(build / binding_test, out / "test_zink_descriptor_binding.exe")
    names.append("test_zink_descriptor_binding.exe")
    shutil.copyfile(build / cache_test, out / "test-win32-disk-cache.exe")
    names.append("test-win32-disk-cache.exe")
    shutil.copyfile(build / sync_test, out / "test-zink-win32-sync.exe")
    names.append("test-zink-win32-sync.exe")
    hashes = {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in names}
    (out / "manifest.json").write_text(json.dumps({
        "component": "d3d10-umd",
        "artifact_id": out.name + "+" + hashes["TegraGpuD3D.dll"][:12],
        "mesa": subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip(),
        "scope": "Experimental NVK D3D10 UMD, controlled fixtures, and explicit DDI/Windows-runtime drawing probes",
        "files": hashes,
    }, indent=2) + "\n")
    print(f"Built the NVK D3D UMD and controlled tests in {out}")


if __name__ == "__main__":
    main()

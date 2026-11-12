#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the NVK Windows ARM64 ICD from the Mesa Switch Windows source tree.

Windows nvkmd uses SDK-compiled KMT calls for memory, VA, queue and sync.
Physical device enumeration requires the live KMD's native-submit flag.
Requires the same Meson/Rust/bindgen LLVM-MinGW environment as the NAK tool,
plus build-native mesa_clc and vtn_bindgen2 in PATH (-Dmesa-clc=system).
Build products go in the output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

REV = "0ff52079ca9e9e837ed27eb95a8d239f439a1864"
HERE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mesa", type=Path, default=HERE.parents[4])
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cross-file", type=Path, required=True)
    p.add_argument("--cc", type=Path, required=True, help="ARM64 LLVM-MinGW clang")
    p.add_argument("--clang-cl", type=Path, required=True)
    p.add_argument("--sdk-include", type=Path, required=True, help="SDK Include/<version>")
    p.add_argument("--wdk-include", type=Path, required=True, help="WDK Include/<version>")
    p.add_argument("--prepare-only", action="store_true",
                   help="build the SDK ABI fixture, KMT bridge and memory tests without configuring Mesa")
    a = p.parse_args()
    src = a.mesa.resolve()
    win = src / "src/nouveau/vulkan/nvkmd/windows"
    if not (win / "kmt_native.c").is_file():
        p.error("Use the Mesa Switch Windows checkout containing NVK's KMT backend")
    rev = subprocess.check_output(["git", "-C", str(src), "rev-parse", "HEAD"], text=True).strip()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sdk_cc = (a.clang_cl, "--target=aarch64-pc-windows-msvc", "/c", "/std:c11",
        "/W4", "/WX", "-Wno-nonportable-include-path", "/D_ARM64_",
        "/imsvc" + str(a.sdk_include / "um"), "/imsvc" + str(a.sdk_include / "shared"),
        "/imsvc" + str(a.wdk_include / "km/crt"))
    run(*sdk_cc, "/Fo" + str(out / "kmt-sdk.obj"), "--", win / "test-kmt-sdk.c")
    run(a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(src / "include"),
        "-I" + str(src / "src/vulkan/util"), win / "test-win32-image-flags.c",
        "-o", out / "test-win32-image-flags.exe")
    kmt_object = out / "kmt-native.obj"
    run(*sdk_cc, "/O2", "/Gy", "/GS-", "/Zl", f"/Fo{kmt_object}", "--", win / "kmt_native.c")
    shared_test = out / "test-kmt-shared.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{shared_test}", "--", win / "test-kmt-shared.c")
    run(a.cc, shared_test, kmt_object, "-Wl,--gc-sections", "-lgdi32",
        "-o", out / "test-kmt-shared.exe")
    sync_test = out / "test-kmt-sync.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{sync_test}", "--", win / "test-kmt-sync.c")
    run(a.cc, sync_test, kmt_object, "-Wl,--gc-sections", "-lgdi32", "-ladvapi32",
        "-o", out / "test-kmt-sync.exe")
    displayable_test = out / "test-kmt-displayable.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{displayable_test}", "--", win / "test-kmt-displayable.c")
    run(a.cc, displayable_test, kmt_object, "-Wl,--gc-sections", "-lgdi32",
        "-o", out / "test-kmt-displayable.exe")

    memory_test = out / "test-kmt-memory.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{memory_test}", "--", win / "test-kmt-memory.c")
    run(a.cc, memory_test, kmt_object, "-Wl,--gc-sections", "-lgdi32",
        "-o", out / "test-kmt-memory.exe")
    run(a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        win / "check-shared.c", win / "kmt.c", kmt_object, "-lgdi32",
        "-o", out / "nvk-windows-shared-check.exe")
    run(a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        win / "check-memory.c", win / "kmt.c", kmt_object, "-lgdi32",
        "-o", out / "nvk-windows-memory-check.exe")
    run(a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(src / "include"),
        win / "check-vulkan-shared.c", win / "kmt.c",
        "-o", out / "nvk-windows-vulkan-shared-check.exe")
    run(a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
        src / "src/vulkan/wsi/tests/win32_pacing.c",
        "-o", out / "test-win32-pacing.exe")
    if a.prepare_only:
        print(f"Validated the SDK KMT layout and built the bridge and memory tests in {out}")
        return

    build = out / "build"
    digest = hashlib.sha256(rev.encode())
    digest.update(subprocess.check_output(["git", "-C", str(src), "diff", "--binary", "HEAD"]))
    digest.update(a.cross_file.read_bytes())
    run("meson", "setup", build, src, "--cross-file", a.cross_file.resolve(),
        "-Dplatforms=windows", "-Dvulkan-drivers=nouveau", "-Dgallium-drivers=[]", "-Dtgd3d=disabled",
        "-Dglx=disabled", "-Degl=disabled", "-Dgbm=disabled", "-Dllvm=disabled",
        "-Dbuild-tests=true", "-Dvideo-codecs=[]", "-Dvalgrind=disabled",
        "-Dlibunwind=disabled", "-Dgles1=disabled", "-Dgles2=disabled", "-Dopengl=false",
        "-Dshader-cache=enabled", "-Dzlib=disabled", "-Dzstd=disabled", "-Dexpat=disabled",
        "-Dmesa-clc=system", "-Dbuildtype=release", f"-Dnvk-windows-kmt-object={kmt_object}",
        "-Dnvk-build-id=" + digest.hexdigest())
    controls = ["test-nvk-win32-sync.exe", "test-nvk-win32-owner.exe", "test-nvk-win32-written.exe"]
    run("ninja", "-C", build, "src/nouveau/vulkan/libvulkan_nouveau.dll",
        *("src/nouveau/vulkan/" + name for name in controls))
    for name in controls:
        shutil.copyfile(build / "src/nouveau/vulkan" / name, out / name)
    for source, name in (("check.c", "nvk-windows-check.exe"), ("test-kmt.c", "test-kmt.exe"),
                         ("nvdec_check.c", "nvdec-resource-query.exe")):
        run(a.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(src / "include"),
            "-I" + str(win), win / source, win / "kmt.c", "-o", out / name)
    dll = out / "libvulkan_nouveau.dll"
    shutil.copyfile(build / "src/nouveau/vulkan/libvulkan_nouveau.dll", dll)
    manifest = out / "nvk_windows_icd.json"
    # The Windows loader recognizes backslashes when resolving this path
    # relative to the manifest. A POSIX './' can instead depend on the
    # application's directory and fail in hosts such as DWM.
    manifest.write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {
        "library_path": r".\libvulkan_nouveau.dll", "api_version": "1.4.0",
    }}, indent=2) + "\n")
    files = [dll, out / "nvk-windows-check.exe", out / "test-kmt.exe", out / "nvdec-resource-query.exe",
             out / "test-kmt-shared.exe", out / "test-kmt-sync.exe", out / "test-kmt-displayable.exe",
             *(out / name for name in controls),
             out / "nvk-windows-shared-check.exe",
             out / "test-kmt-memory.exe", out / "nvk-windows-memory-check.exe",
             out / "nvk-windows-vulkan-shared-check.exe", out / "test-win32-image-flags.exe",
             out / "test-win32-pacing.exe", manifest]
    hashes = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
    (out / "manifest.json").write_text(json.dumps({
        "component": "nvk-icd",
        "artifact_id": out.name + "+" + hashes[dll.name][:12],
        "mesa": rev, "mesa_switch_base": REV, "build_id": digest.hexdigest(), "sdk_kmt_layout_verified": True,
        "scope": "NVK ICD with Windows KMT transport gated by the native-submit KMD flag",
        "files": hashes,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()

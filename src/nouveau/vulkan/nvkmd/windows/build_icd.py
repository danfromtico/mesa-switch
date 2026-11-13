#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the NVK Windows ICD and KMT bridge from the Mesa Switch Windows tree.

Supports ARM64, ARM64EC, x64, and 32-bit x86 (WOW64) Windows application ABIs.
Windows nvkmd uses SDK-compiled KMT calls for memory, VA, queue and sync.
Physical device enumeration requires the live KMD's native-submit flag.
Full ICD linking requires Meson, Rust (with rust-std for the target triple for
NAK), bindgen, LLVM-MinGW CRT/import libraries, and build-native mesa_clc and
vtn_bindgen2 in PATH (-Dmesa-clc=system). Build products go in --out.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

REV = "0ff52079ca9e9e837ed27eb95a8d239f439a1864"
HERE = Path(__file__).resolve().parent

ARCH_SPECS = {
    "arm64": {
        "msvc_target": "aarch64-pc-windows-msvc",
        "msvc_defines": ("/D_ARM64_",),
        "mingw_triple": "aarch64-w64-mingw32",
        "bindgen_target": "aarch64-w64-windows-gnu",
        "rust_target": "aarch64-pc-windows-gnullvm",
        "cpu_family": "aarch64",
        "cpu": "armv8",
        "is_32bit": False,
    },
    "arm64ec": {
        "msvc_target": "arm64ec-pc-windows-msvc",
        "msvc_defines": ("/D_ARM64EC_", "/D_AMD64_"),
        "mingw_triple": "arm64ec-w64-mingw32",
        "bindgen_target": "arm64ec-w64-windows-gnu",
        "rust_target": "arm64ec-pc-windows-msvc",
        "cpu_family": "aarch64",
        "cpu": "arm64ec",
        "is_32bit": False,
    },
    "x64": {
        "msvc_target": "x86_64-pc-windows-msvc",
        "msvc_defines": ("/D_AMD64_",),
        "mingw_triple": "x86_64-w64-mingw32",
        "bindgen_target": "x86_64-w64-windows-gnu",
        "rust_target": "x86_64-pc-windows-gnullvm",
        "cpu_family": "x86_64",
        "cpu": "x86_64",
        "is_32bit": False,
    },
    "x86": {
        "msvc_target": "i686-pc-windows-msvc",
        "msvc_defines": ("/D_X86_",),
        "mingw_triple": "i686-w64-mingw32",
        "bindgen_target": "i686-w64-windows-gnu",
        "rust_target": "i686-pc-windows-gnullvm",
        "cpu_family": "x86",
        "cpu": "i686",
        "is_32bit": True,
    },
}


def run(*args, env=None):
    subprocess.run([str(a) for a in args], check=True, env=env)


def check_rust_target(rustc_cmd, target):
    sysroot = subprocess.check_output(
        [str(rustc_cmd), "--print", "sysroot"], text=True
    ).strip()
    target_dir = Path(sysroot) / "lib/rustlib" / target
    if not target_dir.is_dir():
        installed = sorted(
            p.name for p in (Path(sysroot) / "lib/rustlib").iterdir()
            if p.is_dir() and p.name not in ("etc", "src")
        )
        raise SystemExit(
            f"error: NAK requires Rust target '{target}', which is missing from "
            f"{sysroot}/lib/rustlib (installed: {', '.join(installed)})."
        )


def write_cross_file(path, spec, mingw_bin, rustc_path, pkg_config_libdir):
    triple = spec["mingw_triple"]
    cc = mingw_bin / f"{triple}-clang"
    cxx = mingw_bin / f"{triple}-clang++"
    windres = mingw_bin / f"{triple}-windres"
    ar = mingw_bin / "llvm-ar"
    strip = mingw_bin / "llvm-strip"
    pkg_config = shutil.which("pkg-config") or "/opt/homebrew/bin/pkg-config"
    lines = [
        "[binaries]",
        f"c = '{cc}'",
        f"cpp = '{cxx}'",
        f"ar = '{ar}'",
        f"strip = '{strip}'",
        f"windres = '{windres}'",
        f"rust = ['{rustc_path}', '--target={spec['rust_target']}', '-C', 'linker={cc}']",
        f"pkg-config = '{pkg_config}'",
        "[host_machine]",
        "system = 'windows'",
        f"cpu_family = '{spec['cpu_family']}'",
        f"cpu = '{spec['cpu']}'",
        "endian = 'little'",
        "[properties]",
        "needs_exe_wrapper = true",
    ]
    if pkg_config_libdir is not None:
        lines.append(f"pkg_config_libdir = ['{pkg_config_libdir}']")
    lines += [
        "[built-in options]",
        "cpp_link_args = ['-static']",
        "",
    ]
    path.write_text("\n".join(lines))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mesa", type=Path, default=HERE.parents[4])
    p.add_argument("--arch", choices=tuple(ARCH_SPECS), default="arm64",
                   help="Windows user-mode target architecture (default: arm64)")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cross-file", type=Path,
                   help="Meson cross file (generated for --arch when omitted)")
    p.add_argument("--cc", type=Path, help="Target LLVM-MinGW clang")
    p.add_argument("--mingw-bin", type=Path,
                   help="LLVM-MinGW bin directory (used to locate <triple>-clang when --cc is omitted)")
    p.add_argument("--clang-cl", type=Path, required=True)
    p.add_argument("--rustc", type=Path, help="rustc binary (defaults to rustc in PATH)")
    p.add_argument("--sdk-include", type=Path, required=True, help="SDK Include/<version>")
    p.add_argument("--wdk-include", type=Path, required=True, help="WDK Include/<version>")
    p.add_argument("--package-cache-dir", type=Path,
                   help="Meson subproject packagecache directory (sets MESON_PACKAGE_CACHE_DIR)")
    p.add_argument("--prepare-only", action="store_true",
                   help="build the SDK ABI fixture, KMT bridge and tests without configuring Mesa")
    a = p.parse_args()

    spec = ARCH_SPECS[a.arch]
    src = a.mesa.resolve()
    win = src / "src/nouveau/vulkan/nvkmd/windows"
    if not (win / "kmt_native.c").is_file():
        p.error("Use the Mesa Switch Windows checkout containing NVK's KMT backend")

    cc = a.cc
    if cc is None and a.mingw_bin is not None:
        cc = a.mingw_bin.absolute() / f"{spec['mingw_triple']}-clang"
    if cc is None:
        found = shutil.which(f"{spec['mingw_triple']}-clang")
        if found:
            cc = Path(found)
    if cc is None:
        p.error("Pass --cc or --mingw-bin for the target LLVM-MinGW toolchain")
    cc = cc.absolute()
    mingw_root = cc.parent.resolve().parent
    has_mingw_sysroot = (mingw_root / spec["mingw_triple"]).is_dir()

    rev = subprocess.check_output(["git", "-C", str(src), "rev-parse", "HEAD"], text=True).strip()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)

    sdk_cc = (
        a.clang_cl,
        f"--target={spec['msvc_target']}",
        "/c", "/std:c11", "/W4", "/WX", "-Wno-nonportable-include-path",
        *spec["msvc_defines"],
        "/imsvc" + str(a.sdk_include / "um"),
        "/imsvc" + str(a.sdk_include / "shared"),
        "/imsvc" + str(a.wdk_include / "km/crt"),
    )
    run(*sdk_cc, "/Fo" + str(out / "kmt-sdk.obj"), "--", win / "test-kmt-sdk.c")
    kmt_object = out / "kmt-native.obj"
    run(*sdk_cc, "/O2", "/Gy", "/GS-", "/Zl", f"/Fo{kmt_object}", "--", win / "kmt_native.c")

    shared_test = out / "test-kmt-shared.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{shared_test}", "--", win / "test-kmt-shared.c")
    sync_test = out / "test-kmt-sync.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{sync_test}", "--", win / "test-kmt-sync.c")
    displayable_test = out / "test-kmt-displayable.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{displayable_test}", "--", win / "test-kmt-displayable.c")
    memory_test = out / "test-kmt-memory.obj"
    run(*sdk_cc, "/O2", "/GS-", "/Zl", f"/Fo{memory_test}", "--", win / "test-kmt-memory.c")

    if not has_mingw_sysroot:
        if a.prepare_only:
            print(
                f"Validated {a.arch} SDK KMT layout and compiled KMT objects in {out} "
                f"(PE linking skipped: {mingw_root / spec['mingw_triple']} sysroot not installed)",
                file=sys.stdout,
            )
            return
        raise SystemExit(
            f"error: cannot link {a.arch} PE binaries because LLVM-MinGW sysroot "
            f"'{mingw_root / spec['mingw_triple']}' is not installed."
        )

    run(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(src / "include"),
        "-I" + str(src / "src/vulkan/util"), win / "test-win32-image-flags.c",
        "-o", out / "test-win32-image-flags.exe")
    run(cc, shared_test, kmt_object, "-Wl,--gc-sections", "-lgdi32",
        "-o", out / "test-kmt-shared.exe")
    run(cc, sync_test, kmt_object, "-Wl,--gc-sections", "-lgdi32", "-ladvapi32",
        "-o", out / "test-kmt-sync.exe")
    run(cc, displayable_test, kmt_object, "-Wl,--gc-sections", "-lgdi32",
        "-o", out / "test-kmt-displayable.exe")
    run(cc, memory_test, kmt_object, "-Wl,--gc-sections", "-lgdi32",
        "-o", out / "test-kmt-memory.exe")
    run(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        win / "check-shared.c", win / "kmt.c", kmt_object, "-lgdi32",
        "-o", out / "nvk-windows-shared-check.exe")
    run(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        win / "check-memory.c", win / "kmt.c", kmt_object, "-lgdi32",
        "-o", out / "nvk-windows-memory-check.exe")
    run(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(src / "include"),
        win / "check-vulkan-shared.c", win / "kmt.c",
        "-o", out / "nvk-windows-vulkan-shared-check.exe")
    for source, name in (("check.c", "nvk-windows-check.exe"), ("test-kmt.c", "test-kmt.exe"),
                         ("nvdec_check.c", "nvdec-resource-query.exe")):
        run(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(src / "include"),
            "-I" + str(win), win / source, win / "kmt.c", "-o", out / name)
    run(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
        src / "src/vulkan/wsi/tests/win32_pacing.c",
        "-o", out / "test-win32-pacing.exe")
    run(cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I" + str(src / "src"),
        "-I" + str(src / "src/nouveau/headers"),
        "-I" + str(src / "src/nouveau/headers/nvidia/classes"),
        win / "test-exec-transport.c",
        "-o", out / "test-exec-transport.exe")
    if a.prepare_only:
        print(f"Validated {a.arch} SDK KMT layout and built the bridge and tests in {out}")
        return

    rustc = a.rustc or shutil.which("rustc")
    if not rustc:
        raise SystemExit("error: rustc not found in PATH (required by NAK)")
    check_rust_target(rustc, spec["rust_target"])

    cross_file = a.cross_file
    if cross_file is None:
        cross_file = out / f"windows-{a.arch}.ini"
        empty_pkg = out / "empty-pkgconfig"
        empty_pkg.mkdir(exist_ok=True)
        write_cross_file(cross_file, spec, cc.parent, Path(rustc).absolute(), empty_pkg)
    else:
        cross_file = cross_file.resolve()

    env = os.environ.copy()
    if a.package_cache_dir is not None:
        env["MESON_PACKAGE_CACHE_DIR"] = str(a.package_cache_dir.resolve())
    if "LIBCLANG_PATH" not in env:
        clang_cl_lib = a.clang_cl.resolve().parent.parent / "lib"
        if any((clang_cl_lib / name).is_file() for name in ("libclang.dylib", "libclang.so", "libclang.dll")):
            env["LIBCLANG_PATH"] = str(clang_cl_lib)
    if "BINDGEN_EXTRA_CLANG_ARGS" not in env:
        clang_lib = mingw_root / "lib/clang"
        resource_dirs = sorted(p for p in clang_lib.iterdir() if p.is_dir()) if clang_lib.is_dir() else []
        bindgen_args = [
            f"--target={spec['bindgen_target']}",
            f"--sysroot={mingw_root / spec['mingw_triple']}",
        ]
        if resource_dirs:
            bindgen_args += ["-resource-dir", str(resource_dirs[-1])]
        generic_inc = mingw_root / "generic-w64-mingw32/include"
        if generic_inc.is_dir():
            bindgen_args += ["-isystem", str(generic_inc)]
        env["BINDGEN_EXTRA_CLANG_ARGS"] = " ".join(bindgen_args)

    meson_options_file = src / "meson.options"
    extra_meson_args = []
    if meson_options_file.is_file() and "'tgd3d'" in meson_options_file.read_text(encoding="utf-8", errors="ignore"):
        extra_meson_args.append("-Dtgd3d=disabled")

    build = out / "build"
    digest = hashlib.sha256(rev.encode())
    digest.update(subprocess.check_output(["git", "-C", str(src), "diff", "--binary", "HEAD"]))
    digest.update(cross_file.read_bytes())
    run("meson", "setup", build, src, "--cross-file", cross_file,
        "-Dplatforms=windows", "-Dvulkan-drivers=nouveau", "-Dgallium-drivers=[]",
        "-Dglx=disabled", "-Degl=disabled", "-Dgbm=disabled", "-Dllvm=disabled",
        "-Dbuild-tests=true", "-Dvideo-codecs=[]", "-Dvalgrind=disabled",
        "-Dlibunwind=disabled", "-Dgles1=disabled", "-Dgles2=disabled", "-Dopengl=false",
        "-Dshader-cache=enabled", "-Dzlib=disabled", "-Dzstd=disabled", "-Dexpat=disabled",
        "-Dmesa-clc=system", "-Dbuildtype=release", f"-Dnvk-windows-kmt-object={kmt_object}",
        "-Dnvk-build-id=" + digest.hexdigest(), *extra_meson_args, env=env)
    controls = ["test-nvk-win32-sync.exe", "test-nvk-win32-owner.exe", "test-nvk-win32-written.exe"]
    run("ninja", "-C", build, "src/nouveau/vulkan/libvulkan_nouveau.dll",
        *("src/nouveau/vulkan/" + name for name in controls), env=env)
    for name in controls:
        shutil.copyfile(build / "src/nouveau/vulkan" / name, out / name)
    dll = out / "libvulkan_nouveau.dll"
    shutil.copyfile(build / "src/nouveau/vulkan/libvulkan_nouveau.dll", dll)
    manifest_name = "nvk_windows_icd.x86.json" if spec["is_32bit"] else "nvk_windows_icd.json"
    manifest = out / manifest_name
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
             out / "test-win32-pacing.exe", out / "test-exec-transport.exe", manifest]
    hashes = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
    (out / "manifest.json").write_text(json.dumps({
        "component": "nvk-icd",
        "arch": a.arch,
        "artifact_id": out.name + "+" + hashes[dll.name][:12],
        "mesa": rev, "mesa_switch_base": REV, "build_id": digest.hexdigest(),
        "sdk_kmt_layout_verified": True,
        "scope": f"NVK {a.arch} ICD with Windows KMT transport gated by the native-submit KMD flag",
        "files": hashes,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()

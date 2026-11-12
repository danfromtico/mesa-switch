#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cross-build pinned upstream DXVK and the offscreen Direct3D smoke test.

DXVK is not vendored: pass a clean checkout of the pinned tag, including its
submodules. Requires LLVM-MinGW (UCRT), Meson, Ninja and glslang on PATH (or
pass --mingw-bin for the LLVM-MinGW toolchain directory).

  git clone --recurse-submodules --branch v3.1.1 \\
      https://github.com/doitsujin/dxvk.git
  python3 build_dxvk.py --dxvk dxvk --out build/dxvk-arm64

`--arch arm64` is the native Windows-on-ARM configuration beside the ARM64 NVK
ICD. `--arch x64` and `--arch x86` build the 64-bit x64 and 32-bit x86 (WOW64)
DXVK DLLs and smoke executable; on Windows on ARM those processes require a
matching x64/ARM64EC or 32-bit x86 Vulkan ICD and loader registration because
emulated x64/x86 processes cannot load a pure ARM64 ICD DLL.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

DXVK_TAG = "v3.1.1"
DXVK_COMMIT = "b1a1c99ab52b687cf950d62c88bc2fa316b41663"
TRIPLES = {
    "arm64": ("aarch64", "aarch64", "aarch64-w64-mingw32"),
    "x64": ("x86_64", "x86_64", "x86_64-w64-mingw32"),
    "x86": ("x86", "i686", "i686-w64-mingw32"),
}
DLLS = ("src/d3d9/d3d9.dll", "src/d3d10/d3d10core.dll", "src/d3d11/d3d11.dll", "src/dxgi/dxgi.dll")
HERE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def output(*args):
    return subprocess.check_output([str(a) for a in args], text=True).strip()


def resolve_tool(mingw_bin: Path | None, preferred: str, fallback: str | None = None) -> str:
    candidates = [preferred] + ([fallback] if fallback else [])
    for name in candidates:
        if mingw_bin is not None:
            candidate = mingw_bin / name
            if candidate.is_file():
                return str(candidate)
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit(f"error: required tool '{preferred}' not found")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dxvk", type=Path, required=True, help="clean DXVK checkout at the pinned commit")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--arch", choices=sorted(TRIPLES), default="arm64")
    p.add_argument("--mingw-bin", type=Path, help="optional LLVM-MinGW bin directory")
    a = p.parse_args()

    dxvk = a.dxvk.resolve()
    if not (dxvk / ".git").exists():
        p.error(f"{dxvk} is not a git checkout of DXVK {DXVK_TAG} ({DXVK_COMMIT})")
    head = output("git", "-C", dxvk, "rev-parse", "HEAD")
    if head != DXVK_COMMIT:
        p.error(f"{dxvk} is at {head}; this recipe is pinned to {DXVK_TAG} ({DXVK_COMMIT})")
    # Tracked files only: Meson leaves an untracked lock file in subprojects/.
    if output("git", "-C", dxvk, "status", "--porcelain", "--untracked-files=no",
              "--ignore-submodules=none"):
        p.error(f"{dxvk} has modified tracked files; build the pinned source unmodified")
    submodules = output("git", "-C", dxvk, "submodule", "status", "--recursive")
    if any(line[:1] in "-+U" for line in submodules.splitlines()):
        p.error("DXVK submodules are missing or not at the recorded commits")

    family, cpu, triple = TRIPLES[a.arch]
    mingw_bin = a.mingw_bin.absolute() if a.mingw_bin is not None else None
    cc = resolve_tool(mingw_bin, f"{triple}-clang")
    cxx = resolve_tool(mingw_bin, f"{triple}-clang++")
    ar = resolve_tool(mingw_bin, f"{triple}-ar", "llvm-ar")
    strip = resolve_tool(mingw_bin, f"{triple}-strip", "llvm-strip")
    windres = resolve_tool(mingw_bin, f"{triple}-windres")
    glslang = shutil.which("glslang") or shutil.which("glslangValidator")
    if not glslang:
        raise SystemExit("error: glslang or glslangValidator not found in PATH")

    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    cross = out / "cross.txt"
    cross.write_text(f"""[binaries]
c = '{cc}'
cpp = '{cxx}'
ar = '{ar}'
strip = '{strip}'
windres = '{windres}'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = '{family}'
cpu = '{cpu}'
endian = 'little'
""")
    build = out / "build"
    run("meson", "setup", "--cross-file", cross, "--buildtype", "release", build, dxvk)
    run("ninja", "-C", build)

    package = out / "package"
    package.mkdir()
    for dll in DLLS:
        shutil.copyfile(build / dll, package / Path(dll).name)
    smoke = package / "dxvk-offscreen.exe"
    run(cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
        HERE / "dxvk_offscreen.cpp", "-o", smoke, "-ldxguid", "-luuid")

    files = sorted(package.iterdir())
    (out / "manifest.json").write_text(json.dumps({
        "component": "dxvk-smoke",
        "arch": a.arch,
        "device_configuration": a.arch == "arm64",
        "requires_matching_arch_icd": a.arch != "arm64",
        "dxvk": {"tag": DXVK_TAG, "commit": DXVK_COMMIT, "submodules": submodules.splitlines()},
        "compiler": output(cc, "--version").splitlines()[0],
        "meson": output("meson", "--version"),
        "glslang": output(glslang, "--version").splitlines()[0],
        "scope": "cross-build only; no rendering result is implied",
        "files": {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
    }, indent=2) + "\n")
    print(f"Built DXVK {DXVK_TAG} ({a.arch}) and dxvk-offscreen.exe in {package}")


if __name__ == "__main__":
    main()

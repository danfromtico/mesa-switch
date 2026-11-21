#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build an x86 D3D9 bytecode replay using DXVK's pinned public device interface."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
DXVK_REVISION = "b1a1c99ab52b687cf950d62c88bc2fa316b41663"
INTERFACES_SHA256 = "c80b7df66b07c0cb77f785e4a64c470baa8cd35a39965b941fb9d818468f9a05"
DEVICE_IID = "2eaa4b89-0107-4bdb-87f7-0f541c493ce0"


def device_interface(checkout):
    src = subprocess.check_output([
        "git", "-C", checkout, "show", f"{DXVK_REVISION}:src/d3d9/d3d9_interfaces.h"
    ])
    if hashlib.sha256(src).hexdigest() != INTERFACES_SHA256:
        raise ValueError("Unexpected DXVK device interface header")
    text = src.decode("utf-8")
    declaration = re.search(rf'MIDL_INTERFACE\("{DEVICE_IID}"\).*?^}};',
                            text, re.MULTILINE | re.DOTALL)
    iid = next((line for line in text.splitlines()
                if line.startswith("__CRT_UUID_DECL(ID3D9VkInteropDevice,")), None)
    if declaration is None or iid is None:
        raise ValueError("Missing ID3D9VkInteropDevice declaration")
    return ("#pragma once\nstruct ID3D9VkInteropTexture;\nstruct D3D9VkExtImageDesc;\n"
            + declaration.group() + "\n" + iid + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxvk", type=Path, required=True, help="DXVK checkout containing v3.1.1")
    parser.add_argument("--cxx", default="i686-w64-mingw32-clang++", help="LLVM-MinGW x86 C++ compiler")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    interface = device_interface(args.dxvk)
    with tempfile.TemporaryDirectory() as temp:
        (Path(temp) / "dxvk_device_interop.h").write_text(interface)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-static", "-municode",
            "-I" + temp, "-I" + str(HERE.parents[2] / "include"),
            str(HERE / "d3d9_shader_replay.cpp"), "-o", str(args.out), "-Wl,--no-insert-timestamp",
            "-ldxguid", "-luuid", "-luser32"
        ], check=True)


if __name__ == "__main__":
    main()

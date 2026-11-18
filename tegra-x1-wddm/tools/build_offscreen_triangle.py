#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the ARM64 Vulkan readback test using Khronos headers and loader."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess

SOURCE = Path(__file__).resolve().parent / "vk_offscreen"


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True, type=Path, help="ARM64 LLVM-MinGW clang")
    parser.add_argument("--vulkan-include", required=True, type=Path)
    parser.add_argument("--loader-dll", required=True, type=Path)
    parser.add_argument("--loader-lib", required=True, type=Path, help="ARM64 Vulkan import library")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)

    header = []
    for stage in ("vert", "frag"):
        output = args.out / f"triangle-{stage}.spv"
        run("glslangValidator", "-V", "--target-env", "vulkan1.0", "-o", output,
            SOURCE / f"triangle.{stage}")
        run("spirv-val", "--target-env", "vulkan1.0", output)
        words = [word for (word,) in struct.iter_unpack("<I", output.read_bytes())]
        header.append(f"static const uint32_t {stage}_spirv[] = {{")
        for start in range(0, len(words), 6):
            header.append("    " + ", ".join(f"0x{word:08x}u" for word in words[start:start + 6]) + ",")
        header.append("};")
    (args.out / "triangle-shaders.h").write_text("\n".join(header) + "\n")
    run(args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", args.vulkan_include, "-I", args.out, SOURCE / "triangle.c",
        args.loader_lib, "-lgdi32", "-luser32", "-lpsapi", "-o", args.out / "vk-triangle.exe")
    shutil.copyfile(args.loader_dll, args.out / "vulkan-1.dll")


if __name__ == "__main__":
    main()

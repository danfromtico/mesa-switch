#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the native ARM64 D3D11VA window fixture with LLVM-MinGW and FXC.

Supply the source and static build of FFmpeg n8.0.1 with H.264, the mov
demuxer and D3D11VA enabled. On a cross host, --fxc-runner can point at Wine.
The output executable contains the compiled shaders. Only fixture source is
distributed with this project.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
NOUVEAU = HERE.parents[1] / "src" / "nouveau"


def build_shaders(output, fxc, runner, scale):
    source = HERE / "nvdec720_preview.hlsl"
    with tempfile.TemporaryDirectory(dir=output) as scratch:
        work = Path(scratch)
        shutil.copyfile(source, work / source.name)
        for stage in ("vs", "ps"):
            symbol = "nvdec720_preview_" + stage
            bytecode = symbol + ".cso"
            command = [runner, fxc] if runner else [fxc]
            subprocess.run(command + ["/nologo", "/Ges", "/O3", "/D", f"NVDEC_PREVIEW_SCALE={scale}", "/T", stage + "_4_0",
                                      "/E", "preview_" + stage, "/Fo", bytecode, source.name],
                           cwd=work, check=True)
            data = (work / bytecode).read_bytes()
            rows = [", ".join(f"0x{x:02x}" for x in data[i:i + 16])
                    for i in range(0, len(data), 16)]
            (output / (symbol + ".h")).write_text(
                f"static const unsigned char {symbol}[] = {{\n   " +
                ",\n   ".join(rows) + "\n};\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True, help="ARM64 Windows C compiler")
    parser.add_argument("--cxx", required=True, help="ARM64 Windows C++ compiler")
    parser.add_argument("--fxc", type=Path, required=True)
    parser.add_argument("--fxc-runner", type=Path)
    parser.add_argument("--ffmpeg-source", type=Path, required=True)
    parser.add_argument("--ffmpeg-build", type=Path, required=True)
    parser.add_argument("--height", type=int, choices=(720, 1080), default=720,
                        help="visible height of the native H.264 fixture")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(exist_ok=True, parents=True)
    scale = args.height // 360
    build_shaders(output, args.fxc.resolve(), args.fxc_runner, scale)
    flags = ["-O2", "-Wall", "-Wextra", "-Werror"]
    objects = []
    for source in [NOUVEAU / "nvdec/nvdec_sha256.c", NOUVEAU / "tgd3d/tgd3d_csc.c"]:
        obj = output / (source.stem + ".o")
        subprocess.run([args.cc, "-std=c11", *flags, "-c", source, "-o", obj], check=True)
        objects.append(obj)

    build = args.ffmpeg_build.resolve()
    archives = [build / name / (name + ".a")
                for name in ("libavformat", "libavcodec", "libavutil", "libswresample")]
    subprocess.run([args.cxx, "-std=c++17", *flags, "-static", "-municode",
                    f"-DNVDEC_PREVIEW_SCALE={scale}",
                    "-I" + str(output), "-I" + str(build), "-I" + str(args.ffmpeg_source.resolve()),
                    HERE / "nvdec720_av_window.cpp", *objects,
                    "-Wl,--start-group", *archives, "-Wl,--end-group",
                    "-ld3d11", "-ldxgi", "-ldxguid", "-luser32", "-lole32",
                    "-lbcrypt", "-lws2_32", "-lm", "-ldwmapi", "-ladvapi32",
                    "-o", output / f"nvdec{args.height}-preview.exe"], check=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the windowed D3D11 probe using DirectXTK's MIT-licensed Utah teapot."""
import argparse
import hashlib
import math
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
TK_REVISION = "861f3e7df437b50d1f28c964769329e9b23a50fb"
TEAPOT_SHA256 = "4bafd6569650f30806d35c482c64a72815a98604f080baa08a8e9764e5eab30e"


def basis(t):
    s = 1 - t
    return (s**3, 3*t*s*s, 3*t*t*s, t**3), (-3*s*s, 3*s*s-6*t*s, 6*t*s-3*t*t, 3*t*t)


def mesh(source):
    # Git blob identity is stable with both LF and CRLF checkout settings.
    parts = source.split("const DirectX::XMVECTORF32 TeapotControlPoints[]")
    if len(parts) != 2:
        raise ValueError("missing control point table")
    patches = re.findall(r"\{\s*(true|false),\s*\{\s*([\d,\s]+)\}\s*\}", parts[0])
    points = [[float(n.strip().removesuffix("f")) for n in row.split(",")[:3]]
              for row in re.findall(r"\{\s*\{\s*\{([^{}]+)\}", parts[1])]
    if len(patches) != 10 or len(points) != 127:
        raise ValueError("unexpected DirectXTK teapot dimensions")
    vertices, indices = [], []
    steps = 6
    weights = [basis(i / steps) for i in range(steps + 1)]
    for mirror_z, numbers in patches:
        control = [points[int(n)] for n in numbers.strip().split(",")]
        for sx in (1, -1):
            for sz in ((1, -1) if mirror_z == "true" else (1,)):
                start = len(vertices)
                for v, dv in weights:
                    for u, du in weights:
                        def evaluate(vw, uw):
                            return [sum(vw[i]*uw[j]*control[4*i+j][axis]
                                        for i in range(4) for j in range(4)) for axis in range(3)]
                        x, y, z = evaluate(v, u)
                        a, b = evaluate(dv, u), evaluate(v, du)
                        nx, ny, nz = (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
                        length = math.hypot(nx, ny, nz)
                        if length < 1e-10:
                            nx, ny, nz, length = 0, math.copysign(1, y), 0, 1
                        vertices.append((sx*x, y, sz*z, sx*nx/length, ny/length, sz*nz/length))
                for row in range(steps):
                    for col in range(steps):
                        a = start + row*(steps+1) + col
                        b = a + steps+1
                        faces = (a, b, b+1, a, b+1, a+1)
                        indices.extend(faces if sx == sz else reversed(faces))
    return vertices, indices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directxtk", required=True, type=Path, help=f"DirectXTK checkout of {TK_REVISION}")
    parser.add_argument("--cxx", default="aarch64-w64-mingw32-clang++")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    data = subprocess.check_output(["git", "-C", args.directxtk, "show", f"{TK_REVISION}:Src/TeapotData.inc"])
    if hashlib.sha256(data).hexdigest() != TEAPOT_SHA256:
        raise SystemExit("unexpected DirectXTK teapot source hash")
    vertices, indices = mesh(data.decode())
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        header = Path(directory, "teapot_mesh.inc")
        with header.open("w") as f:
            f.write("static constexpr Vertex vertices[] = {\n")
            for v in vertices:
                f.write("  {" + ", ".join(f"{x:.8f}f" for x in v) + "},\n")
            f.write("};\nstatic constexpr uint16_t indices[] = {\n")
            for i in range(0, len(indices), 18):
                f.write("  " + ", ".join(map(str, indices[i:i+18])) + ",\n")
            f.write("};\n")
        subprocess.run([args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-missing-field-initializers", "-static", "-I"+directory,
                        "-I"+str(HERE.parents[2]/"include"), str(HERE/"dxvk_teapot.cpp"),
                        "-o", args.out, "-ld3dcompiler", "-ldxguid", "-luuid", "-luser32"], check=True)
    print(f"{len(vertices)} vertices, {len(indices)//3} triangles, {args.out}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile fixed descriptor-free draw shaders with the pinned Mesa NAK.

Requires Meson, Ninja, Rust >=1.90, bindgen, cbindgen, glslangValidator,
spirv-val and Mesa's Python dependencies in PATH. No device access or ICD is
built. The external Mesa checkout is read only; builds use a private archive.
On macOS set BINDGEN_EXTRA_CLANG_ARGS to the compiler's SDK/resource paths.
For a Windows ARM64 compiler, pass --windows-cross-file (LLVM-MinGW/gnullvm)
and --build-only. Set bindgen's target/sysroot/includes to that same toolchain.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import tarfile
from pathlib import Path

REV = "0ff52079ca9e9e837ed27eb95a8d239f439a1864"
HERE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def replace(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f"unexpected Mesa source: {path}: {old}")
    path.write_text(text.replace(old, new))


def constants(src):
    nv = src / "src/nouveau"
    private = (nv / "vulkan/nvk_private.h").read_text()
    root = (nv / "vulkan/nvk_cmd_buffer.h").read_text()
    desc = (nv / "vulkan/nvk_descriptor_types.h").read_text()
    shader = (nv / "vulkan/nvk_shader.c").read_text()
    text = "/* SPDX-License-Identifier: MIT\n * Unchanged NVK layouts/constants. */\n#include <stddef.h>\n"
    for name in ("NVK_MAX_SETS", "NVK_MAX_PUSH_SIZE", "NVK_MAX_DYNAMIC_BUFFERS",
                 "NVK_MAX_SAMPLES", "NVK_HW_ROOT_TABLE_SIZE", "NVK_HW_ROOT_TABLE_FIRST_CB"):
        text += re.search(r"^#define " + name + r" .*$", private, re.M)[0] + "\n"
    for name, body in (("nvk_buffer_address", desc), ("nvk_root_descriptor_table", root)):
        text += re.search(r"struct " + name + r" \{.*?^\};", body, re.M | re.S)[0] + "\n"
    text += """#define nvk_root_descriptor_offset(m) offsetof(struct nvk_root_descriptor_table, m)
#define nvk_hw_root_table_index(m) (nvk_root_descriptor_offset(m) / NVK_HW_ROOT_TABLE_SIZE)
#define nvk_hw_root_table_offset(m) (nvk_root_descriptor_offset(m) % NVK_HW_ROOT_TABLE_SIZE)
"""
    for name in ("base", "turing_graphics"):
        text += re.search(r"const struct nak_constant_offset_info nak_const_offsets_" +
                          name + r" = \{.*?^\};", shader, re.M | re.S)[0] + "\n"
    (nv / "t214_nvk_constants.h").write_text(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mesa", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--windows-cross-file", type=Path,
                    help="Meson ARM64 Windows LLVM-MinGW/Rust-gnullvm cross file")
    ap.add_argument("--build-only", action="store_true", help="build compiler without running it")
    a = ap.parse_args()
    if a.windows_cross_file and not a.build_only:
        ap.error("--windows-cross-file requires --build-only; execute the compiler in Windows")
    out = a.out.resolve()
    rev = subprocess.check_output(["git", "-C", str(a.mesa), "rev-parse", "HEAD"], text=True).strip()
    if rev != REV:
        ap.error(f"Mesa HEAD must be {REV}")
    # A fresh directory makes the provenance independent of any previous overlay.
    out.mkdir(parents=True, exist_ok=False)
    archive = out / "mesa.tar"
    with archive.open("wb") as f:
        subprocess.run(["git", "-C", str(a.mesa), "archive", REV], stdout=f, check=True)
    src = out / "mesa-src"
    src.mkdir()
    with tarfile.open(archive) as t:
        t.extractall(src, filter="data")
    archive.unlink()
    replace(src / "meson.build", "elif with_nouveau_vk and not with_platform_switch",
            "elif with_nouveau_vk and not with_platform_switch and false # host compiler only")
    replace(src / "meson.build", "if get_option('mesa-clc') == 'system'",
            "with_driver_using_cl = false # no NVK runtime/meta shaders in this host tool\n" +
            "if get_option('mesa-clc') == 'system'")
    (src / "src/nouveau/meson.build").write_text("""# Compiler-only host overlay; compiler algorithms are unchanged.
subdir('headers')
subdir('rust')
idep_nouveau_ws = declare_dependency()
subdir('nil')
subdir('compiler')
executable('t214-shader-compiler', files('t214_shader_compile.c'),
  include_directories: [inc_include, inc_src],
  dependencies: [idep_nak, idep_vtn, idep_mesautil, idep_compiler, dep_thread, dep_dl, dep_m],
  link_with: [_libnak_rs], install: false)
""")
    # Only cfg(test) hardware runners need the Linux winsys bindings.
    (src / "src/nouveau/compiler/nak_bindings.h").write_text('#include "nak_private.h"\n')
    shutil.copyfile(HERE / "nak_shader_compile.c", src / "src/nouveau/t214_shader_compile.c")
    constants(src)
    if a.windows_cross_file:
        run("git", "-C", src, "apply", HERE / "mesa-windows-nak.patch")
    build = out / "build"
    cross_args = []
    if a.windows_cross_file:
        cross_args = ["--cross-file", a.windows_cross_file.resolve(),
                      "-Dshader-cache=disabled", "-Dzlib=disabled", "-Dzstd=disabled", "-Dexpat=disabled"]
    run("meson", "setup", build, src, "-Dplatforms=[]", "-Dvulkan-drivers=nouveau",
        "-Dgallium-drivers=[]", "-Dglx=disabled", "-Degl=disabled", "-Dgbm=disabled",
        "-Dllvm=disabled", "-Dbuild-tests=false", "-Dvideo-codecs=[]", "-Dvalgrind=disabled",
        "-Dlibunwind=disabled", "-Dgles1=disabled", "-Dgles2=disabled", "-Dopengl=false",
        "-Dbuildtype=release", *cross_args)
    compiler_name = "t214-shader-compiler.exe" if a.windows_cross_file else "t214-shader-compiler"
    run("ninja", "-C", build, "src/nouveau/" + compiler_name)
    compiler = build / "src/nouveau" / compiler_name
    if a.build_only:
        print(f"Built {compiler}; shader generation/execution has not been verified by this command")
        return
    manifest = {"mesa": REV, "scope": "fixed diagnostic shaders; no ICD or Vulkan API pass", "files": {}}
    blobs = []
    infos = []
    for stage, suffix in (("vs", "vert"), ("fs", "frag")):
        source = HERE / "shaders" / ("rect." + suffix)
        prefix = out / ("rect-" + stage)
        spv = prefix.with_suffix(".spv")
        run("glslangValidator", "-V", "--target-env", "vulkan1.0", "-S", suffix,
            "-o", spv, source)
        run("spirv-val", "--target-env", "vulkan1.0", spv)
        run(compiler, stage, spv, prefix)
        info = json.loads(prefix.with_suffix(".json").read_text())
        header = prefix.with_suffix(".header.bin").read_bytes()
        code = prefix.with_suffix(".code.bin").read_bytes()
        if (info["sm"] != 53 or len(header) != 80 or len(code) != info["code_size"] or
                len(code) % 32 or len(code) > 4096 - 128 or info["slm_size"] or info["crs_size"]):
            raise RuntimeError("unsupported shader output")
        blobs.append(header + code)
        infos.append(info)
        for f in (source, spv, prefix.with_suffix(".header.bin"), prefix.with_suffix(".code.bin")):
            manifest["files"][f.name] = hashlib.sha256(f.read_bytes()).hexdigest()
    # FNV-1a over the uploaded header/code words, matching the KMD loader check.
    fnv = 2166136261
    for blob in blobs:
        for word in struct.unpack("<" + "I" * (len(blob) // 4), blob):
            fnv = ((fnv ^ word) * 16777619) & 0xffffffff
    header = ["/* SPDX-License-Identifier: MIT", f" * Generated by pinned Mesa NAK {REV}.",
              " * Reproduce with tools/build_draw_shaders.py; do not edit. */",
              "#ifndef TG_GPU_DRAW_SM53_H", "#define TG_GPU_DRAW_SM53_H",
              f"#define TG_GPU_DRAW_CODE_HASH 0x{fnv:08x}u"]
    for stage, info in zip(("VS", "FS"), infos):
        header += [f"#define TG_GPU_DRAW_{stage}_CODE_BYTES {info['code_size']}u",
                   f"#define TG_GPU_DRAW_{stage}_GPRS {info['gprs']}u"]
    header += ["#ifdef TG_GPU_DRAW_INCLUDE_CODE"]
    for stage, blob in zip(("vs", "fs"), blobs):
        words = struct.unpack("<" + "I" * (len(blob) // 4), blob)
        header += [f"static const uint32_t tg_gpu_draw_{stage}[] = {{"]
        header += ["    " + ",".join(f"0x{w:08x}u" for w in words[i:i+4]) + ","
                   for i in range(0, len(words), 4)]
        header += ["};"]
    header += ["#endif", "#endif", ""]
    (out / "draw_sm53.h").write_text("\n".join(header))
    manifest["code_hash"] = f"{fnv:08x}"
    manifest["shaders"] = infos
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Generated {out / 'draw_sm53.h'}; code hash {fnv:08x}")


if __name__ == "__main__":
    main()

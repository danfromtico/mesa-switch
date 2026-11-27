#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cross-build the upstream Win32 vkcube without modifying its renderer.

Pass a Khronos Vulkan-Tools checkout, matching Vulkan-Headers include directory,
and the ARM64 LLVM-MinGW C compiler. The executable dynamically loads the
Windows Vulkan loader; place vulkan-1.dll alongside it for an app-local test.
Run Run-vkcube.cmd to apply the GM20B opt-in to this process only.
This tool neither downloads dependencies nor installs a driver.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timing", action="store_true",
                        help="Record Present return timing and swapchain recreation in a separate test build")
    args = parser.parse_args()
    source, headers = args.source.resolve(), args.headers.resolve()
    out = args.out.resolve()
    cube = source / "cube"
    if not (cube / "cube_functions.h").is_file():
        parser.error("Expected Vulkan-Tools with the dynamically loaded cube_functions.h")
    if not (headers / "vulkan/vulkan.h").is_file():
        parser.error("--headers must contain vulkan/vulkan.h")
    revision = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--", "cube"]):
        parser.error("Use an unmodified upstream cube directory")
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "vkcube.exe"
    build_source = cube / "cube.c"
    timing_header = Path(__file__).resolve().parent / "vkcube_timing.h"
    if args.timing:
        replacements = {
            "static void demo_draw(struct demo *demo) {":
                '#include "vkcube_timing.h"\n\nstatic void demo_draw(struct demo *demo) {',
            "    err = vkQueuePresentKHR(demo->present_queue, &present);":
                "    const uint64_t timing_begin = cube_timing_now();\n"
                "    err = vkQueuePresentKHR(demo->present_queue, &present);\n"
                "    cube_timing_present(demo->window, timing_begin, err);",
            "    err = vkCreateSwapchainKHR(demo->device, &swapchain_ci, NULL, &demo->swapchain);":
                "    err = vkCreateSwapchainKHR(demo->device, &swapchain_ci, NULL, &demo->swapchain);\n"
                '    cube_timing_event("swapchain", 0, err, swapchain_ci.imageExtent.width, swapchain_ci.imageExtent.height);',
        }
        instrumented = build_source.read_text()
        for old, new in replacements.items():
            if instrumented.count(old) != 1:
                parser.error("Upstream cube timing insertion point changed: " + old)
            instrumented = instrumented.replace(old, new)
        build_source = out / "cube-timing.c"
        build_source.write_text(instrumented)
        shutil.copyfile(timing_header, out / timing_header.name)
    # LLVM-MinGW's target name is a symlink to a wrapper which uses argv[0].
    command = [str(args.cc.absolute()), "-std=c11", "-O2", "-static", "-mwindows",
               "-DVK_USE_PLATFORM_WIN32_KHR", "-I" + str(headers),
               "-I" + str(cube), str(build_source), "-lgdi32", "-lshell32", "-o", str(exe)]
    subprocess.run(command, check=True)
    launchers = [out / name for name in ("Run-vkcube.ps1", "Run-vkcube.cmd")]
    for launcher in launchers:
        shutil.copyfile(Path(__file__).resolve().parent / launcher.name, launcher)
    shutil.copyfile(source / "LICENSE.txt", out / "vkcube-LICENSE.txt")
    notice = (cube / "cube.c").read_text().split("*/", 1)[0] + "*/\n"
    (out / "vkcube-NOTICE.txt").write_text(notice +
        "\nKhronos Vulkan-Tools vkcube, commit " + revision +
        ("\nLocal changes: Present timing and swapchain event recording only; see cube-timing.c and vkcube_timing.h."
         if args.timing else "\nUpstream renderer unmodified.") +
        "\nhttps://github.com/KhronosGroup/Vulkan-Tools\n")
    inputs = [cube / name for name in (
        "cube.c", "cube_functions.h", "linmath.h", "gettime.h",
        "object_type_string_helper.h", "lunarg.ppm.h", "cube.vert.inc", "cube.frag.inc")]
    (out / "vkcube-manifest.json").write_text(json.dumps({
        "upstream": "https://github.com/KhronosGroup/Vulkan-Tools",
        "revision": revision, "renderer_modified": args.timing,
        "instrumentation": "Present return timing and swapchain creation; not display completion" if args.timing else None,
        "compiled_source_sha256": hashlib.sha256(build_source.read_bytes()).hexdigest(),
        "timing_header_sha256": hashlib.sha256(timing_header.read_bytes()).hexdigest() if args.timing else None,
        "command": command,
        "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
        "vulkan_core_h_sha256": hashlib.sha256((headers / "vulkan/vulkan_core.h").read_bytes()).hexdigest(),
        "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "launcher_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in launchers},
        "launch": "Run-vkcube.cmd (process-local GM20B NVK opt-in, 320x240 FIFO)",
        "scope": "Build only; native rendering, Win32 presentation and device identity need live verification",
    }, indent=2) + "\n")
    print(f"Built {'timing-instrumented' if args.timing else 'unmodified upstream'} vkcube at {revision}: {exe}")


if __name__ == "__main__":
    main()

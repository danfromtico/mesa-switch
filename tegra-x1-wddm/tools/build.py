#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the ARM64 GPU backend objects and run its host format tests.

Builds the static backend and optional render-only or KMDF probe packages.
Use LLVM, the Mesa Switch source tree and official WDK/SDK Include roots.
"""
import argparse
import datetime
import hashlib
from pathlib import Path
import re
import shutil
import subprocess

MESA_REVISION = "0ff52079ca9e9e837ed27eb95a8d239f439a1864"
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--mesa", type=Path, default=root.parent)
parser.add_argument("--wdk-include", required=True, type=Path)
parser.add_argument("--sdk-include", required=True, type=Path)
parser.add_argument("--out", required=True, type=Path)
parser.add_argument("--cc", default="clang")
parser.add_argument("--clang-cl", default="clang-cl")
parser.add_argument("--lib", default="llvm-lib")
parser.add_argument("--link", default="lld-link")
parser.add_argument("--dlltool", default="llvm-dlltool")
parser.add_argument("--probe", action="store_true", help="also build the opt-in KMDF probe and its SwitchFan bridge")
miniport = parser.add_mutually_exclusive_group()
miniport.add_argument("--render", action="store_true", help="build the opt-in MCDM package")
miniport.add_argument("--display", action="store_true", help="build the opt-in WDDM internal panel package")
parser.add_argument("--gr-firmware", type=Path, help="external NVIDIA tegra21x NETB_img.bin/fecs.bin/gpccs.bin directory")
parser.add_argument("--acr-firmware", type=Path, help="external NVIDIA 32.7.6 ACR/PMU/FECS files")
parser.add_argument("--keep-fan", action="store_true", help="with --probe, preserve the existing signed SwitchFan package")
parser.add_argument("--windows-tests", action="store_true", help="build the real-WDK paging test executable for ARM64 Windows")
parser.add_argument("--native-test-version", type=int, metavar="N",
                    help="with --display, build gated native submission as package 0.1.0.N")
parser.add_argument("--paging-stop-probe", action="store_true",
                    help="capture one unsupported paging return on the stop screen in a native trial")
parser.add_argument("--render-gpu-khz", type=int, choices=(460800, 729600), default=729600,
                    help="fixed GPU clock; 460800 is an opt-in display power control")
parser.add_argument("--nvdec-firmware", type=Path,
                    help="external nvidia/tegra210/nvdec.bin for NVDEC host tests")
parser.add_argument("--nvdec-sample", type=Path,
                    help="720p H.264 Annex B sample for NVDEC parser/command tests")
args = parser.parse_args()
if args.native_test_version is not None and (not args.display or not 1 <= args.native_test_version <= 65535):
    parser.error("--native-test-version requires --display and a version from 1 to 65535")
if args.paging_stop_probe and args.native_test_version is None:
    parser.error("--paging-stop-probe requires --native-test-version")
if args.keep_fan and not args.probe:
    parser.error("--keep-fan requires --probe")
if args.render_gpu_khz != 729600 and not args.display:
    parser.error("the low-power render clock requires --display")
build_miniport = args.render or args.display
gr_names=("NETB_img.bin", "fecs.bin", "gpccs.bin")
if (args.probe or build_miniport) and (not args.gr_firmware or
    any(not (args.gr_firmware / n).is_file() for n in gr_names)):
    parser.error("--probe, --render and --display require --gr-firmware with the three external NVIDIA GR files")
acr_names=("nv_acr_ucode_prod.bin", "pmu_bl.bin", "gpmu_ucode_image.bin",
           "gpmu_ucode_desc.bin", "pmu_sig.bin", "fecs_sig.bin")
if (args.probe or build_miniport) and (not args.acr_firmware or
    any(not (args.acr_firmware / n).is_file() for n in acr_names)):
    parser.error("--probe, --render and --display require --acr-firmware with all six external NVIDIA ACR files")
mesa_base = subprocess.run(
    ["git", "--no-lazy-fetch", "-C", str(args.mesa), "merge-base", "--is-ancestor", MESA_REVISION, "HEAD"],
    check=False)
if mesa_base.returncode != 0:
    parser.error(f"Mesa must contain the tested mesa-switch base {MESA_REVISION}")
args.out.mkdir(parents=True, exist_ok=True)
mesa_src = args.mesa / "src"
mesa_includes = [mesa_src, mesa_src / "nouveau/headers",
                 mesa_src / "nouveau/headers/nvidia/classes"]
encoder = mesa_src / "nouveau/horizon/nouveau_horizon_gm20b.c"
command_stream = [root / "core/command_stream.c", root / "core/copy_stream.c", root / "core/present_halfturn.c"]
core = [root / "core/gm20b_mmu.c", root / "core/report.c", root / "core/gpfifo.c", root / "core/fault.c",
        root / "core/execute.c", root / "core/submission.c", root / "core/channel.c",
        *command_stream, root / "core/paging_dma.c", root / "core/present_blit.c",
        root / "platform/t214_mmu.c", root / "platform/t214_frontend.c",
        root / "platform/t214_gr.c", root / "platform/t214_acr.c", root / "platform/t214_pmu.c",
        root / "core/gr_firmware.c", root / "core/acr_firmware.c", root / "core/gr_image.c",
        root / "core/gr_submit.c", root / "core/gr_clear.c", root / "core/gr_draw.c",
        root / "platform/t214_gr_context.c", root / "platform/t214_interrupt.c", encoder]
subprocess.run(["python3", str(root / "tests/test_debug_report.py")], check=True)
subprocess.run(["python3", str(root / "tests/test_recovery_decode.py")], check=True)
host = args.out / "test-gm20b-core"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                *[f"-I{p}" for p in mesa_includes], *map(str, core), str(root / "tests/test_core.c"),
                str(root / "tests/test_execute_init.c"),
                str(root / "tests/test_boot_bar1.c"),
                "-o", str(host)], check=True)
subprocess.run([str(host.resolve())], check=True)
stream_test=args.out / "test-command-stream"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                *[f"-I{p}" for p in mesa_includes],*map(str,command_stream),
                str(root / "tests/test_command_stream.c"),"-o",str(stream_test)],check=True)
subprocess.run([str(stream_test.resolve())],check=True)
present_test=args.out / "test-present-blit"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                *[f"-I{p}" for p in mesa_includes],*map(str,command_stream),
                str(root / "core/present_blit.c"),str(root / "tests/test_present_blit.c"),
                "-o",str(present_test)],check=True)
subprocess.run([str(present_test.resolve())],check=True)
power = root / "platform/t214_power.c"
power_test = args.out / "test-t214-power"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(power), str(root / "tests/test_power.c"), "-o", str(power_test)], check=True)
subprocess.run([str(power_test.resolve())], check=True)
host_nt_shim = args.out / "host-nt-shim"
host_nt_shim.mkdir(parents=True, exist_ok=True)
for hdr in ("ntifs.h", "initguid.h", "wdmguid.h"):
    (host_nt_shim / hdr).write_text("/* host pthread shim */\n")
platform_threads_test = args.out / "test-platform-threads"
subprocess.run([args.cc, "-std=c11", "-O2", "-fshort-wchar", "-Wall", "-Wextra",
                "-Wno-unused-function", "-Werror", "-pthread",
                f"-I{host_nt_shim}", f"-I{root}",
                str(root / "tests/test_platform_threads.c"),
                "-o", str(platform_threads_test)], check=True)
subprocess.run([str(platform_threads_test.resolve())], check=True)
clock = root / "platform/t214_clock.c"
clock_test = args.out / "test-t214-clock"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(clock), str(root / "tests/test_clock.c"), "-o", str(clock_test)], check=True)
subprocess.run([str(clock_test.resolve())], check=True)
dc_test = args.out / "test-dc-portrait"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_dc.c"), str(root / "tests/test_dc_portrait.c"),
                "-o", str(dc_test)], check=True)
subprocess.run([str(dc_test.resolve())], check=True)
frontend_test = args.out / "test-t214-frontend"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_frontend.c"), str(root / "tests/test_frontend.c"),
                "-o", str(frontend_test)], check=True)
subprocess.run([str(frontend_test.resolve())], check=True)
interrupt_test=args.out / "test-interrupt"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                str(root / "platform/t214_interrupt.c"),str(root / "tests/test_interrupt.c"),
                "-o",str(interrupt_test)],check=True)
subprocess.run([str(interrupt_test.resolve())],check=True)
tlb = root / "platform/t214_mmu.c"
tlb_test = args.out / "test-tlb"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(tlb), str(root / "tests/test_tlb.c"), "-o", str(tlb_test)], check=True)
subprocess.run([str(tlb_test.resolve())], check=True)
channel_test=args.out / "test-channel"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                str(root / "core/channel.c"),str(root / "core/fault.c"),str(tlb),
                str(root / "platform/t214_frontend.c"),str(root / "tests/test_channel.c"),
                "-o",str(channel_test)],check=True)
subprocess.run([str(channel_test.resolve())],check=True)

# --- NVDEC H.264 backend control tests -------------------------------------
nvdec_dir = mesa_src / "nouveau/nvdec"
nvdec_core = [nvdec_dir / "nvdec_sha256.c", nvdec_dir / "nvdec_fw.c",
              nvdec_dir / "nvdec_h264.c", nvdec_dir / "nvdec_engine.c"]
nvdec_inc = [*mesa_includes, nvdec_dir]
nvdec_platform_test = args.out / "test-nvdec-platform"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                f"-I{root / 'platform'}",
                str(root / "platform/t214_nvdec.c"),
                str(root / "tests/test_nvdec_platform.c"),
                "-o", str(nvdec_platform_test)], check=True)
subprocess.run([str(nvdec_platform_test.resolve())], check=True)
if args.nvdec_firmware:
    nvdec_model_test = args.out / "test-nvdec-model"
    subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    *[f"-I{p}" for p in nvdec_inc],
                    str(root / "tests/test_nvdec.c"),
                    *map(str, nvdec_core),
                    "-o", str(nvdec_model_test)], check=True)
    subprocess.run([str(nvdec_model_test.resolve()),
                    str(args.nvdec_firmware.resolve())], check=True)
else:
    print("NVDEC firmware model test skipped: --nvdec-firmware not provided")
if args.nvdec_sample:
    nvdec_h264_test = args.out / "test-nvdec-h264"
    subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    *[f"-I{p}" for p in nvdec_inc],
                    str(root / "tests/test_nvdec_h264.c"),
                    str(nvdec_dir / "nvdec_h264.c"), str(nvdec_dir / "nvdec_parse.c"),
                    "-o", str(nvdec_h264_test)], check=True)
    subprocess.run([str(nvdec_h264_test.resolve()),
                    str(args.nvdec_sample.resolve())], check=True)
else:
    print("NVDEC H.264 control test skipped: --nvdec-sample not provided")

# --- tgd3d D3D11VA mapping and NV12 CSC controls ---------------------------
csc_test = args.out / "test-tgd3d-csc"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                f"-I{mesa_src / 'nouveau/tgd3d'}",
                str(root / "tests/test_tgd3d_csc.c"),
                str(mesa_src / "nouveau/tgd3d/tgd3d_csc.c"),
                "-o", str(csc_test), "-lm"], check=True)
subprocess.run([str(csc_test.resolve())], check=True)
h264_map_test = args.out / "test-tgd3d-h264-map"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                f"-I{mesa_src / 'nouveau/tgd3d'}", f"-I{nvdec_dir}",
                str(root / "tests/test_tgd3d_h264_map.c"),
                str(mesa_src / "nouveau/tgd3d/tgd3d_h264_map.c"),
                "-o", str(h264_map_test)], check=True)
subprocess.run([str(h264_map_test.resolve())], check=True)
if shutil.which("glslangValidator") and shutil.which("spirv-val"):
    for stage in ("vert", "frag"):
        spv = args.out / ("nv12." + stage + ".spv")
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.1",
                        "-o", str(spv), str(mesa_src / "nouveau/tgd3d/shaders" / ("nv12." + stage))],
                       check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(spv)], check=True)
    print("NV12 CSC SPIR-V validated")
else:
    print("NV12 CSC SPIR-V validation skipped: glslangValidator/spirv-val unavailable")
gr_firmware = root / "core/gr_firmware.c"
gr_firmware_test = args.out / "test-gr-firmware"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(gr_firmware), str(root / "tests/test_gr_firmware.c"),
                "-o", str(gr_firmware_test)], check=True)
subprocess.run([str(gr_firmware_test.resolve())], check=True)
acr_test = args.out / "test-acr-firmware"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "core/acr_firmware.c"), str(gr_firmware),
                str(root / "tests/test_acr_firmware.c"), "-o", str(acr_test)], check=True)
subprocess.run([str(acr_test.resolve()), *([str(args.acr_firmware.resolve())] if args.acr_firmware else [])], check=True)
acr_boot_test = args.out / "test-acr-boot"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_acr.c"), str(root / "tests/test_acr_boot.c"),
                "-o", str(acr_boot_test)], check=True)
subprocess.run([str(acr_boot_test.resolve())], check=True)
pmu_test = args.out / "test-pmu-boot"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_pmu.c"), str(root / "tests/test_pmu_boot.c"),
                "-o", str(pmu_test)], check=True)
subprocess.run([str(pmu_test.resolve())], check=True)
gr = root / "platform/t214_gr.c"
gr_test = args.out / "test-gr-boot"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(gr), str(gr_firmware), str(root / "platform/t214_frontend.c"),
                str(root / "tests/test_gr_boot.c"), "-o", str(gr_test)], check=True)
subprocess.run([str(gr_test.resolve())], check=True)
gr_image_test = args.out / "test-gr-image"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                str(root / "core/gr_image.c"),str(root / "tests/test_gr_image.c"),
                "-o",str(gr_image_test)],check=True)
subprocess.run([str(gr_image_test.resolve())],check=True)
gr_clear_test = args.out / "test-gr-clear"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                *[f"-I{p}" for p in mesa_includes],
                str(root / "core/gr_clear.c"),str(root / "core/report.c"),*map(str,command_stream),str(encoder),
                str(root / "tests/test_gr_clear.c"),"-o",str(gr_clear_test)],check=True)
subprocess.run([str(gr_clear_test.resolve())],check=True)
gr_draw_test=args.out / "test-gr-draw"
subprocess.run([args.cc,"-std=c11","-O2","-Wall","-Wextra","-Werror",
                *[f"-I{p}" for p in mesa_includes],
                str(root / "core/gr_draw.c"),str(root / "core/report.c"),*map(str,command_stream),str(encoder),
                str(root / "tests/test_gr_draw.c"),"-o",str(gr_draw_test)],check=True)
subprocess.run([str(gr_draw_test.resolve())],check=True)
gr_context_test = args.out / "test-gr-context"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_gr_context.c"),str(root / "core/gr_image.c"), str(gr), str(gr_firmware),
                str(root / "platform/t214_frontend.c"), str(root / "core/gm20b_mmu.c"),
                str(root / "tests/test_gr_context.c"), "-o", str(gr_context_test)], check=True)
subprocess.run([str(gr_context_test.resolve()),
                *([str((args.gr_firmware / "NETB_img.bin").resolve())] if args.gr_firmware else [])], check=True)

includes = [args.wdk_include / "km/crt", args.wdk_include / "km",
            args.wdk_include / "shared", args.sdk_include / "shared",
            args.sdk_include / "ucrt"]
windows_abi = root.parent / "src/nouveau/vulkan/nvkmd/windows"
flags = ["--target=aarch64-pc-windows-msvc", "/nologo", "/c", "/kernel",
         "/GS-", "/Gy", "/Zl", "/O2", "/W4", "/WX", "/std:c11",
         "/clang:-ffreestanding", "-Wno-microsoft-anon-tag", "-Wno-pragma-pack",
         "-Wno-ignored-pragma-intrinsic", "-Wno-nonportable-include-path",
         "/D_ARM64_", "/DARM64", "/D_WIN64", "/D_WIN32_WINNT=0x0A00",
         "/DNTDDI_VERSION=0x0A00000C", "/DNDEBUG",
         f"/DTG_GPU_RENDER_KHZ={args.render_gpu_khz}",
         "/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_6",
         f"/I{windows_abi}", *[f"/I{p}" for p in mesa_includes],
         *[f"/imsvc{p}" for p in includes]]
objects = []
for source in [*core, power, clock, *nvdec_core, root / "platform/t214_nvdec.c",
               root / "windows/nvdec_backend.c", root / "windows/nvdec_adapter.c",
               root / "platform/t214_dc.c", root / "windows/resources.c",
               root / "windows/panel.c", root / "windows/panel_vidpn.c",
               root / "windows/panel_scanout.c",
               root / "windows/private_memory.c", root / "windows/nvdec_memory.c", root / "windows/paging.c",
               root / "windows/table_segment.c", root / "windows/root_table.c",
               root / "windows/paging_buffer.c",root / "windows/paging_dispatch.c",
               root / "windows/paging_submit.c",root / "windows/context.c",
               root / "windows/device_lifetime.c",
               root / "windows/internal_va.c", root / "windows/submit_worker.c",
               root / "windows/graphics.c",root / "windows/hardware.c",
               root / "windows/work_queue.c", root / "windows/allocation.c",
               root / "windows/query.c", root / "windows/adapter.c",
               root / "windows/submit_ddi.c",root / "windows/present.c",root / "windows/command_snapshot.c",root / "windows/recovery.c",
               root / "windows/fault_isr.c",root / "windows/diagnostic_events.c",
               root / "windows/platform_io.c",root / "windows/host1x_clock.c",root / "windows/dvfs.c",root / "windows/worker.c",
               root / "windows/platform_adapter.c",root / "windows/gr_firmware_io.c",
               root / "windows/reset_backend.c",root / "windows/adapter_reset.c",
               root / "windows/startup_diagnostic.c"]:
    obj = args.out / (source.stem + ".obj")
    subprocess.run([args.clang_cl, *flags, f"/Fo{obj}", "--", str(source)], check=True)
    objects.append(obj)
library = args.out / "TegraGpuCore.lib"
subprocess.run([args.lib, "/nologo", "/machine:arm64", f"/out:{library}",
                *map(str, objects)], check=True)
print(f"ARM64 kernel library: {library} (hardware execution not tested)")

if args.windows_tests:
    test_obj = args.out / "test_paging_windows.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{test_obj}", "--",
                    str(root / "tests/test_paging_windows.c")], check=True)
    # The SDK header NuGet does not ship import libraries. Use the same LLVM
    # import-library construction as the existing performance tool.
    exports = args.out / "paging-kernel32.def"
    exports.write_text("LIBRARY kernel32.dll\nEXPORTS\nExitProcess\nGetStdHandle\nWriteFile\nGetProcessHeap\nHeapAlloc\nHeapFree\n")
    sdk_lib = args.out / "paging-kernel32.lib"
    subprocess.run([args.dlltool, "-m", "arm64", "-d", str(exports), "-l", str(sdk_lib)], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-paging.exe'}", str(test_obj),
                    str(args.out / "paging.obj"), str(args.out / "gm20b_mmu.obj"),
                    str(args.out / "table_segment.obj"), str(args.out / "private_memory.obj"),
                    str(args.out / "root_table.obj"), str(args.out / "t214_mmu.obj"),
                    str(args.out / "paging_buffer.obj"), str(args.out / "paging_dma.obj"),
                    str(args.out / "resources.obj"), str(args.out / "t214_dc.obj"),
                    str(sdk_lib)], check=True)
    nvdec_memory_test = args.out / "test_nvdec_memory_windows.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{nvdec_memory_test}", "--",
                    str(root / "tests/test_nvdec_memory_windows.c")], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-nvdec-memory.exe'}", str(nvdec_memory_test),
                    str(args.out / "nvdec_memory.obj"), str(args.out / "private_memory.obj"),
                    str(sdk_lib)], check=True)
    nvdec_adapter_test = args.out / "test_nvdec_adapter_windows.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{nvdec_adapter_test}", "--",
                    str(root / "tests/test_nvdec_adapter_windows.c")], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-nvdec-adapter.exe'}", str(nvdec_adapter_test),
                    str(args.out / "nvdec_adapter.obj"), str(args.out / "device_lifetime.obj"),
                    str(args.out / "t214_nvdec.obj"), str(sdk_lib)], check=True)
    platform_io_model = args.out / "platform_io_model.obj"
    subprocess.run([args.clang_cl, *flags, "/DRELOCATABLE_USER_SHARED_DATA",
                    "/clang:-funwind-tables", f"/Fo{platform_io_model}", "--",
                    str(root / "windows/platform_io.c")], check=True)
    context_test = args.out / "test_context_windows.obj"
    inactive_video = args.out / "nvdec_inactive_fixture.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{inactive_video}", "--",
                    str(root / "tests/nvdec_inactive_fixture.c")], check=True)
    subprocess.run([args.clang_cl, *flags, "/DRELOCATABLE_USER_SHARED_DATA",
                    "/clang:-funwind-tables", f"/Fo{context_test}", "--",
                    str(root / "tests/test_context_windows.c")], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-context.exe'}", str(context_test),
                    str(inactive_video), str(platform_io_model), str(library), str(sdk_lib)], check=True)
    timings_test = args.out / "test_panel_timings_windows.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{timings_test}", "--",
                    str(root / "tests/test_panel_timings_windows.c")], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-panel-timings.exe'}", str(timings_test),
                    str(args.out / "panel_vidpn.obj"), str(sdk_lib)], check=True)
    # Keep real WDK layouts/backend code, substituting only shared clock data
    # so synchronous latch waits can execute in a user-mode ARM64 fixture.
    scanout_models = []
    for source in [root / "tests/test_panel_scanout_windows.c",
                   root / "windows/panel.c", root / "windows/panel_scanout.c",
                   root / "windows/fault_isr.c"]:
        obj = args.out / (source.stem + "_model.obj")
        subprocess.run([args.clang_cl, *flags, "/DRELOCATABLE_USER_SHARED_DATA",
                        "/clang:-funwind-tables", f"/Fo{obj}", "--", str(source)], check=True)
        scanout_models.append(obj)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-panel-scanout.exe'}", *map(str, scanout_models),
                    str(args.out / "t214_dc.obj"), str(sdk_lib)], check=True)
    hardware_test=args.out / "test_hardware_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{hardware_test}","--",
                    str(root / "tests/test_hardware_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-hardware.exe'}",str(hardware_test),str(args.out / "hardware.obj"),
                    str(args.out / "reset_backend.obj"),str(args.out / "fault_isr.obj"),
                    str(args.out / "private_memory.obj"),str(sdk_lib)],check=True)
    platform_io_img2 = args.out / "platform_io_img2.obj"
    img2_renames = [f"/D{sym}=Img2_{sym}" for sym in (
        "TgGpuSetPlatformDriverObject", "TgGpuOpenPlatform", "TgGpuClosePlatform",
        "TgGpuPlatformRequest", "TgGpuPlatformTryRequest", "TgGpuPlatformTryVoteRequest",
        "TgGpuReferencePlatformBridge", "TgGpuPlatformPreflight", "TgGpuPlatformAbandonRail",
        "TgGpuPlatformRefreshIsolation", "TgGpuHandOffUnresolvedRail",
        "TgGpuRetryUnresolvedRailRelease", "TgGpuQueryUnresolvedRail", "TgGpuPlatformIo")]
    subprocess.run([args.clang_cl, *flags, "/DRELOCATABLE_USER_SHARED_DATA", *img2_renames,
                    "/clang:-funwind-tables", f"/Fo{platform_io_img2}", "--",
                    str(root / "windows/platform_io.c")], check=True)
    platform_test=args.out / "test_platform_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/DRELOCATABLE_USER_SHARED_DATA","/clang:-funwind-tables",
                    f"/Fo{platform_test}","--",str(root / "tests/test_platform_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-platform.exe'}",str(platform_test),
                    str(platform_io_model),str(platform_io_img2),str(sdk_lib)],check=True)
    clock_test=args.out / "test_host1x_clock_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",
                    "-Wno-unused-function","-Wno-unused-variable","-Wno-unused-but-set-global",
                    f"/Fo{clock_test}","--",str(root / "tests/test_host1x_clock_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-host1x-clock.exe'}",str(clock_test),
                    str(args.out / "host1x_clock.obj"),str(sdk_lib)],check=True)
    worker_test=args.out / "test_worker_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{worker_test}","--",
                    str(root / "tests/test_worker_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-worker.exe'}",str(worker_test),
                    str(args.out / "worker.obj"),str(sdk_lib)],check=True)
    dvfs_test=args.out / "test_dvfs_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/DRELOCATABLE_USER_SHARED_DATA",f"/I{root}","/clang:-funwind-tables",
                    f"/Fo{dvfs_test}","--",str(root / "tests/test_dvfs_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-dvfs.exe'}",str(dvfs_test),
                    str(args.out / "dvfs.obj"),str(args.out / "t214_clock.obj"),
                    str(platform_io_model),str(sdk_lib)],check=True)
    diagnostic_test=args.out / "test_diagnostic_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{diagnostic_test}","--",
                    str(root / "tests/test_diagnostic_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-diagnostic.exe'}",str(diagnostic_test),str(sdk_lib)],check=True)
    native_start_test=args.out / "test_native_start_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{native_start_test}","--",
                    str(root / "tests/test_native_start_windows.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-native-start.exe'}",str(native_start_test),str(sdk_lib)],check=True)
    startup_diagnostic_test=args.out / "test_startup_diagnostic_windows.obj"
    subprocess.run([args.clang_cl,*flags,"/clang:-funwind-tables",f"/Fo{startup_diagnostic_test}","--",
                    str(root / "tests/test_startup_diagnostic_windows.c")],check=True)
    startup_diagnostic_model=args.out / "startup_diagnostic_model.obj"
    subprocess.run([args.clang_cl,*flags,"/UDBG","/clang:-funwind-tables",
                    f"/Fo{startup_diagnostic_model}","--",
                    str(root / "windows/startup_diagnostic.c")],check=True)
    subprocess.run([args.link,"/nologo","/machine:arm64","/subsystem:console,10.0","/entry:start",
                    "/include:_load_config_used","/nodefaultlib","/release","/opt:ref","/timestamp:0",
                    f"/out:{args.out / 'test-startup-diagnostic.exe'}",str(startup_diagnostic_test),
                    str(startup_diagnostic_model),str(sdk_lib)],check=True)

def copy_firmware(dest):
    for filename in gr_names:
        shutil.copyfile(args.gr_firmware / filename, dest / filename)
    for filename in acr_names:
        shutil.copyfile(args.acr_firmware / filename, dest / filename)

if build_miniport:
    dest = args.out / "kit/tegragpurender"
    dest.mkdir(parents=True, exist_ok=True)
    obj = args.out / "render_miniport.obj"
    km_lib = args.wdk_include.parent.parent / "Lib" / args.wdk_include.name / "km/ARM64"
    miniport_flags = ["/DTG_GPU_PANEL_WDDM=1"] if args.display else []
    if args.paging_stop_probe:
        miniport_flags.append("/DTG_GPU_PAGING_STOP_PROBE=1")
    test_objects = []
    if args.native_test_version is not None:
        miniport_flags.append(f"/DTG_GPU_NATIVE_TEST_VERSION={args.native_test_version}")
        miniport_flags.append("/DTG_GPU_PAGING_DIAGNOSTICS=1")
        gate = args.out / "native_start_test.obj"
        subprocess.run([args.clang_cl,*flags,*miniport_flags,f"/Fo{gate}","--",
                        str(root / "windows/native_start_test.c")],check=True)
        test_objects.append(gate)
    subprocess.run([args.clang_cl, *flags, *miniport_flags, "/GS", f"/Fo{obj}", "--",
                    str(root / "windows/render_miniport.c")], check=True)
    subprocess.run([args.link, "/nologo", "/driver", "/subsystem:native,10.0", "/machine:arm64",
                    "/entry:GsDriverEntry", "/include:_load_config_used", "/nodefaultlib", "/release",
                    "/opt:ref", "/tsaware:no", "/section:.text,ERP", "/section:.rdata,RP",
                    "/section:.data,RWP", "/section:.pdata,RP", "/section:INIT,ERD", "/timestamp:0",
                    f"/out:{dest / 'tegragpurender.sys'}", str(obj), *map(str,test_objects), str(library),
                    *[str(km_lib / name) for name in ("displib.lib", "ntoskrnl.lib", "hal.lib",
                                                    "bufferoverflowfastfailk.lib", "libcntpr.lib")]], check=True)
    umd = args.out / "no_d3d.obj"
    umd_includes = [args.sdk_include / "um", args.wdk_include / "km/crt",
                    args.wdk_include / "shared", args.sdk_include / "shared",
                    args.sdk_include / "ucrt"]
    subprocess.run([args.clang_cl, "--target=aarch64-pc-windows-msvc", "/nologo", "/c",
                    "/O2", "/GS-", "/Zl", "/W4", "/WX", "/std:c11",
                    "-Wno-nonportable-include-path", "/D_ARM64_",
                    *[f"/imsvc{p}" for p in umd_includes],
                    f"/Fo{umd}", "--", str(root / "windows/no_d3d.c")], check=True)
    subprocess.run([args.link, "/nologo", "/dll", "/noentry", "/machine:arm64", "/nodefaultlib",
                    "/release", "/opt:ref", "/timestamp:0", f"/out:{dest / 'tegragpuno3d.dll'}",
                    f"/implib:{args.out / 'tegragpuno3d.lib'}", str(umd)], check=True)
    inf = root / "windows/tegragpurender.inf"
    if args.display:
        display_inf = inf.read_text().replace("; Opt-in MCDM miniport", "; Opt-in WDDM display miniport")
        display_inf = display_inf.replace("Class=ComputeAccelerator", "Class=Display")
        display_inf = display_inf.replace("{F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}",
                                          "{4d36e968-e325-11ce-bfc1-08002be10318}")
        display_inf = display_inf.replace(",0.1.0.65", ",0.1.0.105")
        if args.native_test_version is not None:
            version=f"{datetime.datetime.now(datetime.timezone.utc):%m/%d/%Y},0.1.0.{args.native_test_version}"
            display_inf=re.sub(r"^DriverVer=.*$",f"DriverVer={version}",display_inf,flags=re.MULTILINE)
        display_inf = display_inf.replace("Switch Lite GM20B Render Adapter (experimental)", "Tegra X1 (GM20B)")
        if args.native_test_version is not None:
            # Driver Store can reuse an identical INF despite changed payloads.
            # Same-version test kernels therefore need distinct package identity.
            build_id = hashlib.sha256((dest / "tegragpurender.sys").read_bytes()).hexdigest()[:12]
            display_inf = re.sub(r"^Device=.*$",
                f'Device="Tegra X1 (GM20B) .{args.native_test_version} {build_id}"',
                display_inf, flags=re.MULTILINE)
        # Keep the driver version and device identity for a firmware-only trial.
        # Windows deduplicates the old package if its INF is byte-identical.
        hs_acr = args.acr_firmware / "nv_acr_ucode_prod.bin"
        if hashlib.sha256(hs_acr.read_bytes()).hexdigest() == "f948c259ca1c9fc8aad3013fe67372243be025b2790016f0ad9e60f7f4534841":
            display_inf = display_inf.replace('Disk="T214 render driver"',
                                              'Disk="T214 render driver - ACR f948c259ca1c"')
        (dest / inf.name).write_text(display_inf)
    else:
        shutil.copyfile(inf, dest / inf.name)
    copy_firmware(dest)
    package = "WDDM display" if args.display else "MCDM"
    print(f"Unsigned {package} package: {dest}; requires catalog generation and signing.")

if args.probe:
    kit = args.out / "kit"
    wdk_root = args.wdk_include.parent.parent
    wdf_inc = args.wdk_include.parent / "wdf/kmdf/1.15"
    wdf_lib = wdk_root / "Lib/wdf/kmdf/ARM64/1.15"
    km_lib = wdk_root / "Lib" / args.wdk_include.name / "km/ARM64"
    bridge = root / "integration/switch-lite/windows/SwitchFan"
    kflags = [*flags, "/DKMDF_VERSION_MAJOR=1", "/DKMDF_VERSION_MINOR=15",
              "/DPOOL_NX_OPTIN=1", "/DSWITCH_LITE_GPU=1", "-Wno-unused-parameter",
              "-Wno-invalid-token-paste", "-Wno-unknown-pragmas", "-Wno-ignored-attributes",
              f"/I{root}", f"/imsvc{wdf_inc}", f"/imsvc{args.wdk_include / 'km/spb/1.0'}"]
    support = args.out / "kmdf_support.obj"
    subprocess.run([args.clang_cl, *kflags, f"/Fo{support}", "--", str(root / "tools/kmdf_support.c")], check=True)
    packages = [("tegragpuprobe", root / "windows/probe.c", root / "windows/tegragpuprobe.inf")]
    if not args.keep_fan:
        packages.append(("switchfan", bridge / "src/switchfan.c", bridge / "switchfan.inf"))
    for name, source, inf in packages:
        dest = kit / name
        dest.mkdir(parents=True, exist_ok=True)
        obj = args.out / (name + ".obj")
        subprocess.run([args.clang_cl, *kflags, f"/Fo{obj}", "--", str(source)], check=True)
        libs = [wdf_lib / "wdfdriverentry.lib", wdf_lib / "wdfldr.lib",
                km_lib / "ntoskrnl.lib", km_lib / "hal.lib", km_lib / "ntstrsafe.lib",
                km_lib / "bufferoverflowfastfailk.lib"]
        if name == "tegragpuprobe":
            diagnostic = args.out / "diagnostic_backend.obj"
            subprocess.run([args.clang_cl, *kflags, f"/Fo{diagnostic}", "--",
                            str(root / "windows/diagnostic_backend.c")], check=True)
            libs.append(diagnostic)
            libs.append(library)
            copy_firmware(dest)
        subprocess.run([args.link, "/nologo", "/driver", "/subsystem:native,10.0", "/machine:arm64",
                        "/entry:FxDriverEntry", "/nodefaultlib", "/release", "/opt:ref", "/opt:icf",
                        "/tsaware:no", "/section:.text,ERP", "/section:.rdata,RP",
                        "/section:.data,RWP", "/section:.pdata,RP", "/section:INIT,ERD",
                        "/timestamp:0", f"/out:{dest / (name + '.sys')}",
                        str(obj), str(support), *map(str, libs)], check=True)
        shutil.copyfile(inf, dest / inf.name)
    built = "probe (existing SwitchFan package preserved)" if args.keep_fan else "probe/bridge"
    print(f"Unsigned experimental {built}: {kit}; requires catalog generation and signing.")

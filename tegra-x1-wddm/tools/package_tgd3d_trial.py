#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package tgd3d with a reviewed .158 KMD and manual recovery tools."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile
from build_display_acpi import validate_table
from package_graphics_policy import stage_graphics_policy


def identity(path):
    return {"Length": path.stat().st_size, "Sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def normalize_records(records):
    if not isinstance(records, dict) or not records:
        return None
    normalized = {}
    for name, record in records.items():
        if (not isinstance(name, str) or not isinstance(record, dict) or
                set(record) != {"Length", "Sha256"} or
                not isinstance(record["Length"], int) or isinstance(record["Length"], bool) or
                not isinstance(record["Sha256"], str) or len(record["Sha256"]) != 64):
            return None
        try:
            int(record["Sha256"], 16)
        except ValueError:
            return None
        normalized[name] = {"Length": record["Length"], "Sha256": record["Sha256"].lower()}
    return normalized


def checked_copy(source, target, expected):
    actual = identity(source)
    if actual["Sha256"].lower() != expected["Sha256"].lower() or actual["Length"] != expected["Length"]:
        raise ValueError(f"Frozen component differs: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("umd", "icd", "kmd", "frozen-manifest", "helpers", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--default-start", action="store_true",
                        help="package automatic enablement after logon, with manual recovery")
    parser.add_argument("--candidate-kmd", type=Path,
                        help="reviewed bit14 or GPU clock-control artifact with manifest.json and signed-package")
    parser.add_argument("--native-boot", action="store_true",
                        help="arm .158 for direct driver startup before logon; no PnP restart task")
    args = parser.parse_args()
    if args.candidate_kmd and not args.default_start:
        parser.error("--candidate-kmd requires --default-start")
    if args.native_boot and not args.default_start:
        parser.error("--native-boot requires --default-start")
    root = Path(__file__).resolve().parents[2]
    umd = json.loads((args.umd / "manifest.json").read_text())
    icd = json.loads((args.icd / "manifest.json").read_text())
    frozen = json.loads(args.frozen_manifest.read_text(encoding="utf-8-sig"))
    frozen_kmd = normalize_records(frozen.get("kmd"))
    frozen_helpers = normalize_records(frozen.get("helpers"))
    candidate = (json.loads((args.candidate_kmd / "manifest.json").read_text(encoding="utf-8-sig"))
                 if args.candidate_kmd else None)
    emc_stack = candidate is not None and candidate.get("variant") == "reviewed-dvfs-emc"
    baseline_hash = ("e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e"
                     if emc_stack else "46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088")
    if (frozen.get("kmd_version") != "0.1.0.158" or not frozen_kmd or not frozen_helpers or
            frozen_kmd.get("tegragpurender.sys", {}).get("Sha256") != baseline_hash):
        raise ValueError("Only the retained visible-desktop .158 KMD is allowed")
    frozen["kmd"] = frozen_kmd
    frozen["helpers"] = frozen_helpers
    if emc_stack:
        # A plain firmware DSDT has FAN0 but omits the retained render GPU.
        # Reject it before creating any installable package directory.
        validate_table(args.helpers / "display158.aml", display=True, emc=True, actmon_irq=True)
        cand_base_files = normalize_records(candidate.get("baseline_signed_files"))
        cand_signed_files = normalize_records(candidate.get("signed_files"))
        cand_source = candidate.get("source")
        frozen_source = frozen.get("kmd_source")
        if (args.native_boot or umd.get("sync_mode") != "gpu-bridge" or
                not isinstance(frozen_source, str) or not frozen_source.strip() or
                frozen_source.strip().lower().startswith("6c854e9f") or
                candidate.get("base") != frozen_source or
                not isinstance(cand_source, str) or not cand_source.strip() or
                cand_source.strip().lower().startswith("6c854e9f") or
                candidate.get("windows_signature_verified") is not True or
                cand_base_files != frozen_kmd or
                not cand_signed_files or set(cand_signed_files) != set(frozen_kmd) or
                candidate.get("platform_fan_version") != 33 or
                isinstance(candidate.get("platform_fan_version"), bool)):
            raise ValueError("EMC requires the signed, matched .158/Fan33 ACTMON-IRQ logon stack and exact recovery package")
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = {"scope": "tgd3d-vulkan trial; retain the active .158 after testing; recovery is manual",
                "automatic_recovery": False,
                "recovery_target": "0.1.0.158-gated",
                "kmd_version": frozen["kmd_version"], "kmd_source": frozen["kmd_source"],
                "umd_name": "TegraGpuD3DVk.dll", "umd_source": umd["mesa"], "icd_source": icd["mesa"],
                "sync_mode": umd.get("sync_mode", "gpu-bridge"),
                "tools_source": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "kmd": frozen["kmd"], "app": {}, "helpers": {}, "files": {}}
    for key in ("shader_cache", "umd_frozen_base", "umd_patch_commits", "red_primary_control"):
        if key in umd:
            manifest[key] = umd[key]
    if args.default_start:
        manifest["scope"] = "frozen .158 with automatic tgd3d enablement after logon; manual testing"
        manifest["startup_mode"] = "logon-default"
    kernel = args.kmd
    if emc_stack:
        manifest.update(kmd_variant="reviewed-dvfs-emc", baseline_kmd=frozen_kmd,
                        kmd=cand_signed_files, kmd_source=cand_source.strip(),
                        platform_fan_version=candidate["platform_fan_version"], dvfs_integration=True,
                        scope=".158 EMC integration; Handheld/85%; GPU bridge; logon+5s; physical acceptance pending")
        kernel = args.candidate_kmd / "signed-package"
        for name, record in frozen["kmd"].items():
            checked_copy(args.kmd / name, args.out / "baseline-kmd" / name, record)
    if args.candidate_kmd and not emc_stack:
        changed = {"tegra-x1-wddm/platform/t214_gr_context.c",
                   "tegra-x1-wddm/tests/test_gr_context.c"}
        vidpn_fix = candidate["variant"] == "shader-output-bit14-gpu460-vidpn"
        clock_control = candidate["variant"] in ("shader-output-bit14-gpu460", "shader-output-bit14-gpu460-vidpn")
        native = candidate["variant"] == "shader-output-bit14-native-boot" or clock_control
        if native:
            changed |= {"tegra-x1-wddm/windows/native_start_test.c",
                        "tegra-x1-wddm/tests/test_native_start_windows.c"}
        if clock_control:
            changed |= {"tegra-x1-wddm/platform/t214_clock.c", "tegra-x1-wddm/platform/t214_clock.h",
                        "tegra-x1-wddm/tests/test_clock.c", "tegra-x1-wddm/tests/test_hardware_windows.c",
                        "tegra-x1-wddm/tools/build.py", "tegra-x1-wddm/windows/hardware.c",
                        "tegra-x1-wddm/windows/render_miniport.c"}
            if vidpn_fix:
                changed |= {"tegra-x1-wddm/windows/panel_vidpn.c",
                            "tegra-x1-wddm/tests/test_panel_timings_windows.c"}
                if candidate.get("modeset_latch_patch_commit"):
                    changed |= {"tegra-x1-wddm/windows/panel_scanout.c",
                                "tegra-x1-wddm/tests/test_panel_scanout_windows.c"}
                    if candidate.get("source_visibility_patch_commit"):
                        changed |= {"tegra-x1-wddm/windows/panel.c",
                                    "tegra-x1-wddm/windows/panel.h"}
                        if candidate.get("scanout_snapshot_patch_commit"):
                            changed |= {"tegra-x1-wddm/windows/panel_snapshot.h",
                                        "tegra-x1-wddm/platform/t214_dc.h"}
                if candidate.get("gpu_prerotation_patch_commit"):
                    changed |= {"tegra-x1-wddm/platform/t214_dc.c",
                                "tegra-x1-wddm/platform/t214_dc.h",
                                "tegra-x1-wddm/windows/allocation.c",
                                "tegra-x1-wddm/windows/present.c",
                                "tegra-x1-wddm/tests/test_context_windows.c",
                                "tegra-x1-wddm/tests/test_dc_rotation.c",
                                "tegra-x1-wddm/tests/test_dc_portrait.c"}
                    if (umd.get("gpu_prerotation_patch_commit") != candidate["gpu_prerotation_patch_commit"] or
                        umd.get("sync_mode") != "gpu-bridge"):
                        raise ValueError("Portrait-only scanout requires its matching GPU-prerotation UMD")
            baseline_hashes = ({
                "59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81",
                "69aa6036da81870efb439e8e09530ae662e810dccf887b603bdfeb6ba8a4a11a",
                "1a86a02b479b716b2b695ade0bd6860076d4d5f61a29d95b804cb655ce0bf164",
            } if vidpn_fix else {
                "c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53",
            })
            if (candidate.get("target_khz") != 460800 or
                candidate["baseline_signed_files"]["tegragpurender.sys"]["Sha256"] not in
                    baseline_hashes):
                raise ValueError("Candidate must restore its reviewed .158 clock baseline")
        if (candidate["base"] != frozen["kmd_source"] or
            candidate["variant"] not in ("shader-output-bit14", "shader-output-bit14-native-boot", "shader-output-bit14-gpu460", "shader-output-bit14-gpu460-vidpn") or
            candidate.get("windows_signature_verified") is not True or
            set(candidate["changed_files"]) != changed):
            raise ValueError("Unreviewed kernel variant or unsigned artifact")
        manifest["baseline_kmd"] = frozen["kmd"]
        if clock_control:
            manifest["baseline_kmd"] = candidate["baseline_signed_files"]
            manifest["kmd_gpu_khz"] = 460800
            manifest["kmd_clock_patch_commit"] = candidate["clock_patch_commit"]
            if vidpn_fix:
                manifest["kmd_vidpn_patch_commit"] = candidate["vidpn_patch_commit"]
                if candidate.get("modeset_latch_patch_commit"):
                    manifest["kmd_modeset_latch_patch_commit"] = candidate["modeset_latch_patch_commit"]
                    if candidate.get("source_visibility_patch_commit"):
                        manifest["kmd_source_visibility_patch_commit"] = candidate["source_visibility_patch_commit"]
        manifest["kmd_variant"] = candidate["variant"]
        manifest["kmd_patch_commit"] = candidate["patch_commit"]
        if native:
            manifest["kmd_startup_patch_commit"] = candidate["startup_patch_commit"]
        if candidate.get("inf_identity"):
            manifest["kmd_package_identity"] = candidate["inf_identity"]
        if candidate.get("additional_patch_commits"):
            manifest["kmd_additional_patch_commits"] = candidate["additional_patch_commits"]
        if candidate.get("scanout_snapshot_patch_commit"):
            manifest["kmd_scanout_snapshot_patch_commit"] = candidate["scanout_snapshot_patch_commit"]
        if candidate.get("gpu_prerotation_patch_commit"):
            manifest["gpu_prerotation_patch_commit"] = candidate["gpu_prerotation_patch_commit"]
        manifest["kmd"] = candidate["signed_files"]
        manifest["scope"] = ".158 bit14 fix with automatic tgd3d enablement after logon; manual testing"
        if clock_control:
            manifest["scope"] = (".158 GPU460.8 VidPN source-mode correction; restore the retained GPU460.8 kernel" if vidpn_fix else
                                 ".158 bit14 GPU460.8 control; restore the retained 729.6 MHz kernel; logon start")
            if manifest.get("kmd_modeset_latch_patch_commit"):
                manifest["scope"] = ".158 GPU460.8 VidPN with synchronous modeset latch; restore the retained GPU460.8 kernel"
            if manifest.get("kmd_source_visibility_patch_commit"):
                manifest["scope"] = ".158 GPU460.8 source visibility preserves primary binding; restore the retained GPU460.8 kernel"
            if manifest.get("gpu_prerotation_patch_commit"):
                manifest["scope"] = ".158 GPU460.8 tgd3d GPU prerotation into portrait scanout; logon start; physical acceptance pending"
        kernel = args.candidate_kmd / "signed-package"
        for name, record in manifest["baseline_kmd"].items():
            checked_copy(args.kmd / name, args.out / "baseline-kmd" / name, record)
    if args.native_boot:
        if manifest.get("kmd_variant") not in ("shader-output-bit14-native-boot", "shader-output-bit14-gpu460", "shader-output-bit14-gpu460-vidpn"):
            raise ValueError("Direct boot requires persistent native-start kernel support")
        manifest["startup_mode"] = "native-boot"
        manifest["scope"] = ".158 direct native startup before logon; owner-operated restart and application tests"
    for name, record in manifest["kmd"].items():
        checked_copy(kernel / name, args.out / "package" / name, record)
    for directory, component in ((args.umd, umd), (args.icd, icd)):
        for name, sha in component["files"].items():
            src = directory / name
            record = identity(src)
            if record["Sha256"].lower() != sha.lower():
                raise ValueError(f"Built artifact differs: {src}")
            checked_copy(src, args.out / "app" / name, record)
            manifest["app"][name] = record
    for name in ("display-state.exe", "desktop-path.exe", "trace-flush.exe", "etl-export-full.exe", "display158.aml"):
        record = frozen["helpers"][name]
        checked_copy(args.helpers / name, args.out / name, record)
        manifest["helpers"][name] = record
    scripts = root / "tegra-x1-wddm/tools/tgd3d-trial"
    default_shared = {"Common.ps1", "Prepare.ps1", "Enable-RuntimeAppContainerRead.ps1",
                      "Read-PlatformState.ps1", "Restore-AfterBoot.ps1",
                      "restore-preference.ps1", "Stop-DxgBootTrace.ps1", "tool-provenance.json",
                      "Switch-Kmd.ps1"}
    for file in scripts.iterdir():
        if args.default_start and file.name not in default_shared:
            continue
        if file.suffix in (".ps1", ".cmd", ".json", ".md"):
            shutil.copyfile(file, args.out / file.name)
    if args.default_start:
        for file in (root / "tegra-x1-wddm/tools/tgd3d-default").iterdir():
            if file.is_file():
                shutil.copyfile(file, args.out / file.name)
    if args.default_start:
        manifest["graphics_policy_sha256"] = stage_graphics_policy(root, args.out)
    licenses = args.out / "licenses"
    licenses.mkdir()
    for source, target in ((root / "docs/license.rst", "Mesa.txt"),
                           (root / "subprojects/dxbc-spirv/LICENSE", "dxbc-spirv.txt")):
        shutil.copyfile(source, licenses / target)
    for file in sorted(args.out.rglob("*")):
        if file.is_file():
            manifest["files"][file.relative_to(args.out).as_posix()] = identity(file)
    (args.out / "trial-components.json").write_text(json.dumps(manifest, indent=2) + "\n")
    archive = args.out.with_suffix(".zip")
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as z:
        for file in sorted(args.out.rglob("*")):
            if file.is_file():
                z.write(file, file.relative_to(args.out).as_posix())
    with zipfile.ZipFile(archive) as z:
        if z.testzip():
            raise ValueError("Archive CRC failed")
        for name, record in manifest["files"].items():
            data = z.read(name)
            if len(data) != record["Length"] or hashlib.sha256(data).hexdigest() != record["Sha256"]:
                raise ValueError("Archive manifest differs: " + name)
    result = {"archive": str(archive.resolve()), **identity(archive), "files": len(manifest["files"]),
              "umd_source": umd["mesa"], "icd_source": icd["mesa"], "kmd_source": manifest["kmd_source"]}
    archive.with_suffix(".delivery.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

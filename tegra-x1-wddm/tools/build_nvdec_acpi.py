#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Append the optional NVDEC resource bundle to an explicitly pinned GPU0 table.

No devices, panel timings, interrupts or FAN0 resources are replaced. This
builds a local ACPI candidate; it neither installs it nor enables decode.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

from build_display_acpi import (
    _aml_device_crs_obj, _aml_pkg_length, _aml_sb_devices,
    _crs_mem32_descriptors, device_span, disassemble, syntax_mask, validate_table,
)

# Reviewed installed table. Its four dynamic _CRS methods are UAR2 (CRS3),
# SDM1/SDM4 (GCRS) and BAT0 (GPIO/I2C buffers only). SIO0's SCAR fields touch
# offsets008/014/1a0/308/30c/328/32c/358/360/3f0/430/434/440/444, not the new
# Y-bank words or the read-only Host1x source at0x180. A different table needs
# renewed ownership review, not a bypass.
REVIEWED_BASELINE = "0502bd0aed0aa3e236d24b0977d389bf04c64d777ba94cad4c5c27a188b9e438"
DYNAMIC_MEMORY = [(0x70006040, 0x40), (0x700B0000, 0x200), (0x700B0600, 0x200)]


def nvdec_ranges():
    header = Path(__file__).resolve().parents[1] / "platform/t214_resources.h"
    text = header.read_text().split("#define T214_NVDEC_RESOURCES(X)", 1)[1]
    text = text.split("#define T214_NVDEC_RESOURCE_COUNT", 1)[0]
    rows = re.findall(r"X\((\w+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*([01])\)", text)
    if len(rows) != 12 or rows[-1] != ("HostSource", "0x60006180", "0x00000004", "0"):
        raise ValueError("Unexpected NVDEC resource contract")
    return [(name, int(base, 16), int(size, 16), int(writable))
            for name, base, size, writable in rows]


def buffer_bytes(obj):
    length, pos = _aml_pkg_length(obj, 1)
    if obj[0] != 0x11 or 1 + length != len(obj):
        raise ValueError("Expected a complete AML Buffer")
    op = obj[pos]
    if op in (0, 1):
        size, pos = op, pos + 1
    elif op in (0x0A, 0x0B, 0x0C, 0x0E):
        width = {0x0A: 1, 0x0B: 2, 0x0C: 4, 0x0E: 8}[op]
        size = int.from_bytes(obj[pos + 1:pos + 1 + width], "little")
        pos += width + 1
    else:
        raise ValueError("Dynamic buffer sizes are outside this tool's scope")
    if size != len(obj) - pos:
        raise ValueError("AML Buffer size mismatch")
    return obj[pos:]


CRS = re.compile(r"\bName\s*\(\s*_CRS\s*,\s*Buffer\s*\([^)]*\)\s*\{([^}]*)\}\s*\)", re.S)


def crs_matches(text):
    masked = syntax_mask(text)
    matches = list(CRS.finditer(masked))
    if len(matches) != len(re.findall(r"\bName\s*\(\s*_CRS\b", masked)):
        raise ValueError("An unrecognized static _CRS requires separate review")
    return matches


def crs_payload(match):
    body = match[1]
    if not re.fullmatch(r"\s*(?:0x[0-9A-Fa-f]{2}\s*,?\s*)*", body):
        raise ValueError("Unsupported _CRS buffer initializer")
    return bytes(int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", body))


def append_resources(text, old_payload, ranges):
    # Check every static resource buffer, including nested devices, before
    # granting a new physical range to GPU0. Parse only the standard descriptor
    # forms already admitted by the retained-table validator.
    existing_memory = list(DYNAMIC_MEMORY)
    for match in crs_matches(text):
        payload = crs_payload(match)
        # Wrap for the existing descriptor parser; use an explicit DWORD size.
        body = b"\x0c" + struct.pack("<I", len(payload)) + payload
        plen = len(body) + 2  # all current _CRS objects fit a two-byte PkgLength
        if not 64 <= plen < 4096:
            # The length's own byte count is included in PkgLength.
            if len(body) + 1 < 64:
                encoded = bytes([len(body) + 1])
            else:
                raise ValueError("Unexpectedly large resource buffer")
        else:
            encoded = bytes([0x40 | (plen & 15), plen >> 4])
        obj = b"\x11" + encoded + body
        existing_memory.extend((base, size) for _, base, size in _crs_mem32_descriptors(obj))
    for base, size in existing_memory:
        for name, added, length, _ in ranges:
            if base < added + length and added < base + size:
                raise ValueError(f"New {name} range overlaps an existing owner")
    begin, end = device_span(text, "GPU0")
    device = text[begin:end]
    matches = crs_matches(device)
    if len(matches) != 1 or crs_payload(matches[0]) != old_payload:
        raise ValueError("GPU0 source _CRS differs from its compiled baseline")
    if old_payload[-2:] != b"\x79\x00":
        raise ValueError("Expected the retained resource end tag")
    extra = b"".join(struct.pack("<BHBII", 0x86, 9, writable, base, size)
                     for _, base, size, writable in ranges)
    payload = old_payload[:-2] + extra + old_payload[-2:]
    rows = [", ".join(f"0x{v:02X}" for v in payload[i:i + 16])
            for i in range(0, len(payload), 16)]
    replacement = f"Name (_CRS, Buffer (0x{len(payload):X})\n{{\n" + ",\n".join(rows) + "\n})"
    match = matches[0]
    device = device[:match.start()] + replacement + device[match.end():]
    return text[:begin] + device + text[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--baseline-sha256", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iasl", default="iasl")
    args = parser.parse_args()
    before = validate_table(args.baseline, display=True, emc=True, actmon_irq=True)
    if hashlib.sha256(before).hexdigest() != args.baseline_sha256.lower():
        parser.error("Baseline does not match the explicitly selected component combination")
    if args.baseline_sha256.lower() != REVIEWED_BASELINE:
        parser.error("This table's dynamic _CRS/OperationRegion owners have not been reviewed")
    manifest = args.out.with_suffix(".json")
    if args.out == manifest or args.out.exists() or manifest.exists():
        parser.error("Output exists or collides with its manifest")
    ranges = nvdec_ranges()
    old_obj = _aml_device_crs_obj(before, b"GPU0", b"SWOA0F01")
    old_payload = buffer_bytes(old_obj)
    args.out.parent.mkdir(exist_ok=True, parents=True)
    with tempfile.TemporaryDirectory(prefix="nvdec-acpi-") as tmp:
        work = Path(tmp)
        old = disassemble(args.iasl, args.baseline, work / "old")
        if len(re.findall(r"\bMethod\s*\(\s*_CRS\b", syntax_mask(old))) != 4:
            raise ValueError("Reviewed dynamic resource methods changed")
        source = work / "candidate.dsl"
        source.write_text(append_resources(old, old_payload, ranges))
        subprocess.run([args.iasl, "-ve", "-p", str(work / "candidate"), str(source)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        data = validate_table(work / "candidate.aml", display=True, emc=True, actmon_irq=True)
        devices_before, devices_after = [], []
        _aml_sb_devices(before, 36, len(before), devices_before)
        _aml_sb_devices(data, 36, len(data), devices_after)
        if [x[0] for x in devices_before] != [x[0] for x in devices_after]:
            raise ValueError("Device namespace changed")
        for (name, old_body), (_, new_body) in zip(devices_before, devices_after):
            if name != b"GPU0" and old_body != new_body:
                raise ValueError("Unrelated AML device changed: " + name.decode("ascii"))
        new_obj = _aml_device_crs_obj(data, b"GPU0", b"SWOA0F01")
        new_payload = buffer_bytes(new_obj)
        expected = old_payload[:-2] + b"".join(
            struct.pack("<BHBII", 0x86, 9, writable, base, size)
            for _, base, size, writable in ranges) + old_payload[-2:]
        if new_payload != expected:
            raise ValueError("Compiled GPU resource data differs")
        old_gpu = dict(devices_before)[b"GPU0"]
        new_gpu = dict(devices_after)[b"GPU0"]
        if old_gpu.count(old_obj) != 1 or new_gpu.count(new_obj) != 1 or \
                old_gpu.replace(old_obj, b"") != new_gpu.replace(new_obj, b""):
            raise ValueError("GPU0 changed outside the resource buffer")
        args.out.write_bytes(data)
    result = {"baseline_sha256": hashlib.sha256(before).hexdigest(),
              "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
              "updated_device": "GPU0", "added_ranges": ranges,
              "other_device_aml_unchanged": True, "gpu_nonresource_aml_unchanged": True,
              "native_installed": False, "decode_enabled": False}
    manifest.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

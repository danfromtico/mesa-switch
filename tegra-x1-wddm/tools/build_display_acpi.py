#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Update FAN0 resources without replacing the retained display namespace."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def _aml_pkg_length(data, pos):
    if pos >= len(data):
        raise ValueError("Truncated AML package length")
    lead = data[pos]
    extra = lead >> 6
    if pos + 1 + extra > len(data) or (extra and lead & 0x30):
        raise ValueError("Invalid AML package length")
    if extra == 0:
        return lead & 0x3F, pos + 1
    value = lead & 0x0F
    for idx in range(extra):
        value |= data[pos + 1 + idx] << (4 + 8 * idx)
    return value, pos + 1 + extra


def _aml_nameseg(buf, pos, end):
    """This project's static namespace permits single NameSegs, not paths."""
    seg = buf[pos:pos + 4]
    if pos + 4 > end or re.fullmatch(rb"[A-Z_][A-Z0-9_]{3}", seg) is None:
        raise ValueError("Unsupported or truncated static AML NameSeg")
    return seg


def _aml_skip_data(buf, pos, end):
    if pos >= end:
        raise ValueError("Truncated AML data object")
    op = buf[pos]
    if op in (0x00, 0x01, 0xFF):
        return pos + 1
    if op in (0x0A, 0x0B, 0x0C, 0x0E):
        step = {0x0A: 2, 0x0B: 3, 0x0C: 5, 0x0E: 9}[op]
        if pos + step > end:
            raise ValueError("Truncated AML integer constant")
        return pos + step
    if op == 0x0D:
        nul = buf.find(b"\x00", pos + 1, end)
        if nul < 0:
            raise ValueError("Unterminated AML string")
        return nul + 1
    if op in (0x11, 0x12, 0x13):
        plen, _ = _aml_pkg_length(buf, pos + 1)
        if pos + 1 + plen > end:
            raise ValueError("Truncated AML buffer/package")
        return pos + 1 + plen
    raise ValueError("Unsupported AML data opcode in static layout")


def _aml_sb_devices(buf, pos, end, out, in_sb=False):
    r"""Collect direct \_SB Device bodies from this project's static DSDT layout."""
    while pos < end:
        op = buf[pos]
        if op == 0x10:
            if in_sb:
                raise ValueError("Nested AML Scope is outside the static layout")
            plen, npos = _aml_pkg_length(buf, pos + 1)
            send = pos + 1 + plen
            if send > end:
                raise ValueError("Truncated AML Scope")
            if buf[npos:npos + 5] == b"\\_SB_":
                if npos + 5 > send:
                    raise ValueError("Truncated AML Scope name")
                _aml_sb_devices(buf, npos + 5, send, out, True)
            elif buf[npos:npos + 4] == b"_SB_":
                if npos + 4 > send:
                    raise ValueError("Truncated AML Scope name")
                _aml_sb_devices(buf, npos + 4, send, out, True)
            else:
                raise ValueError("Unsupported non-_SB AML Scope in static layout")
            pos = send
        elif buf[pos:pos + 2] == b"\x5b\x82":
            if not in_sb:
                raise ValueError("Device outside direct _SB scope")
            plen, npos = _aml_pkg_length(buf, pos + 2)
            dend = pos + 2 + plen
            if npos + 4 > dend or dend > end:
                raise ValueError("Truncated AML Device")
            out.append((_aml_nameseg(buf, npos, dend), buf[npos + 4:dend]))
            pos = dend
        elif op == 0x14:
            plen, npos = _aml_pkg_length(buf, pos + 1)
            mend = pos + 1 + plen
            if npos + 5 > mend or mend > end:
                raise ValueError("Truncated AML Method")
            _aml_nameseg(buf, npos, mend)
            pos = mend
        elif op == 0x08:
            if pos + 5 > end:
                raise ValueError("Truncated AML Name")
            _aml_nameseg(buf, pos + 1, end)
            pos = _aml_skip_data(buf, pos + 5, end)
        else:
            raise ValueError("Unsupported top-level AML opcode in static layout")


def _aml_device_crs_obj(data, dev_name, hid):
    hid_op = b"\x08_HID\x0d" + hid + b"\x00"
    if data.count(hid + b"\x00") != 1 or data.count(hid_op) != 1:
        raise ValueError("Missing/duplicate display or platform device identity in DSDT")
    devices = []
    _aml_sb_devices(data, 36, len(data), devices)
    matches = [body for name, body in devices if name == dev_name]
    if len(matches) != 1:
        raise ValueError("Unresolved AML Device scope for " + dev_name.decode("ascii"))
    body = matches[0]
    pos, members = 0, {}
    while pos < len(body):
        if body[pos] == 0x08:
            if pos + 5 > len(body):
                raise ValueError("Truncated Device Name member")
            seg = _aml_nameseg(body, pos + 1, len(body))
            dend = _aml_skip_data(body, pos + 5, len(body))
            if seg in members:
                raise ValueError("Duplicate AML member in " + dev_name.decode("ascii"))
            members[seg] = ("Name", body[pos + 5:dend])
            pos = dend
        elif body[pos] == 0x14:
            plen, mpos = _aml_pkg_length(body, pos + 1)
            mend = pos + 1 + plen
            if mpos + 5 > mend or mend > len(body):
                raise ValueError("Truncated Device Method member")
            seg = _aml_nameseg(body, mpos, mend)
            if seg in members:
                raise ValueError("Duplicate AML member in " + dev_name.decode("ascii"))
            members[seg] = ("Method", body[pos:mend])
            pos = mend
        else:
            raise ValueError("Unsupported nested AML opcode in " + dev_name.decode("ascii"))
    if members.get(b"_HID") != ("Name", b"\x0d" + hid + b"\x00"):
        raise ValueError("Unresolved AML Device scope for " + dev_name.decode("ascii"))
    if members.get(b"_STA") not in (("Method", b"\x14\x09_STA\x00\xa4\x0a\x0f"), ("Name", b"\x0a\x0f")):
        raise ValueError("Inactive or missing _STA=0x0F on " + dev_name.decode("ascii"))
    kind, crs_obj = members.get(b"_CRS", (None, b""))
    if kind != "Name" or not crs_obj.startswith(b"\x11"):
        raise ValueError("Missing or duplicate static _CRS buffer")
    return crs_obj


def _crs_mem32_descriptors(crs_obj, strict_fan=False, interrupts=None):
    plen, bpos = _aml_pkg_length(crs_obj, 1)
    end = 1 + plen
    if end != len(crs_obj) or bpos >= end:
        raise ValueError("Truncated _CRS buffer")
    op = crs_obj[bpos]
    if op == 0x0A and bpos + 2 <= end:
        blen, rpos = crs_obj[bpos + 1], bpos + 2
    elif op == 0x0B and bpos + 3 <= end:
        blen, rpos = struct.unpack_from("<H", crs_obj, bpos + 1)[0], bpos + 3
    elif op == 0x0C and bpos + 5 <= end:
        blen, rpos = struct.unpack_from("<I", crs_obj, bpos + 1)[0], bpos + 5
    else:
        raise ValueError("Unsupported _CRS buffer length encoding")
    crs = crs_obj[rpos:end]
    if len(crs) != blen or crs[-2:] != b"\x79\x00":
        raise ValueError("Malformed _CRS resource template")
    idx, mem32, irq_count = 0, [], 0
    while idx < len(crs):
        tag = crs[idx]
        if (tag & 0xF8) == 0x78:
            if tag == 0x79 and idx + 2 == len(crs):
                return mem32
            break
        if tag & 0x80:
            if idx + 3 > len(crs):
                break
            rlen = struct.unpack_from("<H", crs, idx + 1)[0]
            if idx + 3 + rlen > len(crs):
                break
            if tag in (0x81, 0x85, 0x87, 0x88, 0x8A, 0x8B):
                raise ValueError("Unsupported non-Memory32Fixed address aperture in _CRS")
            if strict_fan and tag not in (0x86, 0x89, 0x8E):
                raise ValueError("Unexpected non-MMIO/I2C resource descriptor on FAN0")
            if strict_fan and tag == 0x89:
                # Exact ACTMON extended interrupt: consumer, level, active
                # high, exclusive, one GSIV77. Do not accept an arbitrary IRQ.
                if rlen != 6 or crs[idx + 3:idx + 5] != b'\x01\x01' or struct.unpack_from('<I', crs, idx + 5)[0] != 77:
                    raise ValueError("FAN0 interrupt differs from T210 ACTMON GSIV77")
                irq_count += 1
                if irq_count > 1:
                    raise ValueError("Duplicate FAN0 ACTMON interrupt")
                if interrupts is not None:
                    interrupts.append(77)
            if strict_fan and tag == 0x8E and (rlen < 9 or crs[idx + 5] != 1):
                raise ValueError("Malformed or non-I2C serial bus resource on FAN0")
            if tag == 0x86:
                if rlen != 9:
                    break
                mem32.append(struct.unpack_from("<BII", crs, idx + 3))
            idx += 3 + rlen
        else:
            if strict_fan or idx + 1 + (tag & 7) > len(crs):
                break
            idx += 1 + (tag & 7)
    raise ValueError("Corrupted _CRS descriptor stream")


def validate_table(path, display=False, emc=False, actmon_irq=False):
    data = Path(path).read_bytes()
    if (len(data) < 36 or data[:4] != b"DSDT" or
            struct.unpack_from("<I", data, 4)[0] != len(data) or sum(data) & 255):
        raise ValueError("Invalid DSDT header, length or checksum")
    fan_crs = _aml_device_crs_obj(data, b"FAN0", b"SWOA0402")
    interrupts = []
    fan_mem = _crs_mem32_descriptors(fan_crs, strict_fan=True, interrupts=interrupts)
    if actmon_irq and interrupts != [77]:
        raise ValueError("FAN0 must supply exactly one ACTMON interrupt")
    if emc:
        # Matched Fan25 contract: exact resources, never a broad MC aperture.
        # 20 PWM/DFLL/BPMP/ACTMON resources, 3 read-only display/DRAM inputs,
        # and 75 exact PTSA registers (60 writable, 15 validation-only).
        expected = {
            (1,0x7000A010,4), (1,0x7000A000,4), (1,0x60006000,0x100),
            (0,0xA9FB0000,0x1000), (1,0x70110000,0x300),
            (1,0x6000662C,8), (1,0x600062F4,4), (0,0x6000635C,12),
            (1,0x60006438,8), (1,0x60006448,8), (0,0x60006128,4),
            (1,0x60001000,12), (1,0x60004018,4),
            (1,0xFFE00000,0x100000), (1,0x40000000,0x40000),
            (1,0x6000C800,0x400), (1,0x600063E8,4),
            (1,0x60006430,8), (1,0x60006440,8), (0,0x60006358,4),
            (0,0x700192E8,4), (0,0x70019690,4), (0,0x7001B104,4),
            (1,0x700190FC,4), (1,0x70019960,4), (0,0x700192AC,4),
        }
        mins = [0x420,0x42C,0x438,0x498,0x444,0x450,0x45C,0x480,
                0x4A4,0x48C,0x4F8,0x4C8,0x540,0x558,0x4EC,0x4B0,
                0x51C,0x4E0,0x4BC,0x54C,0x4D4,0x528,0x534,0x510]
        rate_writers = {0x420,0x42C,0x438,0x498,0x444,0x450,0x45C,0x480,0x4A4,0x510}
        for minimum in mins:
            expected.add((1,0x70019000+minimum,4))
            expected.add((1,0x70019000+minimum+4,4))
            expected.add((int(minimum in rate_writers),0x70019000+minimum-4,4))
        if len(fan_mem) != len(expected) or set(fan_mem) != expected:
            raise ValueError("FAN0 MMIO resources/permissions differ from the matched Fan25 contract")
    if display:
        gpu_mem = _crs_mem32_descriptors(_aml_device_crs_obj(data, b"GPU0", b"SWOA0F01"))
        if ((1,0x57000000,0x1000000) not in gpu_mem or
                (1,0x58000000,0x1000000) not in gpu_mem):
            raise ValueError("GPU0 lacks required MMIO BAR resources in DSDT")
        _crs_mem32_descriptors(_aml_device_crs_obj(data, b"PWRB", b"SWOA0601"))
    return data


def syntax_mask(text):
    """Mask comments/strings so braces and Device names mean ASL syntax."""
    token = re.compile(r'"(?:\\.|[^"\\])*"|/\*.*?\*/|//[^\n]*', re.S)
    return token.sub(lambda m: "".join("\n" if c == "\n" else " " for c in m[0]), text)


def device_span(text, name):
    mask = syntax_mask(text)
    matches = list(re.finditer(r'\bDevice\s*\(\s*' + re.escape(name) + r'\s*\)\s*\{', mask))
    if len(matches) != 1:
        raise ValueError("Expected one ASL Device " + name)
    start = matches[0].start()
    pos, depth = matches[0].end(), 1
    while pos < len(mask) and depth:
        depth += (mask[pos] == "{") - (mask[pos] == "}")
        pos += 1
    if depth:
        raise ValueError("Unclosed ASL Device " + name)
    return start, pos


def merge_fan(baseline, platform):
    begin, end = device_span(baseline, "FAN0")
    pbegin, pend = device_span(platform, "FAN0")
    fan = platform[pbegin:pend]
    if not re.search(r'Name\s*\(\s*_HID\s*,\s*"SWOA0402"\s*\)', fan):
        raise ValueError("FAN0 hardware identity differs")
    return baseline[:begin] + fan + baseline[end:]


def disassemble(iasl, source, prefix):
    subprocess.run([iasl, "-d", "-p", str(prefix), str(source)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return prefix.with_suffix(".dsl").read_text()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--platform", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--iasl", default="iasl")
    args = parser.parse_args()
    before = validate_table(args.baseline, display=True)
    platform_data = validate_table(args.platform, emc=True, actmon_irq=True)
    manifest_out = args.out.with_suffix(".json")
    if args.out == manifest_out or args.out.suffix.lower() == ".json" or args.out.exists() or manifest_out.exists():
        raise ValueError("Output already exists or collides with manifest")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="display-acpi-") as directory:
        work = Path(directory)
        old = disassemble(args.iasl, args.baseline, work / "baseline")
        platform = disassemble(args.iasl, args.platform, work / "platform")
        merged = merge_fan(old, platform)
        source = work / "display.dsl"
        source.write_text(merged)
        subprocess.run([args.iasl, "-ve", "-p", str(work / "display"), str(source)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        result = work / "display.aml"
        data = validate_table(result, display=True, emc=True, actmon_irq=True)
        roundtrip = disassemble(args.iasl, result, work / "verified")
        # Recompilation must preserve the namespace and every device body;
        # FAN0 must match the replacement, all others the display baseline.
        names = re.findall(r'\bDevice\s*\(\s*([A-Z0-9_]+)\s*\)', syntax_mask(old))
        if re.findall(r'\bDevice\s*\(\s*([A-Z0-9_]+)\s*\)', syntax_mask(roundtrip)) != names:
            raise ValueError("Roundtrip device list changed")
        normalize = lambda s: re.sub(r"\s+", "", re.sub(r'/\*.*?\*/|//[^\n]*', "", s, flags=re.S))
        pbegin, pend = device_span(platform, "FAN0")
        for name in names:
            b, y = device_span(roundtrip, name)
            if name == "FAN0":
                if normalize(platform[pbegin:pend]) != normalize(roundtrip[b:y]):
                    raise ValueError("Updated device changed: FAN0")
                continue
            a, z = device_span(old, name)
            if normalize(old[a:z]) != normalize(roundtrip[b:y]):
                raise ValueError("Retained device changed: " + name)
        args.out.write_bytes(data)
    manifest = {"baseline_sha256": hashlib.sha256(before).hexdigest(),
                "platform_sha256": hashlib.sha256(platform_data).hexdigest(),
                "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                "updated_device": "FAN0", "actmon_irq_gsiv": 77,
                "retained_other_devices": [n for n in names if n != "FAN0"],
                "native_tested": False}
    manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest))


if __name__ == "__main__":
    main()

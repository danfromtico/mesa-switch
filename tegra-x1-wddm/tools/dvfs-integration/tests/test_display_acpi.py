#!/usr/bin/env python3
"""Reject misrouted/duplicate ACTMON IRQ resources before a native install."""
import importlib.util
from pathlib import Path
import struct
import unittest

path = Path(__file__).resolve().parents[2] / "build_display_acpi.py"
spec = importlib.util.spec_from_file_location("display_acpi", path)
acpi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acpi)


def buffer(resources):
    raw = resources + b"\x79\x00"
    payload = b"\x0b" + struct.pack("<H", len(raw)) + raw
    length = len(payload) + 2
    return b"\x11" + bytes([0x40 | (length & 15), length >> 4]) + payload


def irq(flags=1, vector=77):
    return b"\x89\x06\x00" + bytes([flags, 1]) + struct.pack("<I", vector)


class ActmonResources(unittest.TestCase):
    def test_exact_irq_and_mmio(self):
        mmio = b"\x86\x09\x00" + struct.pack("<BII", 1, 0x6000C800, 0x400)
        interrupts = []
        self.assertEqual(acpi._crs_mem32_descriptors(buffer(mmio + irq()), True, interrupts),
                         [(1, 0x6000C800, 0x400)])
        self.assertEqual(interrupts, [77])

    def test_wrong_irq_contract(self):
        for flags, vector in [(3,77), (5,77), (9,77), (0,77), (1,45), (1,78)]:
            with self.subTest(flags=flags,vector=vector), self.assertRaises(ValueError):
                acpi._crs_mem32_descriptors(buffer(irq(flags,vector)), True)

    def test_duplicate_or_truncated(self):
        for raw in [irq()+irq(), irq()[:-1], b"\x89\x06\x00\x01\x02\x4d\x00\x00\x00"]:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                acpi._crs_mem32_descriptors(buffer(raw), True)

    def test_resource_body_is_not_a_second_descriptor(self):
        # IRQ bytes inside a serial-bus body are not an interrupt resource.
        body=b"\x02\x00\x01"+b"\x00"*6+irq()
        serial=b"\x8e"+struct.pack("<H",len(body))+body
        interrupts=[]
        acpi._crs_mem32_descriptors(buffer(serial), True, interrupts)
        self.assertEqual(interrupts, [])


if __name__ == "__main__":
    unittest.main()

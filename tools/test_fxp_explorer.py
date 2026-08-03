from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from fxp_explorer import FXPFormatError, extract_records, parse_fxp


def dxbc(payload_tag: bytes = b"TEST", payload: bytes = b"data") -> bytes:
    chunk_offset = 36
    total_size = chunk_offset + 8 + len(payload)
    return b"".join(
        (
            b"DXBC",
            bytes(16),
            struct.pack("<I", 1),
            struct.pack("<I", total_size),
            struct.pack("<I", 1),
            struct.pack("<I", chunk_offset),
            payload_tag,
            struct.pack("<I", len(payload)),
            payload,
        )
    )


def record(stage: str, technique_id: int, blob: bytes) -> bytes:
    metadata_size = 32 if stage == "PS" else 40
    return b"".join(
        (
            struct.pack("<III", 0x11223344, len(blob), technique_id),
            bytes(range(metadata_size)),
            b"\x01\x02\x03\x04",
            blob,
        )
    )


def family(stage_records: dict[str, list[bytes]]) -> bytes:
    counts = tuple(len(stage_records.get(stage, ())) for stage in ("VS", "HS", "DS", "PS", "CS"))
    body = b"".join(
        item
        for stage in ("VS", "HS", "DS", "PS", "CS")
        for item in stage_records.get(stage, ())
    )
    return struct.pack("<5I", *counts) + body


class FXPExplorerTests(unittest.TestCase):
    def write_fixture(self, data: bytes, root: Path) -> Path:
        path = root / "fixture.fxp"
        path.write_bytes(data)
        return path

    def test_parses_concatenated_families_and_stage_layouts(self) -> None:
        source = b"".join(
            (
                family(
                    {
                        "VS": [record("VS", 0x10, dxbc(b"ISGN"))],
                        "PS": [record("PS", 0x20, dxbc(b"SHDR"))],
                    }
                ),
                family({"CS": [record("CS", 0x30, dxbc(b"SHEX"))]}),
            )
        )
        with tempfile.TemporaryDirectory() as temporary:
            archive = parse_fxp(self.write_fixture(source, Path(temporary)))

        self.assertEqual(len(archive.families), 2)
        self.assertEqual([item.stage for item in archive.records], ["VS", "PS", "CS"])
        self.assertEqual(
            [item.technique_id for item in archive.records], [0x10, 0x20, 0x30]
        )
        self.assertEqual(archive.records[1].dxbc.chunk_tags, ("SHDR",))
        self.assertEqual(len(archive.records[0].metadata), 40)
        self.assertEqual(len(archive.records[1].metadata), 32)

    def test_rejects_wrong_record_magic_at_exact_offset(self) -> None:
        source = bytearray(family({"PS": [record("PS", 1, dxbc())]}))
        source[20:24] = b"BAD!"
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write_fixture(bytes(source), Path(temporary))
            with self.assertRaisesRegex(FXPFormatError, r"offset 0x14: bad record magic"):
                parse_fxp(path)

    def test_rejects_dxbc_size_mismatch(self) -> None:
        blob = bytearray(dxbc())
        struct.pack_into("<I", blob, 24, len(blob) + 4)
        source = family({"PS": [record("PS", 1, bytes(blob))]})
        with tempfile.TemporaryDirectory() as temporary:
            path = self.write_fixture(source, Path(temporary))
            with self.assertRaisesRegex(FXPFormatError, "DXBC size field"):
                parse_fxp(path)

    def test_extracts_without_overwriting_different_existing_file(self) -> None:
        source = family({"PS": [record("PS", 0x1234, dxbc())]})
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = parse_fxp(self.write_fixture(source, root))
            output = root / "out"
            output.mkdir()
            section = output / archive.records[0].section_directory
            section.mkdir()
            expected_name = archive.records[0].filename
            (section / expected_name).write_bytes(b"different")
            files = extract_records(archive, archive.records, output)

            self.assertEqual((section / expected_name).read_bytes(), b"different")
            self.assertEqual(files[0].name, Path(expected_name).stem + "_1.bin")
            self.assertTrue((output / "manifest.csv").is_file())


if __name__ == "__main__":
    unittest.main()

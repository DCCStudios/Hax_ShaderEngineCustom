#!/usr/bin/env python3
"""Fallout 4 FXP explorer and DXBC extractor.

The tool uses only the Python standard library.  Run without arguments for the
Tk GUI, or use ``--list`` / ``--extract-all`` for scriptable inspection.

The format below is confirmed from Fallout 4 OG 1.10.163:

* ``BSShader::Load`` at 0x142891450 reads five uint32 stage counts in the
  order VS, HS, DS, PS, CS, then consumes that many stage records.
* ``Renderer::CreatePixelShaderFromStream`` at 0x141D10D40 reads a 48-byte
  record header followed by DXBC.
* The VS/HS/DS/CS stream readers at 0x141D10580, 0x141D10880,
  0x141D10A30, and 0x141D10FB0 read a 56-byte header followed by DXBC.

Shaders011.fxp concatenates many such sections without a top-level section
count.  Parsing therefore continues until the file ends, with every boundary,
magic value, bytecode size, and DXBC chunk table validated along the way.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import queue
import re
import struct
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence


FXP_MAGIC = 0x11223344
STAGES = ("VS", "HS", "DS", "PS", "CS")
SECTION_HEADER_SIZE = 20
PIXEL_METADATA_SIZE = 32
OTHER_METADATA_SIZE = 40
MAX_FAMILIES = 4096
MAX_RECORDS_PER_STAGE = 1_000_000
MAX_TOTAL_RECORDS = 2_000_000

# Section ownership is confirmed by BSShaderManager::Initialize at
# 0x1427D4F90.  A section ordinal is deliberately not called fxpShaderType:
# for example, section 9 belongs to BSDFLightShader but that object's native
# shaderType is 4.  Later image-space sections are left numeric because their
# individual construction order is not encoded in the FXP itself.
SECTION_NAMES = {
    0: "BloodSpatter",
    1: "DistantTree",
    2: "Particle",
    3: "Sky",
    4: "Effect",
    5: "Lighting",
    6: "Utility",
    7: "Water",
    8: "DFPrepass",
    9: "DFLight",
    10: "DFComposite",
    11: "FaceCustomization",
    12: "DFTiledLighting",
    13: "DFDecalsCompute",
    14: "MeshCombinerCompute",
}

NATIVE_SHADER_TYPES = {
    9: 4,   # BSDFLightShader, independently confirmed at runtime
    10: 6,  # BSDFCompositeShader, independently confirmed at runtime
}


class FXPFormatError(ValueError):
    """Raised when a byte range violates the confirmed FXP contract."""

    def __init__(self, message: str, offset: int | None = None) -> None:
        self.offset = offset
        prefix = f"offset 0x{offset:X}: " if offset is not None else ""
        super().__init__(prefix + message)


@dataclass(frozen=True, slots=True)
class DXBCInfo:
    total_size: int
    chunk_count: int
    chunk_offsets: tuple[int, ...]
    chunk_tags: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class FXPRecord:
    family_index: int
    stage: str
    stage_index: int
    technique_id: int
    record_offset: int
    bytecode_offset: int
    bytecode_size: int
    metadata: bytes
    flags: bytes
    sha256: str
    dxbc: DXBCInfo

    @property
    def end_offset(self) -> int:
        return self.bytecode_offset + self.bytecode_size

    @property
    def family_name(self) -> str:
        return SECTION_NAMES.get(self.family_index, "")

    @property
    def native_shader_type(self) -> int | None:
        return NATIVE_SHADER_TYPES.get(self.family_index)

    @property
    def key(self) -> str:
        return f"{self.family_index}:{self.stage}:{self.stage_index}"

    @property
    def filename(self) -> str:
        # Matches the input convention used by generate_fxp_shadow_variants.py.
        # Section directories keep identical IDs from unrelated owners apart.
        return f"{self.stage}_{self.technique_id:08X}.bin"

    @property
    def section_directory(self) -> str:
        suffix = re.sub(r"[^A-Za-z0-9_.-]+", "_", self.family_name).strip("_")
        return f"Section_{self.family_index:03d}" + (f"_{suffix}" if suffix else "")


@dataclass(frozen=True, slots=True)
class FXPFamily:
    index: int
    offset: int
    end_offset: int
    counts: tuple[int, int, int, int, int]
    records: tuple[FXPRecord, ...]

    @property
    def name(self) -> str:
        return SECTION_NAMES.get(self.index, "")


@dataclass(frozen=True, slots=True)
class FXPArchive:
    path: Path
    data: bytes
    families: tuple[FXPFamily, ...]
    records: tuple[FXPRecord, ...]

    def bytecode(self, record: FXPRecord) -> bytes:
        return self.data[record.bytecode_offset : record.end_offset]


def _u32(data: bytes, offset: int, context: str) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise FXPFormatError(f"truncated {context}", offset)
    return struct.unpack_from("<I", data, offset)[0]


def validate_dxbc(blob: bytes, file_offset: int) -> DXBCInfo:
    """Validate a DXBC container and return its bounded chunk directory."""

    if len(blob) < 32:
        raise FXPFormatError("DXBC container is smaller than 32 bytes", file_offset)
    if blob[:4] != b"DXBC":
        signature = blob[:4].hex(" ").upper()
        raise FXPFormatError(
            f"expected DXBC bytecode, found {signature or 'EOF'}", file_offset
        )

    total_size, chunk_count = struct.unpack_from("<II", blob, 24)
    if total_size != len(blob):
        raise FXPFormatError(
            f"DXBC size field is {total_size}, record declares {len(blob)}",
            file_offset + 24,
        )
    directory_end = 32 + chunk_count * 4
    if directory_end > total_size:
        raise FXPFormatError("DXBC chunk directory exceeds container", file_offset + 28)

    chunk_offsets: list[int] = []
    chunk_tags: list[str] = []
    for chunk_index in range(chunk_count):
        chunk_offset = struct.unpack_from("<I", blob, 32 + chunk_index * 4)[0]
        if chunk_offset < directory_end or chunk_offset + 8 > total_size:
            raise FXPFormatError(
                f"DXBC chunk {chunk_index} has invalid offset 0x{chunk_offset:X}",
                file_offset + 32 + chunk_index * 4,
            )
        chunk_size = struct.unpack_from("<I", blob, chunk_offset + 4)[0]
        if chunk_offset + 8 + chunk_size > total_size:
            raise FXPFormatError(
                f"DXBC chunk {chunk_index} exceeds container", file_offset + chunk_offset
            )
        tag_bytes = blob[chunk_offset : chunk_offset + 4]
        tag = tag_bytes.decode("ascii", errors="replace")
        chunk_offsets.append(chunk_offset)
        chunk_tags.append(tag)

    return DXBCInfo(total_size, chunk_count, tuple(chunk_offsets), tuple(chunk_tags))


def parse_fxp(path: str | os.PathLike[str]) -> FXPArchive:
    source = Path(path).expanduser().resolve()
    try:
        data = source.read_bytes()
    except OSError as exc:
        raise FXPFormatError(f"could not read {source}: {exc}") from exc
    if not data:
        raise FXPFormatError("file is empty", 0)

    families: list[FXPFamily] = []
    all_records: list[FXPRecord] = []
    offset = 0

    while offset < len(data):
        family_index = len(families)
        if family_index >= MAX_FAMILIES:
            raise FXPFormatError(
                f"section count exceeds safety limit {MAX_FAMILIES}", offset
            )
        if len(data) - offset < SECTION_HEADER_SIZE:
            raise FXPFormatError(
                f"trailing {len(data) - offset} bytes cannot form a section header",
                offset,
            )

        family_offset = offset
        counts = struct.unpack_from("<5I", data, offset)
        offset += SECTION_HEADER_SIZE
        for stage, count in zip(STAGES, counts):
            if count > MAX_RECORDS_PER_STAGE:
                raise FXPFormatError(
                    f"{stage} count {count} exceeds safety limit "
                    f"{MAX_RECORDS_PER_STAGE}",
                    family_offset + STAGES.index(stage) * 4,
                )

        family_records: list[FXPRecord] = []
        for stage, count in zip(STAGES, counts):
            metadata_size = (
                PIXEL_METADATA_SIZE if stage == "PS" else OTHER_METADATA_SIZE
            )
            header_size = 12 + metadata_size + 4
            for stage_index in range(count):
                if len(all_records) >= MAX_TOTAL_RECORDS:
                    raise FXPFormatError(
                        f"record count exceeds safety limit {MAX_TOTAL_RECORDS}", offset
                    )
                record_offset = offset
                if offset + header_size > len(data):
                    raise FXPFormatError(
                        f"truncated {stage} record header for section {family_index}",
                        offset,
                    )

                magic, bytecode_size, technique_id = struct.unpack_from(
                    "<III", data, offset
                )
                if magic != FXP_MAGIC:
                    raise FXPFormatError(
                        f"bad record magic 0x{magic:08X}; expected 0x{FXP_MAGIC:08X}",
                        offset,
                    )
                metadata_start = offset + 12
                metadata_end = metadata_start + metadata_size
                flags = data[metadata_end : metadata_end + 4]
                bytecode_offset = metadata_end + 4
                end_offset = bytecode_offset + bytecode_size
                if end_offset < bytecode_offset or end_offset > len(data):
                    raise FXPFormatError(
                        f"{stage} bytecode size {bytecode_size} exceeds file", offset + 4
                    )

                blob = data[bytecode_offset:end_offset]
                dxbc = validate_dxbc(blob, bytecode_offset)
                record = FXPRecord(
                    family_index=family_index,
                    stage=stage,
                    stage_index=stage_index,
                    technique_id=technique_id,
                    record_offset=record_offset,
                    bytecode_offset=bytecode_offset,
                    bytecode_size=bytecode_size,
                    metadata=data[metadata_start:metadata_end],
                    flags=flags,
                    sha256=hashlib.sha256(blob).hexdigest(),
                    dxbc=dxbc,
                )
                family_records.append(record)
                all_records.append(record)
                offset = end_offset

        families.append(
            FXPFamily(
                index=family_index,
                offset=family_offset,
                end_offset=offset,
                counts=tuple(counts),
                records=tuple(family_records),
            )
        )

    return FXPArchive(source, data, tuple(families), tuple(all_records))


def _unique_output_path(path: Path, expected: bytes | None = None) -> Path:
    if not path.exists():
        return path
    if expected is not None:
        try:
            if path.read_bytes() == expected:
                return path
        except OSError:
            pass
    for suffix in range(1, 10_000):
        candidate = path.with_name(f"{path.stem}_{suffix}{path.suffix}")
        if not candidate.exists():
            return candidate
    raise OSError(f"could not find an unused filename for {path}")


def extract_records(
    archive: FXPArchive,
    records: Iterable[FXPRecord],
    output_dir: str | os.PathLike[str],
    progress: Callable[[int, int], None] | None = None,
) -> list[Path]:
    selected = tuple(records)
    destination = Path(output_dir).expanduser().resolve()
    destination.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    manifest_rows: list[dict[str, str | int]] = []

    for index, record in enumerate(selected, 1):
        blob = archive.bytecode(record)
        section_dir = destination / record.section_directory
        section_dir.mkdir(parents=True, exist_ok=True)
        output = _unique_output_path(section_dir / record.filename, blob)
        if not output.exists():
            output.write_bytes(blob)
        written.append(output)
        manifest_rows.append(
            {
                "section": record.family_index,
                "sectionOwner": record.family_name,
                "fxpShaderType": (
                    f"0x{record.native_shader_type:08X}"
                    if record.native_shader_type is not None
                    else ""
                ),
                "stage": record.stage,
                "stageIndex": record.stage_index,
                "techniqueID": f"0x{record.technique_id:08X}",
                "recordOffset": f"0x{record.record_offset:X}",
                "bytecodeOffset": f"0x{record.bytecode_offset:X}",
                "bytecodeSize": record.bytecode_size,
                "flags": record.flags.hex().upper(),
                "metadata": record.metadata.hex().upper(),
                "sha256": record.sha256,
                "file": str(output.relative_to(destination)),
            }
        )
        if progress:
            progress(index, len(selected))

    if manifest_rows:
        manifest = _unique_output_path(destination / "manifest.csv")
        with manifest.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(manifest_rows[0]))
            writer.writeheader()
            writer.writerows(manifest_rows)
    return written


def _family_label(family: FXPFamily) -> str:
    name = f" ({family.name})" if family.name else ""
    return f"{family.index:03d}{name}"


def _record_matches(
    record: FXPRecord, family: str, stage: str, query: str
) -> bool:
    if family != "All" and record.family_index != int(family.split()[0]):
        return False
    if stage != "All" and record.stage != stage:
        return False
    if not query:
        return True
    normalized = query.strip().lower()
    searchable = " ".join(
        (
            str(record.family_index),
            record.family_name.lower(),
            record.stage.lower(),
            str(record.stage_index),
            str(record.technique_id),
            f"0x{record.technique_id:08x}",
            f"{record.technique_id:08x}",
            record.sha256,
            record.dxbc.chunk_tags and " ".join(record.dxbc.chunk_tags).lower()
            or "",
        )
    )
    return normalized in searchable


class FXPExplorerApp:
    def __init__(self, initial_path: Path | None = None) -> None:
        import tkinter as tk
        from tkinter import ttk

        self.tk = tk
        self.ttk = ttk
        self.root = tk.Tk()
        self.root.title("Fallout 4 FXP Explorer")
        self.root.geometry("1320x820")
        self.root.minsize(940, 560)
        self.archive: FXPArchive | None = None
        self.visible_records: list[FXPRecord] = []
        self.record_by_key: dict[str, FXPRecord] = {}
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self._filter_after: str | None = None

        self.path_var = tk.StringVar()
        default_decompiler = Path(r"C:\Assets\HLSLDecompiler\cmd_Decompiler.exe")
        self.decompiler_var = tk.StringVar(
            value=str(default_decompiler) if default_decompiler.is_file() else ""
        )
        self.search_var = tk.StringVar()
        self.family_var = tk.StringVar(value="All")
        self.stage_var = tk.StringVar(value="All")
        self.status_var = tk.StringVar(value="Open a Fallout 4 .fxp file.")

        self._build_ui()
        self.root.after(100, self._poll_events)
        if initial_path:
            self.root.after(0, lambda: self.open_path(initial_path))

    def _build_ui(self) -> None:
        from tkinter import ttk

        toolbar = ttk.Frame(self.root, padding=6)
        toolbar.pack(fill="x")
        ttk.Button(toolbar, text="Open FXP...", command=self.choose_file).pack(
            side="left"
        )
        ttk.Entry(toolbar, textvariable=self.path_var).pack(
            side="left", fill="x", expand=True, padx=(6, 6)
        )
        ttk.Button(toolbar, text="Reload", command=self.reload).pack(side="left")

        filters = ttk.Frame(self.root, padding=(6, 0, 6, 6))
        filters.pack(fill="x")
        ttk.Label(filters, text="Section").pack(side="left")
        self.family_combo = ttk.Combobox(
            filters,
            textvariable=self.family_var,
            values=("All",),
            width=23,
            state="readonly",
        )
        self.family_combo.pack(side="left", padx=(4, 10))
        ttk.Label(filters, text="Stage").pack(side="left")
        ttk.Combobox(
            filters,
            textvariable=self.stage_var,
            values=("All",) + STAGES,
            width=6,
            state="readonly",
        ).pack(side="left", padx=(4, 10))
        ttk.Label(filters, text="Search").pack(side="left")
        ttk.Entry(filters, textvariable=self.search_var).pack(
            side="left", fill="x", expand=True, padx=(4, 10)
        )
        ttk.Button(
            filters, text="Extract selected...", command=self.extract_selected
        ).pack(side="left", padx=(0, 4))
        ttk.Button(
            filters, text="Extract visible...", command=self.extract_visible
        ).pack(side="left", padx=(0, 4))
        ttk.Button(
            filters, text="Decompile selected...", command=self.decompile_selected
        ).pack(side="left")

        self.family_var.trace_add("write", self._schedule_filter)
        self.stage_var.trace_add("write", self._schedule_filter)
        self.search_var.trace_add("write", self._schedule_filter)

        pane = ttk.Panedwindow(self.root, orient="vertical")
        pane.pack(fill="both", expand=True, padx=6)
        tree_frame = ttk.Frame(pane)
        detail_frame = ttk.Frame(pane)
        pane.add(tree_frame, weight=3)
        pane.add(detail_frame, weight=1)

        columns = (
            "family",
            "name",
            "stage",
            "index",
            "technique",
            "size",
            "offset",
            "sha",
        )
        self.tree = ttk.Treeview(
            tree_frame,
            columns=columns,
            show="headings",
            selectmode="extended",
        )
        headings = {
            "family": "Section",
            "name": "Owner",
            "stage": "Stage",
            "index": "Index",
            "technique": "Technique ID",
            "size": "DXBC bytes",
            "offset": "Record offset",
            "sha": "SHA-256",
        }
        widths = {
            "family": 65,
            "name": 125,
            "stage": 55,
            "index": 65,
            "technique": 110,
            "size": 90,
            "offset": 105,
            "sha": 220,
        }
        for column in columns:
            self.tree.heading(
                column,
                text=headings[column],
                command=lambda value=column: self._sort_tree(value, False),
            )
            self.tree.column(column, width=widths[column], stretch=column == "sha")
        y_scroll = ttk.Scrollbar(
            tree_frame, orient="vertical", command=self.tree.yview
        )
        x_scroll = ttk.Scrollbar(
            tree_frame, orient="horizontal", command=self.tree.xview
        )
        self.tree.configure(yscrollcommand=y_scroll.set, xscrollcommand=x_scroll.set)
        self.tree.grid(row=0, column=0, sticky="nsew")
        y_scroll.grid(row=0, column=1, sticky="ns")
        x_scroll.grid(row=1, column=0, sticky="ew")
        tree_frame.rowconfigure(0, weight=1)
        tree_frame.columnconfigure(0, weight=1)
        self.tree.bind("<<TreeviewSelect>>", self._show_selection)

        decompiler_bar = ttk.Frame(detail_frame)
        decompiler_bar.pack(fill="x", pady=(4, 3))
        ttk.Label(decompiler_bar, text="Decompiler").pack(side="left")
        ttk.Entry(decompiler_bar, textvariable=self.decompiler_var).pack(
            side="left", fill="x", expand=True, padx=(6, 6)
        )
        ttk.Button(
            decompiler_bar, text="Browse...", command=self.choose_decompiler
        ).pack(side="left")

        self.details = self.tk.Text(
            detail_frame,
            height=10,
            wrap="none",
            font=("Consolas", 10),
            state="disabled",
        )
        self.details.pack(fill="both", expand=True)
        ttk.Label(
            self.root,
            textvariable=self.status_var,
            anchor="w",
            relief="sunken",
            padding=(5, 2),
        ).pack(fill="x", padx=6, pady=(5, 6))

    def run(self) -> None:
        self.root.mainloop()

    def choose_file(self) -> None:
        from tkinter import filedialog

        selected = filedialog.askopenfilename(
            title="Open Fallout 4 FXP",
            filetypes=(("Fallout 4 shader package", "*.fxp"), ("All files", "*.*")),
        )
        if selected:
            self.open_path(Path(selected))

    def choose_decompiler(self) -> None:
        from tkinter import filedialog

        selected = filedialog.askopenfilename(
            title="Select HLSLDecompiler",
            filetypes=(("Executable", "*.exe"), ("All files", "*.*")),
        )
        if selected:
            self.decompiler_var.set(selected)

    def reload(self) -> None:
        value = self.path_var.get().strip()
        if value:
            self.open_path(Path(value))

    def open_path(self, path: Path) -> None:
        self.path_var.set(str(path))
        self.status_var.set(f"Parsing {path}...")
        threading.Thread(target=self._parse_worker, args=(path,), daemon=True).start()

    def _parse_worker(self, path: Path) -> None:
        try:
            self.events.put(("parsed", parse_fxp(path)))
        except Exception as exc:  # displayed with exact parser context in the GUI
            self.events.put(("error", exc))

    def _poll_events(self) -> None:
        from tkinter import messagebox

        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "parsed":
                    self._accept_archive(payload)  # type: ignore[arg-type]
                elif kind == "error":
                    self.status_var.set("Operation failed.")
                    messagebox.showerror("FXP Explorer", str(payload))
                elif kind == "status":
                    self.status_var.set(str(payload))
                elif kind == "done":
                    self.status_var.set(str(payload))
                    messagebox.showinfo("FXP Explorer", str(payload))
        except queue.Empty:
            pass
        self.root.after(100, self._poll_events)

    def _accept_archive(self, archive: FXPArchive) -> None:
        self.archive = archive
        self.record_by_key = {record.key: record for record in archive.records}
        self.family_combo.configure(
            values=("All",) + tuple(_family_label(family) for family in archive.families)
        )
        self.family_var.set("All")
        self.stage_var.set("All")
        self.search_var.set("")
        self._apply_filter()
        self.status_var.set(
            f"{archive.path.name}: {len(archive.families)} sections, "
            f"{len(archive.records)} shaders, {len(archive.data):,} bytes"
        )

    def _schedule_filter(self, *_args: object) -> None:
        if self._filter_after:
            self.root.after_cancel(self._filter_after)
        self._filter_after = self.root.after(120, self._apply_filter)

    def _apply_filter(self) -> None:
        self._filter_after = None
        if not self.archive:
            return
        family = self.family_var.get()
        stage = self.stage_var.get()
        query = self.search_var.get()
        self.visible_records = [
            record
            for record in self.archive.records
            if _record_matches(record, family, stage, query)
        ]
        self.tree.delete(*self.tree.get_children())
        for record in self.visible_records:
            self.tree.insert(
                "",
                "end",
                iid=record.key,
                values=(
                    record.family_index,
                    record.family_name,
                    record.stage,
                    record.stage_index,
                    f"0x{record.technique_id:08X}",
                    record.bytecode_size,
                    f"0x{record.record_offset:X}",
                    record.sha256,
                ),
            )
        self.status_var.set(f"Showing {len(self.visible_records):,} shaders")

    def _sort_tree(self, column: str, descending: bool) -> None:
        items = [(self.tree.set(item, column), item) for item in self.tree.get_children("")]
        numeric = column in {"family", "index", "size", "offset", "technique"}

        def key(item: tuple[str, str]) -> object:
            value = item[0]
            if numeric:
                try:
                    return int(value, 0)
                except ValueError:
                    return -1
            return value.lower()

        items.sort(key=key, reverse=descending)
        for position, (_value, item) in enumerate(items):
            self.tree.move(item, "", position)
        self.tree.heading(
            column,
            command=lambda: self._sort_tree(column, not descending),
        )

    def _selected_records(self) -> list[FXPRecord]:
        return [
            self.record_by_key[key]
            for key in self.tree.selection()
            if key in self.record_by_key
        ]

    def _show_selection(self, _event: object = None) -> None:
        records = self._selected_records()
        if not records:
            value = ""
        elif len(records) > 1:
            total = sum(record.bytecode_size for record in records)
            value = f"{len(records)} shaders selected, {total:,} DXBC bytes"
        else:
            record = records[0]
            family = self.archive.families[record.family_index] if self.archive else None
            counts = family.counts if family else (0, 0, 0, 0, 0)
            value = "\n".join(
                (
                    f"Section       : {record.family_index} {record.family_name}",
                    f"fxpShaderType : "
                    + (
                        f"0x{record.native_shader_type:08X}"
                        if record.native_shader_type is not None
                        else "not encoded / unmapped"
                    ),
                    f"Section counts: "
                    + ", ".join(
                        f"{stage}={count}" for stage, count in zip(STAGES, counts)
                    ),
                    f"Stage/index   : {record.stage}/{record.stage_index}",
                    f"Technique ID  : 0x{record.technique_id:08X}",
                    f"Record range  : 0x{record.record_offset:X}..0x{record.end_offset:X}",
                    f"DXBC range    : 0x{record.bytecode_offset:X}..0x{record.end_offset:X}",
                    f"DXBC size     : {record.bytecode_size:,}",
                    f"Flags         : {record.flags.hex(' ').upper()}",
                    f"Metadata      : {record.metadata.hex(' ').upper()}",
                    f"DXBC chunks   : {record.dxbc.chunk_count} "
                    + ", ".join(record.dxbc.chunk_tags),
                    f"SHA-256       : {record.sha256}",
                    f"Output path   : {record.section_directory}/{record.filename}",
                )
            )
        self.details.configure(state="normal")
        self.details.delete("1.0", "end")
        self.details.insert("1.0", value)
        self.details.configure(state="disabled")

    def extract_selected(self) -> None:
        self._choose_extract(self._selected_records())

    def extract_visible(self) -> None:
        self._choose_extract(self.visible_records)

    def _choose_extract(self, records: Sequence[FXPRecord]) -> None:
        from tkinter import filedialog, messagebox

        if not self.archive or not records:
            messagebox.showwarning("FXP Explorer", "No shaders are selected.")
            return
        destination = filedialog.askdirectory(title="Extract DXBC shaders")
        if not destination:
            return
        self.status_var.set(f"Extracting {len(records):,} shaders...")
        threading.Thread(
            target=self._extract_worker,
            args=(self.archive, tuple(records), Path(destination)),
            daemon=True,
        ).start()

    def _extract_worker(
        self, archive: FXPArchive, records: Sequence[FXPRecord], destination: Path
    ) -> None:
        try:
            files = extract_records(
                archive,
                records,
                destination,
                progress=lambda done, total: self.events.put(
                    ("status", f"Extracting {done:,}/{total:,} shaders...")
                ),
            )
            self.events.put(
                ("done", f"Extracted {len(files):,} shaders to\n{destination}")
            )
        except Exception as exc:
            self.events.put(("error", exc))

    def decompile_selected(self) -> None:
        from tkinter import filedialog, messagebox

        if not self.archive:
            return
        records = self._selected_records()
        if not records:
            messagebox.showwarning("FXP Explorer", "No shaders are selected.")
            return
        executable = Path(self.decompiler_var.get().strip()).expanduser()
        if not executable.is_file():
            messagebox.showerror("FXP Explorer", "Select a valid decompiler executable.")
            return
        destination = filedialog.askdirectory(
            title="Extract and decompile selected shaders"
        )
        if not destination:
            return
        self.status_var.set(f"Decompiling {len(records):,} shaders...")
        threading.Thread(
            target=self._decompile_worker,
            args=(self.archive, tuple(records), Path(destination), executable),
            daemon=True,
        ).start()

    def _decompile_worker(
        self,
        archive: FXPArchive,
        records: Sequence[FXPRecord],
        destination: Path,
        executable: Path,
    ) -> None:
        try:
            files = extract_records(archive, records, destination)
            failures: list[str] = []
            for index, path in enumerate(files, 1):
                self.events.put(
                    ("status", f"Decompiling {index:,}/{len(files):,}: {path.name}")
                )
                result = subprocess.run(
                    [str(executable), "-D", str(path)],
                    cwd=str(executable.parent),
                    capture_output=True,
                    text=True,
                    errors="replace",
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                    check=False,
                )
                if result.returncode != 0 or not path.with_suffix(".hlsl").is_file():
                    message = (result.stderr or result.stdout).strip()
                    failures.append(f"{path.name}: {message or 'no HLSL output'}")
            if failures:
                preview = "\n".join(failures[:10])
                raise RuntimeError(
                    f"{len(failures)} of {len(files)} shaders failed:\n{preview}"
                )
            self.events.put(
                ("done", f"Decompiled {len(files):,} shaders to\n{destination}")
            )
        except Exception as exc:
            self.events.put(("error", exc))


def _print_listing(archive: FXPArchive) -> None:
    writer = csv.writer(sys.stdout, dialect="excel-tab", lineterminator="\n")
    writer.writerow(
        (
            "section",
            "sectionOwner",
            "fxpShaderType",
            "stage",
            "stageIndex",
            "techniqueID",
            "bytecodeSize",
            "recordOffset",
            "flags",
            "chunks",
            "sha256",
        )
    )
    for record in archive.records:
        writer.writerow(
            (
                record.family_index,
                record.family_name,
                (
                    f"0x{record.native_shader_type:08X}"
                    if record.native_shader_type is not None
                    else ""
                ),
                record.stage,
                record.stage_index,
                f"0x{record.technique_id:08X}",
                record.bytecode_size,
                f"0x{record.record_offset:X}",
                record.flags.hex().upper(),
                ",".join(record.dxbc.chunk_tags),
                record.sha256,
            )
        )


def _parse_id(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from exc


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fxp", nargs="?", type=Path, help="FXP file to open")
    parser.add_argument("--list", action="store_true", help="print a TSV listing")
    parser.add_argument(
        "--extract-all",
        metavar="DIRECTORY",
        type=Path,
        help="extract matching records without opening the GUI",
    )
    parser.add_argument(
        "--section",
        "--family",
        dest="family",
        type=int,
        help="limit CLI extraction/listing by archive section ordinal",
    )
    parser.add_argument("--stage", choices=STAGES, help="limit CLI extraction/listing")
    parser.add_argument(
        "--technique-id", type=_parse_id, help="limit CLI extraction/listing"
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    cli_mode = args.list or args.extract_all is not None
    if cli_mode and args.fxp is None:
        print("error: an FXP path is required for CLI operations", file=sys.stderr)
        return 2

    if cli_mode:
        try:
            archive = parse_fxp(args.fxp)
            records = [
                record
                for record in archive.records
                if (args.family is None or record.family_index == args.family)
                and (args.stage is None or record.stage == args.stage)
                and (
                    args.technique_id is None
                    or record.technique_id == args.technique_id
                )
            ]
            if args.list:
                filtered = FXPArchive(
                    archive.path, archive.data, archive.families, tuple(records)
                )
                _print_listing(filtered)
            if args.extract_all is not None:
                files = extract_records(archive, records, args.extract_all)
                print(f"extracted {len(files)} shaders to {args.extract_all}")
            return 0
        except (FXPFormatError, OSError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

    try:
        app = FXPExplorerApp(args.fxp)
        app.run()
        return 0
    except ImportError as exc:
        print(f"error: Tkinter is unavailable: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

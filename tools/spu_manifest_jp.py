#!/usr/bin/env python3
"""Rebuild a local BLJM55005 SPU position manifest, without shipping code.

Inputs stay read-only. The Japanese EBOOT must match the audited profile SHA.
Firmware ranges are reused only after their complete prepared-file SHA matches.
Two classes of game record are emitted:
* Real EM_SPU ELF load images, using current Japanese bytes and static entry seeds.
* Legacy collected root chunks whose original FNV-1a identity is reproduced exactly
  using a candidate file-offset shift. A guessed shift alone NEVER accepts code.

The latter is a conservative bridge for raw SPURS code without an ELF container.
Rejected/unobserved/modified code remains on the runtime's exact-byte-validated
interpreter fallback. Static generation is not a runtime coverage guarantee.

Example (run from the source root; generated code/records remain outside source):
  python3 tools/spu_manifest_jp.py --prepared ../local/prepared/BLJM55005 \
    --out ../local/jp-spu/spu_manifest.txt --report ../reports/jp-spu-manifest.json
  ../local/build-host/acfa-prep spu-records --manifest ../local/jp-spu/spu_manifest.txt \
    --out ../local/jp-spu/records ../local/prepared/BLJM55005
  ../local/build-host/acfa-spurecomp ../local/gen/spu ../local/jp-spu/records
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import struct
from pathlib import Path

LS_SIZE = 0x40000
JP_SHA256 = "ee03439ea2cbda0de31b32848bc8b686c60dc0761b54db25be7281afbe20d5d2"
DIRECT = {"BR", "BRA", "BRZ", "BRNZ", "BRHZ", "BRHNZ", "BRSL", "BRASL"}
CALL = {"BRSL", "BRASL"}
INDIRECT = {"BI", "IRET"}
INDIRECT_CALL = {"BISL", "BISLED"}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def opcode_table(path: Path) -> list[str]:
    """Use the shipped ISA's exact longest-opcode-first table, not a second ISA."""
    defs = [(n, int(b), int(c, 0)) for n, b, c in
            re.findall(r"D\((\w+),\s*(\d+),\s*(0x[0-9A-Fa-f]+)\)", path.read_text())]
    if len(defs) < 190:
        raise ValueError("SPU ISA definitions were not recognized")
    table = ["UNK"] * 2048
    for width in range(4, 12):
        for name, bits, code in defs:
            if bits == width:
                shift = 11 - bits
                for low in range(1 << shift):
                    table[(code << shift) | low] = name
    return table


def target(op: str, word: int, pc: int) -> int:
    imm = (word >> 7) & 0xffff
    imm = imm - 0x10000 if imm & 0x8000 else imm
    return ((0 if op in {"BRA", "BRASL"} else pc) + imm * 4) & 0x3fffc


def discover(ls: bytes, entry: int, table: list[str]) -> tuple[list[tuple[int, int]], int]:
    """Byte-for-byte equivalent chunk ranges/hash to spu::discover_chunk/hash."""
    entry &= 0x3fffc
    seen: set[int] = set()
    work = [entry]
    while work and len(seen) < 8192:
        pc = work.pop()
        while pc < LS_SIZE and pc not in seen and len(seen) < 8192:
            word = struct.unpack_from(">I", ls, pc)[0]
            op = table[word >> 21]
            if op == "UNK" or word == 0:
                break
            seen.add(pc)
            if op in DIRECT:
                if op in {"BR", "BRA"}:
                    work.append(target(op, word, pc))
                    break
                if op in CALL:
                    break
                work.append(target(op, word, pc))
            elif op in INDIRECT or op in INDIRECT_CALL:
                break
            pc += 4
    ranges: list[tuple[int, int]] = []
    for pc in sorted(seen):
        if ranges and ranges[-1][1] == pc:
            ranges[-1] = (ranges[-1][0], pc + 4)
        else:
            ranges.append((pc, pc + 4))
    value = 0xcbf29ce484222325
    def mix(v: int) -> None:
        nonlocal value
        for byte in struct.pack("<I", v):
            value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    mix(entry)
    for start, end in ranges:
        mix(start)
        mix(end)
        for pc in range(start, end, 4):
            mix(struct.unpack_from(">I", ls, pc)[0])
    return ranges, value


def embedded_images(data: bytes, table: list[str]) -> list[dict]:
    out = []
    pos = 0
    while (base := data.find(b"\x7fELF", pos)) >= 0:
        pos = base + 4
        if data[base + 4:base + 7] != b"\x01\x02\x01":
            continue
        if base + 52 > len(data):
            continue
        h = struct.unpack_from(">HHIIIIIHHHHHH", data, base + 16)
        typ, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shstr = h
        if machine != 23:
            continue
        if typ != 2 or version != 1 or ehsize != 52 or phsize != 32 or not 0 < phnum <= 32:
            raise ValueError(f"malformed SPU ELF at {base:x}")
        if base + phoff + phsize * phnum > len(data) or shsize != 40 or not 0 < shnum < 128:
            raise ValueError(f"SPU ELF table bounds at {base:x}")
        if base + shoff + shsize * shnum > len(data) or shstr >= shnum:
            raise ValueError(f"SPU ELF section bounds at {base:x}")
        ph = [struct.unpack_from(">IIIIIIII", data, base + phoff + i * phsize) for i in range(phnum)]
        sh = [struct.unpack_from(">IIIIIIIIII", data, base + shoff + i * shsize) for i in range(shnum)]
        ss = sh[shstr]
        names = data[base + ss[4]:base + ss[4] + ss[5]]
        loads = []
        ls = bytearray(LS_SIZE)
        for pt, offset, va, pa, size, mem, pf, align in ph:
            if pt != 1:
                continue
            if size > mem or va + mem > LS_SIZE or base + offset + size > len(data):
                raise ValueError(f"SPU PT_LOAD bounds at {base:x}")
            if any(va < b and a < va + mem for a, b, *_ in loads):
                raise ValueError("overlapping SPU PT_LOADs")
            ls[va:va + size] = data[base + offset:base + offset + size]
            if size:
                loads.append((va, va + size, 0, base + offset))
        executable = []
        name = f"elf_{base:x}"
        pointer_seeds = set()
        for no, st, sf, va, offset, size, link, info, align, entsize in sh:
            section_name = names[no:].split(b"\0", 1)[0].decode("utf-8", errors="strict")
            if st != 8 and base + offset + size > len(data):
                raise ValueError("SPU section bytes outside EBOOT")
            if section_name == ".note.spu_name":
                note = data[base + offset:base + offset + size]
                if len(note) >= 20 and note[12:20] == b"SPUNAME\0":
                    name = note[20:].split(b"\0", 1)[0].decode("ascii")
            if st == 1 and sf & 4 and sf & 2 and section_name != ".SpuGUID":
                if va % 4 or size % 4 or not any(a <= va and va + size <= b for a, b, *_ in loads):
                    raise ValueError("SPU executable section not in file-backed PT_LOAD")
                executable.append((va, va + size))
            elif st == 1 and sf & 2:
                for at in range(offset, offset + size - 3, 4):
                    pointer_seeds.add(struct.unpack_from(">I", data, base + at)[0])
        def in_exec(pc: int) -> bool:
            return pc % 4 == 0 and any(a <= pc < b for a, b in executable)
        if not in_exec(entry):
            raise ValueError("SPU entry not in executable section")
        seeds = {entry} | {p for p in pointer_seeds if in_exec(p)}
        for a, b in executable:
            for pc in range(a, b, 4):
                word = struct.unpack_from(">I", ls, pc)[0]
                op = table[word >> 21]
                if op in DIRECT:
                    dest = target(op, word, pc)
                    if in_exec(dest):
                        seeds.add(dest)
                if (op in CALL or op in INDIRECT_CALL or op in {"STOP", "STOPD"}) and in_exec(pc + 4):
                    seeds.add(pc + 4)
        seeds = {p for p in seeds if discover(ls, p, table)[0]}
        out.append({"base": base, "name": name, "entry": entry, "ranges": loads,
                    "executable": executable, "entries": sorted(seeds)})
    if not out:
        raise ValueError("no embedded EM_SPU ELF found")
    return out


def line_for_record(name: str, entries: list[int], ranges: list[tuple[int, int, int, int]]) -> str:
    return "record " + name + " " + ",".join(f"{x:x}" for x in entries) + " " + " ".join(
        f"{a:x}:{b:x}:{s}:{o:x}" for a, b, s, o in ranges)


def read_manifest(path: Path) -> tuple[dict[int, tuple[str, str]], list[tuple]]:
    sources, records = {}, []
    for no, line in enumerate(path.read_text().splitlines(), 1):
        words = line.split()
        if not words or words[0].startswith("#"):
            continue
        if words[0] == "source" and len(words) == 4:
            idx, rel, digest = int(words[1]), words[2], words[3]
            if Path(rel).is_absolute() or ".." in Path(rel).parts or idx in sources:
                raise ValueError("unsafe or duplicate manifest source")
            sources[idx] = (rel, digest)
        elif words[0] == "record" and len(words) >= 4:
            ranges = []
            for value in words[3:]:
                a, b, s, o = value.split(":")
                ranges.append((int(a, 16), int(b, 16), int(s), int(o, 16)))
            records.append((words[1], [int(e, 16) for e in words[2].split(",")], ranges))
        else:
            raise ValueError(f"malformed manifest line {no}")
    return sources, records


def build(prepared: Path, legacy: Path, isa: Path, shifts: list[int]) -> tuple[str, dict]:
    eboot = (prepared / "eboot.elf").read_bytes()
    if sha(eboot) != JP_SHA256:
        raise ValueError("EBOOT does not match audited BLJM55005 profile; refusing a guessed regional mapping")
    table = opcode_table(isa)
    sources, records = read_manifest(legacy)
    available = {0: eboot}
    matched_sources = {0: ("eboot.elf", JP_SHA256)}
    for idx, (rel, digest) in sources.items():
        if idx == 0:
            continue
        p = prepared / rel
        if p.is_file():
            value = p.read_bytes()
            if sha(value) == digest:
                available[idx] = value
                matched_sources[idx] = (rel, digest)
    if len(available) != 11:
        raise ValueError("expected all ten audited firmware modules to match a manifest version")
    images = embedded_images(eboot, table)
    out = ["# ACFA BLJM55005 SPU positions v1; no code bytes.",
           "# Generated by tools/spu_manifest_jp.py from strict Japanese input and matched firmware.",
           "# Embedded ELF static entries plus identity-verified legacy roots; not complete runtime coverage.",
           "# Candidate file shifts accept a root ONLY after its original chunk FNV-1a matches.",
           "# Runtime still compares every compiled chunk's complete LS bytes before dispatch."]
    out += [f"source {idx} {rel} {digest}" for idx, (rel, digest) in sorted(matched_sources.items())]
    firmware_records = firmware_partial = 0
    used = set()
    for name, entries, ranges in records:
        keep = [r for r in ranges if r[2] != 0 and r[2] in available]
        ent = [e for e in entries if any(a <= e < b for a, b, *_ in keep)]
        if not keep or not ent:
            continue
        if len(keep) != len(ranges):
            firmware_partial += 1
        out.append(line_for_record("fw_" + name, ent, keep))
        firmware_records += 1
    for image in images:
        out.append(line_for_record(f"jp_elf_{image['base']:x}", image["entries"], image["ranges"]))
    # Legacy record names are the exact hash of ONE original root, not of all
    # listed entries. Never accept the remaining closure just because one root
    # matched. Emit only that root and the byte positions it actually traverses.
    accepted, rejected = [], collections.Counter()
    unmatched_roots = []
    for name, entries, ranges in records:
        if not any(r[2] == 0 for r in ranges):
            continue
        if any(s != 0 and s not in available for _, _, s, _ in ranges):
            rejected["other_firmware_version"] += 1
            continue
        if not re.fullmatch(r"[0-9a-fA-F]{16}", name):
            raise ValueError("legacy root name is not a chunk hash")
        found = False
        for shift in shifts:
            ls = bytearray(LS_SIZE)
            mapping = []
            for a, b, s, off in ranges:
                pos = off + (shift if s == 0 else 0)
                if a % 4 or b % 4 or not 0 <= a < b <= LS_SIZE or pos < 0 or pos + b - a > len(available[s]):
                    break
                if mapping and a < mapping[-1][1]:
                    raise ValueError("overlapping or unsorted legacy ranges")
                ls[a:b] = available[s][pos:pos + b - a]
                mapping.append((a, b, s, pos))
            else:
                for entry in entries:
                    chunk, identity = discover(ls, entry, table)
                    if identity != int(name, 16) or not chunk:
                        continue
                    mapped = []
                    for a, b in chunk:
                        cursor = a
                        for x, y, s, off in mapping:
                            if x <= cursor < y:
                                end = min(y, b)
                                mapped.append((cursor, end, s, off + cursor - x))
                                cursor = end
                                if cursor == b:
                                    break
                        if cursor != b:
                            raise ValueError("verified chunk escaped the provided LS byte ranges")
                    key = (entry, identity)
                    if key not in used:
                        out.append(line_for_record("jp_verified_" + name, [entry], mapped))
                        accepted.append({"hash": name, "entry": entry, "candidate_shift": shift,
                                         "instruction_count": sum((b-a)//4 for a,b in chunk)})
                        used.add(key)
                    found = True
                    break
                if found:
                    break
        if not found:
            rejected["no_root_identity_match"] += 1
            unmatched_roots.append({"legacy_hash": name, "entries": entries,
                                    "source_ranges": [list(r) for r in ranges]})
    report = {"schema": 1, "profile": "BLJM55005", "eboot_sha256": JP_SHA256,
              "legacy_manifest_sha256": sha(legacy.read_bytes()), "isa_sha256": sha(isa.read_bytes()),
              "matched_firmware_sources": {str(k): list(v) for k, v in matched_sources.items() if k},
              "firmware_records": firmware_records, "firmware_partial_records": firmware_partial,
              "embedded_images": images, "verified_legacy_roots": accepted,
              "legacy_rejected": dict(rejected), "unmatched_legacy_roots": unmatched_roots,
              "candidate_file_shifts": shifts,
              "game_runtime_verified": False, "complete_runtime_coverage": False,
              "fallback": "Unobserved, modified, rejected, or runtime-overlaid chunks execute in the existing interpreter; LS byte comparison gates every compiled dispatch."}
    return "\n".join(out) + "\n", report


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepared", type=Path, required=True)
    parser.add_argument("--legacy", type=Path, default=root / "data/spu_manifest.txt")
    parser.add_argument("--isa", type=Path, default=root / "src/spu/spu_isa.cpp")
    parser.add_argument("--candidate-eboot-shift", action="append", type=lambda v: int(v, 0))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    # 0x780 is a candidate observed between embedded JP ELF load mapping and the
    # dominant legacy mappings. Zero catches byte locations unchanged by region.
    value, report = build(args.prepared, args.legacy, args.isa, args.candidate_eboot_shift or [0x780, 0])
    prepared = args.prepared.resolve()
    for p in (args.out, args.report):
        if p.resolve().is_relative_to(prepared):
            parser.error("outputs must be outside the read-only prepared tree")
        if p.resolve() in {args.legacy.resolve(), args.isa.resolve()}:
            parser.error("outputs must not replace an input")
    if args.out.resolve() == args.report.resolve():
        parser.error("manifest and report outputs must differ")
    report["manifest_sha256"] = sha(value.encode())
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(value)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("profile", "firmware_records", "firmware_partial_records", "legacy_rejected")}, ensure_ascii=False))
    print(f"embedded_images={len(report['embedded_images'])} verified_legacy_roots={len(report['verified_legacy_roots'])} manifest={args.out}")


if __name__ == "__main__":
    main()

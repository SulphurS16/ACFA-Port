#!/usr/bin/env python3
"""Check actual acfa-prep/acfa-spurecomp outputs against a JP position manifest.

This is an independent SPUREC wire-format round trip, not a game runtime test.
It checks every rebuilt range against the original prepared-file bytes, every
entry/range table against its manifest, and every verified legacy root against
the native C++ recompiler's emitted chunk identities.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from spu_manifest_jp import JP_SHA256, read_manifest


def audit(prepared: Path, manifest: Path, generation_report: Path,
          records: Path, generated: Path) -> dict:
    sources, entries = read_manifest(manifest)
    payloads = {}
    for idx, (rel, digest) in sources.items():
        data = (prepared / rel).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"source {idx} hash mismatch")
        payloads[idx] = data
    if sources.get(0) != ("eboot.elf", JP_SHA256):
        raise ValueError("manifest does not bind audited JP source")
    expected = set()
    total_bytes = 0
    total_entries = 0
    total_ranges = 0
    for name, seeds, ranges in entries:
        filename = name + ".spurec"
        if filename in expected:
            raise ValueError("duplicate record name")
        expected.add(filename)
        data = (records / filename).read_bytes()
        if len(data) < 16 or data[:8] != b"SPUREC1\0":
            raise ValueError(f"bad record magic: {name}")
        ne, nr = struct.unpack_from("<II", data, 8)
        if (ne, nr) != (len(seeds), len(ranges)):
            raise ValueError(f"count mismatch: {name}")
        cursor = 16
        actual_entries = list(struct.unpack_from(f"<{ne}I", data, cursor))
        cursor += ne * 4
        if actual_entries != seeds:
            raise ValueError(f"entry mismatch: {name}")
        actual_ranges = [struct.unpack_from("<II", data, cursor + i * 8) for i in range(nr)]
        cursor += nr * 8
        if actual_ranges != [(a, b) for a, b, *_ in ranges]:
            raise ValueError(f"range mismatch: {name}")
        for a, b, src, off in ranges:
            length = b - a
            if not 0 <= a < b <= 0x40000 or off < 0 or off + length > len(payloads[src]):
                raise ValueError(f"range outside source: {name}")
            if data[cursor:cursor + length] != payloads[src][off:off + length]:
                raise ValueError(f"source bytes differ: {name}")
            cursor += length
        if cursor != len(data):
            raise ValueError(f"extra or missing record bytes: {name}")
        total_bytes += len(data)
        total_entries += ne
        total_ranges += nr
    actual = {p.name for p in records.glob("*.spurec")}
    if actual != expected:
        raise ValueError(f"stale/missing record files: {sorted(actual ^ expected)[:8]}")
    # Native C++ emitter computes these identities from its own ISA decoder and
    # chunk discovery. This cross-check does not reuse the Python implementation.
    table = (generated / "spu_chunks.cpp").read_text()
    native_hashes = set(re.findall(r"bool spu_c_([0-9a-f]{16})\(R::Thread& t\);", table))
    count_match = re.search(r"g_chunk_count = (\d+)u", table)
    if not count_match or len(native_hashes) != int(count_match.group(1)):
        raise ValueError("native emitted table identity/count mismatch")
    report = json.loads(generation_report.read_text())
    if report["manifest_sha256"] != hashlib.sha256(manifest.read_bytes()).hexdigest():
        raise ValueError("manifest changed after generation report")
    required = {row["hash"] for row in report["verified_legacy_roots"]}
    if not required.issubset(native_hashes):
        raise ValueError(f"Python/native chunk identity disagreement: {sorted(required-native_hashes)[:8]}")
    unmatched = {row["legacy_hash"] for row in report["unmatched_legacy_roots"]}
    return {"schema": 1, "profile": "BLJM55005", "records": len(expected),
            "record_bytes": total_bytes, "entry_seeds": total_entries, "ranges": total_ranges,
            "source_files_checked": len(payloads), "native_emitted_chunks": len(native_hashes),
            "native_verified_legacy_roots": len(required), "byte_roundtrip_passed": True,
            "unmatched_roots_covered_by_other_records": sorted(unmatched & native_hashes),
            "legacy_roots_without_same_native_identity": sorted(unmatched - native_hashes),
            "native_identity_crosscheck_passed": True, "stale_records": 0,
            "game_runtime_verified": False, "complete_runtime_coverage": False}


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--prepared", type=Path, required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--generation-report", type=Path, required=True)
    p.add_argument("--records", type=Path, required=True)
    p.add_argument("--generated", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    report = audit(args.prepared, args.manifest, args.generation_report, args.records, args.generated)
    output = args.out.resolve()
    for d in (args.prepared, args.records, args.generated):
        if output.is_relative_to(d.resolve()):
            p.error("audit report must be outside the read-only input trees")
    if output in {args.manifest.resolve(), args.generation_report.resolve()}:
        p.error("audit report must not replace input metadata")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()

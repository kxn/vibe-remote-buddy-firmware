"""Fetch the official ITU G.722.1 package and generate local decoder data.

Only numeric initializers are read. The archive and generated headers belong in
the ignored build directory; neither is licensed as part of this repository.
"""

import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import sys
import urllib.request
import zipfile

from generate_huffman_lookup import generate_lookup


URL = ("https://www.itu.int/rec/dologin_pub.asp?"
       "id=T-REC-G.722.1-200505-I%21%21SOFT-ZST-E&lang=e&type=items")
SHA256 = "1b0b5c32495af09249b30fdae68c20a1f2397bf8c69cfef332fac984f2b62580"
ROOT = Path(__file__).resolve().parents[3]
CACHE = ROOT / "build/itu-g7221/official-package.zip"
PREFIX = "Software/Fixed-200505-Rel.2.1/common/"

SPEC = {
    "huff_tab.c": [
        ("differential_region_power_bits", "uint8_t", 672),
        ("differential_region_power_codes", "uint16_t", 672),
        ("mlt_quant_centroid", "uint16_t", 128),
        ("expected_bits_table", "uint8_t", 8),
        *[(f"mlt_sqvh_{kind}_category_{category}", ctype, count)
          for category, count in enumerate((196, 100, 49, 625, 256, 243, 32))
          for kind, ctype in (("bitcount", "uint8_t"), ("code", "uint16_t"))],
    ],
    "tables.c": [
        ("int_region_standard_deviation_table", "uint16_t", 64),
        ("vector_dimension", "uint8_t", 8),
        ("number_of_vectors", "uint8_t", 8),
        ("max_bin", "uint8_t", 8),
    ],
}


def archive_bytes(path: Path | None) -> bytes:
    if path is not None:
        data = path.read_bytes()
    elif CACHE.is_file():
        data = CACHE.read_bytes()
    else:
        print(f"Downloading official ITU G.722.1 software package: {URL}", flush=True)
        with urllib.request.urlopen(URL, timeout=90) as response:
            data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SHA256:
        raise ValueError(f"ITU archive SHA-256 mismatch: {digest}; expected {SHA256}")
    if path is None and not CACHE.is_file():
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        temp = CACHE.with_suffix(".tmp")
        temp.write_bytes(data)
        temp.replace(CACHE)
    return data


def extract_numbers(source: str, name: str, count: int) -> list[int]:
    # Remove comments before finding declarations; the ITU file retains an old
    # commented-out int_dead_zone initializer.
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    match = re.search(r"\b(?:U?Word(?:16|32))\s+" + re.escape(name)
                      + r"\s*(?:\[[^\]]*\])+\s*=\s*\{", source)
    if not match:
        raise ValueError(f"ITU table missing: {name}")
    start = match.end()
    depth = 1
    end = start
    while depth:
        if end >= len(source):
            raise ValueError(f"unterminated ITU table: {name}")
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    body = source[start:end - 1]
    if re.search(r"[^\s{},+\-\dxa-fA-FuUlL]", body):
        raise ValueError(f"non-numeric initializer in ITU table: {name}")
    numbers = [int(value.rstrip("uUlL"), 0) if value.lower().startswith("0x")
               else int(value.rstrip("uUlL"))
               for value in re.findall(r"-?(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*", body)]
    if len(numbers) != count:
        raise ValueError(f"ITU table {name}: expected {count} numbers, got {len(numbers)}")
    return numbers


def numeric_header(archive: bytes) -> str:
    rows = ["/* Locally generated from the official ITU-T G.722.1 release 2.1.",
            " * Original numeric table files: Copyright 2004 Polycom, Inc.",
            " * All rights reserved. Do not redistribute this generated file.",
            " */", "#include <stdint.h>"]
    with zipfile.ZipFile(io.BytesIO(archive)) as package:
        for filename, entries in SPEC.items():
            source = package.read(PREFIX + filename).decode("latin-1")
            for name, ctype, count in entries:
                numbers = extract_numbers(source, name, count)
                limit = 255 if ctype == "uint8_t" else 65535
                if any(number < 0 or number > limit for number in numbers):
                    raise ValueError(f"ITU table out of range: {name}")
                rows.append(f"static const {ctype} itu_{name}[{count}] = {{")
                for i in range(0, count, 12):
                    rows.append("    " + ", ".join(map(str, numbers[i:i + 12])) + ",")
                rows.append("};")
    return "\n".join(rows) + "\n"


def write_if_changed(path: Path, content: str) -> None:
    if path.is_file() and path.read_text(encoding="ascii") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(content, encoding="ascii", newline="\n")
    temp.replace(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path,
                        default=Path(os.environ["BUDDY_ITU_G7221_ARCHIVE"])
                        if "BUDDY_ITU_G7221_ARCHIVE" in os.environ else None,
                        help="already downloaded official ZIP (also BUDDY_ITU_G7221_ARCHIVE)")
    parser.add_argument("--output", type=Path, required=True,
                        help="ignored local build directory for the generated headers")
    args = parser.parse_args()
    numeric = numeric_header(archive_bytes(args.archive))
    write_if_changed(args.output / "itu_g7221_numeric_tables.h", numeric)
    write_if_changed(args.output / "ico_huffman_lookup.h", generate_lookup(numeric))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as exc:
        sys.exit(f"Cannot prepare ITU decoder tables: {exc}")

"""Build the fast Huffman index from locally extracted numeric tables."""

import re


HUFFMAN_PREFIX_BITS = 10


def generate_lookup(numeric_text: str) -> str:
    huffman_tables = []
    short_lookup_names = []
    bucket_offset_names = []
    bucket_symbol_names = []
    for category, count in enumerate((196, 100, 49, 625, 256, 243, 32)):
        prefix_bits = HUFFMAN_PREFIX_BITS

        def values(name: str) -> list[int]:
            match = re.search(
                rf"static const (?:uint8_t|uint16_t) {name}\[\d+\] = \{{(.*?)\}};",
                numeric_text, re.S,
            )
            if not match:
                raise ValueError(f"missing numeric table: {name}")
            return [int(value) for value in re.findall(r"\b\d+\b", match.group(1))]

        lengths = values(f"itu_mlt_sqvh_bitcount_category_{category}")
        codes = values(f"itu_mlt_sqvh_code_category_{category}")
        if len(lengths) != count or len(codes) != count:
            raise ValueError(f"unexpected category {category} table size")
        lookup = [0xFFFF] * (1 << prefix_bits)
        ordered = sorted(enumerate(zip(codes, lengths)), key=lambda item: item[1][1])
        for symbol, (code, length) in ordered:
            if not 1 <= length <= 16 or code >= (1 << length):
                raise ValueError(f"invalid category {category} code at {symbol}")
            if length > prefix_bits:
                continue
            first = code << (prefix_bits - length)
            for prefix in range(first, first + (1 << (prefix_bits - length))):
                if lookup[prefix] != 0xFFFF:
                    old_length = lookup[prefix] >> 10
                    if old_length <= length:
                        continue
                lookup[prefix] = (length << 10) | symbol
        buckets: list[list[int]] = [[] for _ in range(1 << prefix_bits)]
        for symbol, (code, length) in enumerate(zip(codes, lengths)):
            if length > prefix_bits:
                buckets[code >> (length - prefix_bits)].append(symbol)
        offsets = [0]
        symbols = []
        for bucket in buckets:
            symbols.extend(bucket)
            offsets.append(len(symbols))

        def emit_u16(name: str, data: list[int], width: int = 12) -> str:
            rows = []
            for i in range(0, len(data), width):
                rows.append("    " + ", ".join(str(x) for x in data[i:i + width]) + ",")
            return (f"static const uint16_t {name}[{len(data)}] = {{\n"
                    + "\n".join(rows) + "\n};")

        short_name = f"ico_vq_short_{category}"
        offset_name = f"ico_vq_bucket_offsets_{category}"
        symbol_name = f"ico_vq_bucket_symbols_{category}"
        short_lookup_names.append(short_name)
        bucket_offset_names.append(offset_name)
        bucket_symbol_names.append(symbol_name)
        huffman_tables.extend((
            emit_u16(short_name, lookup),
            emit_u16(offset_name, offsets),
            emit_u16(symbol_name, symbols),
        ))

    return (
        "/* Locally generated from ITU-derived codeword and length tables.\n"
        " * Do not redistribute this generated file. */\n"
        "#include <stdint.h>\n\n"
        "typedef struct { const uint16_t *short_code, *bucket_offsets, *bucket_symbols; } ico_vq_lookup_table;\n\n"
        + "\n\n".join(huffman_tables)
        + "\n\nstatic const ico_vq_lookup_table ico_vq_lookup[7] = {\n"
        + "    "
        + ",\n    ".join(
            "{" + short_lookup_names[i] + ", " + bucket_offset_names[i]
            + ", " + bucket_symbol_names[i] + "}" for i in range(7)
        )
        + "\n};\n"
    )

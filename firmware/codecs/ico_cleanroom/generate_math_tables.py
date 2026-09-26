"""Generate Q15 transform constants from normative G.722.1 equations.

This does not read or derive from the ITU software package. The complex
twiddles are for the independently factored 640-point FFT identity used to
evaluate the 320-point type-IV DCT. The synthesis window follows section 4.7.
"""

import sys
from math import cos, pi, sin
from pathlib import Path


OUT = Path(__file__).with_name("ico_dct_iv_q15.h")


def q15(value: float) -> int:
    # Python round is round-to-nearest, ties-to-even.
    return max(-32768, min(32767, round(value * 32768)))


twiddles = [(q15(cos(2.0 * pi * phase / 1280.0)),
             q15(sin(2.0 * pi * phase / 1280.0))) for phase in range(1280)]
final_twiddles = [
    (q15(cos(2.0 * pi * j * k / 640.0 + pi * (2 * k + 1) / 1280.0)),
     q15(sin(2.0 * pi * j * k / 640.0 + pi * (2 * k + 1) / 1280.0)))
    for k in range(320)
    for j in range(5)
]
window = [q15(sin(pi * (n + 0.5) / 640.0)) for n in range(320)]


def emit(name: str, ctype: str, values: list[int], per_line: int) -> str:
    rows = []
    for i in range(0, len(values), per_line):
        rows.append("    " + ", ".join(str(x) for x in values[i : i + per_line]) + ",")
    return f"static const {ctype} {name}[{len(values)}] = {{\n" + "\n".join(rows) + "\n};\n"


twiddle_rows = [f"    {{{re}, {im}}}," for re, im in twiddles]
twiddle_table = (
    "typedef struct { int16_t re, im; } ico_complex_q15;\n"
    "static const ico_complex_q15 ico_dct_iv_fft_twiddle[1280] = {\n"
    + "\n".join(twiddle_rows)
    + "\n};\n"
)
final_rows = [f"    {{{re}, {im}}}," for re, im in final_twiddles]
final_table = (
    "static const ico_complex_q15 ico_dct_iv_final_twiddle[1600] = {\n"
    + "\n".join(final_rows)
    + "\n};\n"
)

generated_math = (
    "/* Derived solely from G.722.1 section 4.7's DCT-IV and window equations. */\n"
    "#include <stdint.h>\n\n"
    + twiddle_table
    + final_table
    + "\n"
    + emit("ico_dct_iv_q15_window", "uint16_t", window, 12)
)
if "--check" in sys.argv:
    if not OUT.is_file() or OUT.read_text(encoding="ascii") != generated_math:
        raise SystemExit(f"generated table is stale: {OUT.name}")
else:
    OUT.write_text(generated_math, encoding="ascii", newline="\n")

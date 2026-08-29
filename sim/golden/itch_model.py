"""
Thin wrapper around the C++ golden model (cpp/itch_model_cli), built from
cpp/itch_model.hpp. Mirrors mold_model.py's role for moldudp64_deframer.v:
this is the reference decoder rtl/itch_decoder.v is checked against, just
implemented in C++ and invoked as a subprocess rather than in pure Python
-- see architecture.md for why.

decode() takes the same [2-byte length][message] block-stream bytes the
RTL itself consumes (a MoldUDP64 message block or a raw historical ITCH
file, they're the same framing) and returns a list of plain dicts, one per
decoded message, in file order.
"""
import subprocess
from pathlib import Path

CLI_PATH = Path(__file__).resolve().parent.parent.parent / "cpp" / "build" / "itch_model_cli"

_FIXED_COLUMNS = [
    "seq_num",
    "msg_type",
    "stock_locate",
    "tracking_number",
    "timestamp",
    "field_count",
    "error_unknown_type",
    "error_length_mismatch",
    "error_truncated",
]


def _parse_line(line):
    cols = line.split("|")
    msg = {
        "seq_num": int(cols[0]),
        "msg_type": cols[1],
        "stock_locate": int(cols[2]),
        "tracking_number": int(cols[3]),
        "timestamp": int(cols[4]),
        "field_count": int(cols[5]),
        "error_unknown_type": cols[6] == "1",
        "error_length_mismatch": cols[7] == "1",
        "error_truncated": cols[8] == "1",
    }
    msg["fields"] = cols[len(_FIXED_COLUMNS):]
    return msg


def decode(blob: bytes) -> list[dict]:
    if not CLI_PATH.exists():
        raise FileNotFoundError(
            f"{CLI_PATH} not found -- build it first: "
            f"cmake -S cpp -B cpp/build && cmake --build cpp/build"
        )

    result = subprocess.run([str(CLI_PATH)], input=blob, capture_output=True, check=True)
    text = result.stdout.decode()
    return [_parse_line(line) for line in text.splitlines() if line]

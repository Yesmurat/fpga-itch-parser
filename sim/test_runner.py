"""
Python-based cocotb runner (cocotb_tools.runner) for udp_rx_top.
Usage: python3 sim/test_runner.py
"""
import os
import sys
from pathlib import Path

from cocotb_tools.runner import get_runner

SIM = os.environ.get("SIM", "icarus")

PROJ_ROOT = Path(__file__).resolve().parent.parent

VERILOG_SOURCES = [
    PROJ_ROOT / "rtl" / "vendor" / "eth_axis_rx.v",
    PROJ_ROOT / "rtl" / "vendor" / "ip_eth_rx_64.v",
    PROJ_ROOT / "rtl" / "vendor" / "udp_ip_rx_64.v",
    PROJ_ROOT / "rtl" / "vendor" / "udp_rx_top.v",
]

TOPLEVEL = "udp_rx_top"
MODULE = "test_udp_rx_top"


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parent))

    runner = get_runner(SIM)

    runner.build(
        verilog_sources=VERILOG_SOURCES,
        hdl_toplevel=TOPLEVEL,
        build_dir=str(PROJ_ROOT / "sim" / "sim_build"),
        always=True,
        build_args=["-g2012"],
    )

    runner.test(
        test_module=MODULE,
        hdl_toplevel=TOPLEVEL,
        test_dir=str(Path(__file__).resolve().parent),
        results_xml="results.xml",
    )


if __name__ == "__main__":
    main()

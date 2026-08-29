import argparse
import os
import sys
from pathlib import Path
from cocotb_tools.runner import get_runner

sim = os.getenv("SIM", "icarus")

proj_root = Path(__file__).resolve().parent.parent  # this file lives in sim/
sim_dir = proj_root / "sim"

VENDOR_SOURCES = [
    proj_root / "rtl" / "vendor" / "eth_axis_rx.v",
    proj_root / "rtl" / "vendor" / "ip_eth_rx_64.v",
    proj_root / "rtl" / "vendor" / "udp_ip_rx_64.v",
    proj_root / "rtl" / "vendor" / "udp_rx_top.v",
]

TARGETS = {
    "udp_rx_top": {
        "toplevel": "udp_rx_top",
        "test_module": "test_udp_rx_top",
        "sources": VENDOR_SOURCES,
    },
    "moldudp64_deframer": {
        "toplevel": "moldudp64_deframer",
        "test_module": "test_moldudp64",
        "sources": [proj_root / "rtl" / "moldudp64_deframer.v"],
    },
    "itch_decoder": {
        "toplevel": "itch_decoder",
        "test_module": "test_itch",
        "sources": [
            proj_root / "rtl" / "gearbox16.v",
            proj_root / "rtl" / "itch_decoder.v",
        ],
    },
    "itch_raw_pipeline": {
        "toplevel": "itch_raw_pipeline_top",
        "test_module": "test_itch_pipeline",
        "sources": [
            proj_root / "rtl" / "gearbox16.v",
            proj_root / "rtl" / "itch_decoder.v",
            proj_root / "rtl" / "itch_raw_deframer.v",
            proj_root / "rtl" / "itch_raw_pipeline_top.v",
        ],
    },
    "feed_parser_top": {
        "toplevel": "feed_parser_top",
        "test_module": "test_feed_parser",
        "sources": VENDOR_SOURCES + [
            proj_root / "rtl" / "moldudp64_deframer.v",
            proj_root / "rtl" / "gearbox16.v",
            proj_root / "rtl" / "itch_decoder.v",
            proj_root / "rtl" / "feed_parser_top.v",
        ],
    },
    "bench_latency": {
        "toplevel": "feed_parser_top",
        "test_module": "bench_latency",
        "sources": VENDOR_SOURCES + [
            proj_root / "rtl" / "moldudp64_deframer.v",
            proj_root / "rtl" / "gearbox16.v",
            proj_root / "rtl" / "itch_decoder.v",
            proj_root / "rtl" / "feed_parser_top.v",
        ],
    },
}


def run_target(name, cfg):
    runner = get_runner(sim)

    runner.build(
        verilog_sources=cfg["sources"],
        hdl_toplevel=cfg["toplevel"],
        build_dir=str(sim_dir / "sim_build" / name),
        always=True,
        build_args=["-g2012"],
    )

    runner.test(
        test_module=cfg["test_module"],
        hdl_toplevel=cfg["toplevel"],
        test_dir=str(sim_dir),
        results_xml=f"results_{name}.xml",
    )


def main():
    sys.path.insert(0, str(sim_dir))

    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=list(TARGETS) + ["all"], default="all")
    args = parser.parse_args()

    names = list(TARGETS) if args.target == "all" else [args.target]
    for name in names:
        run_target(name, TARGETS[name])


if __name__ == "__main__":
    main()

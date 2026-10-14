#!/usr/bin/env python3
"""Checks that the CPI stack sums exactly to total cycles (docs/phase3-design.md §5).

Runs rvsim-pipe --stats on every program under every given config, and
verifies sum(cpi_stack_cycles) == cycles, and that the branch breakdown sums
to the branch category. The model also asserts this internally at every halt;
this script checks it from the outside, on the emitted JSON.

    scripts/check_cpi_sum.py BUILD_DIR CONFIG.cfg [CONFIG.cfg ...]
"""
import glob
import json
import os
import subprocess
import sys


def main() -> int:
    build, configs = sys.argv[1], sys.argv[2:]
    pipe = os.path.join(build, "rvsim-pipe")
    programs = sorted(glob.glob(os.path.join(build, "tests", "riscv-tests", "*.elf")))
    runs = bad = 0
    for cfg in configs:
        for prog in programs:
            out = subprocess.run([pipe, prog, "--config", cfg, "--stats", "-"],
                                 capture_output=True, text=True, check=False)
            stats = json.loads(out.stdout)
            runs += 1
            stack = stats["cpi_stack_cycles"]
            br = stats["branch_cycles"]
            ok = (sum(stack.values()) == stats["cycles"]
                  and br["mispredict"] + br["id_redirect"] + br["fence_i"] == stack["branch"]
                  and stats["halt"] == "tohost" and out.returncode == 0)
            if not ok:
                bad += 1
                print(f"FAIL {os.path.basename(cfg)} {os.path.basename(prog)}: "
                      f"cycles={stats['cycles']} stack={stack} branch={br} halt={stats['halt']}")
    print(f"cpi-sum: {runs - bad} of {runs} runs ({len(configs)} configs x {len(programs)} programs) "
          f"have CPI stack == total cycles and pass")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

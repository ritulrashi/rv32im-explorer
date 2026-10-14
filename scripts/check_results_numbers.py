#!/usr/bin/env python3
"""Rule 1 guard: every number quoted in a hand-written results document must
appear verbatim in a generated results file.

    scripts/check_results_numbers.py RESULTS.md GENERATED_FILE [GENERATED_FILE ...]

Checked tokens are the ones a reader would take as measurements:
  * integers with thousands separators   (1,181,090)
  * decimals                             (1.714, 0.29, 15.4)
Plain small integers (N=64, 8x8 tiles, "3 cycles") are parameters, not
measurements, and are not checked. A decimal in RESULTS.md may also be quoted
with fewer digits than the generated file only if it is a prefix of a
generated value with the same integer part (e.g. "1.71" from "1.714") - no
rounding is allowed, only truncation that keeps the printed digits exact.
"""
import re
import sys

# The lookbehind also skips section references such as "§5.2".
TOKEN = re.compile(r"(?<![\w.§])(\d{1,3}(?:,\d{3})+|\d+\.\d+)(?![\w])")


def main():
    doc, gens = sys.argv[1], sys.argv[2:]
    with open(doc, encoding="utf-8") as f:
        text = f.read()
    # Links like [contract §6.4](docs/...) and "(§5.1, §5.2)" are references,
    # not measurements: drop every "§n.n" before scanning.
    text = re.sub(r"§\d+(?:\.\d+)*", "", text)
    # Section numbers in Markdown headings ("### 5.2 The fix") likewise.
    text = re.sub(r"(?m)^(#+\s+)\d+(?:\.\d+)*", r"\1", text)
    corpus = ""
    for g in gens:
        with open(g, encoding="utf-8") as f:
            corpus += f.read() + "\n"
    generated = set(TOKEN.findall(corpus))
    missing = []
    for tok in sorted(set(TOKEN.findall(text))):
        if tok in generated:
            continue
        if "." in tok and any(g.startswith(tok) and "." in g for g in generated):
            continue
        missing.append(tok)
    checked = len(set(TOKEN.findall(text)))
    if missing:
        print(f"check-results-numbers: {len(missing)} of {checked} numbers in {doc} not found in generated files:")
        for m in missing:
            print("  " + m)
        return 1
    print(f"check-results-numbers: all {checked} numbers in {doc} appear in the generated results")
    return 0


if __name__ == "__main__":
    sys.exit(main())

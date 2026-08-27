#!/usr/bin/env python3

"""Generate the deterministic straight-line/branch dense-AE fixture source.

Write the result to stdout and compile it at -O0.  Keeping generation separate
from timing makes the input auditable while avoiding checked-in target-specific
LLVM bitcode.
"""

import argparse


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--blocks", type=int, required=True)
    options = parser.parse_args()
    if options.blocks <= 0:
        parser.error("blocks must be positive")

    print("int main(int argc, char **argv) {")
    print("  int x = argc;")
    print("  int y = argv != 0;")
    print("  int limit = 97;")
    for _ in range(options.blocks):
        print("  x = x + y + 3;")
        print("  if (x > limit) x -= limit;")
        print("  y += 1;")
        print("  if (y > 31) y -= 17;")
    print("  return x + y;")
    print("}")


if __name__ == "__main__":
    main()

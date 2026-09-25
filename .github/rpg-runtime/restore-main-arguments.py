#!/usr/bin/env python3
"""Restore argv marshaling in Lutro's pinned Emscripten output."""

from pathlib import Path
import sys


path = Path(sys.argv[1])
script = path.read_text()
old = "function callMain(){var entryFunction=_main;var argc=0;var argv=0;"
new = (
    "function callMain(args=[]){var entryFunction=_main;"
    "args.unshift(thisProgram);var argc=args.length;"
    "var argv=stackAlloc((argc+1)*4);var argv_ptr=argv;"
    "args.forEach(arg=>{HEAPU32[argv_ptr>>2]=stringToUTF8OnStack(arg);"
    "argv_ptr+=4});HEAPU32[argv_ptr>>2]=0;"
)
if script.count(old) != 1:
    raise SystemExit("unexpected Emscripten callMain output")
if "var stringToUTF8OnStack=" not in script or "var stackAlloc=" not in script:
    raise SystemExit("Emscripten argument helpers are unavailable")
path.write_text(script.replace(old, new, 1))

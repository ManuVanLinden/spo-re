"""sgrecomp -- the x86-32 -> C translator (Capstone in, C against spore_guest.h out).

Modules:
  image.py  byte access by guest address (PE file or a flat fixture blob), imports, thunks
  cfg.py    per-function control-flow graph by recursive descent (mirrors tools/manifest/build.py's rules)
  lift.py   instruction semantics: one x86 instruction -> C statements
  emit.py   one function -> one SG_FUNC, with calls, returns, switches, tail calls and landings
"""

#!/bin/bash
# FRANK DOS: syntax-check FreeDOS port sources with the firmware's flags
# (main.c's, from build/compile_commands.json). Usage: fdos_try.sh FILE...
cd "$(dirname "$0")/.."
GCC=$(python3 -c "
import json,shlex
cc=json.load(open('build/compile_commands.json'))
print(shlex.split([x for x in cc if x['file'].endswith('src/main.c')][0]['command'])[0])")
GXX=${GCC%gcc}g++
BASE=$(python3 -c "
import json,shlex
cc=json.load(open('build/compile_commands.json'))
e=[x for x in cc if x['file'].endswith('src/main.c')][0]
a=shlex.split(e['command'])[1:]
out=[]; skip=False
for x in a:
    if skip: skip=False; continue
    if x in ('-o','-c'): skip=True; continue
    if x.endswith('main.c'): continue
    out.append(x)
print(' '.join(shlex.quote(x) for x in out))")
DEFS="-DWITHFAT32=1 -DWITHLFNAPI=1 -DQUIET=1 -DI386_MODE=1 -DEMU_CPU_GEN=3 -DSD_DATA_DIR=\\\"dos\\\" -DSD_DATA_DIR_SLASH=\\\"dos/\\\""
INC="$DEFS -Isrc/fdos/compat -Isrc -Isrc/fdos -Isrc/fdos/hdr"
for f in "$@"; do
  case "$f" in
    *.cpp|*/fcom.c) CC=$GXX; X="-x c++ -std=gnu++17 -fno-exceptions -fno-rtti" ;;
    *) CC=$GCC; X="-std=gnu11" ;;
  esac
  eval "$CC" $BASE $X $INC -fsyntax-only -Wno-unused "$f" 2>&1 | grep -E "error" | head -${N:-15}
done

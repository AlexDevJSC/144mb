#!/bin/sh
# Real Linux ELF link measurement: identical toolchain and source list,
# dynamic inputs; the only code change is a compile-time feature flag.
# NEITHER is a Windows PE or the final game's 1.44MB compliance test.
set -eu
CC=${CC:-cc}
mkdir -p build
FLAGS="-std=c11 -Os -flto -ffunction-sections -fdata-sections -fno-ident -Iinclude -Icontract"
CORE="src/ear_music.c src/ear_synth.c src/ear_bones_bridge.c"
EXTRA="src/ear_surface.c src/ear_bones_contacts.c src/surface_materials.c"
$CC $FLAGS -Wl,--gc-sections tests/benchmark_delta.c $CORE $EXTRA -lm -o build/bench_base
$CC $FLAGS -DWITH_SURFACE=1 -Wl,--gc-sections tests/benchmark_delta.c $CORE $EXTRA -lm -o build/bench_surface
base=$(wc -c < build/bench_base | tr -d ' ')
surface=$(wc -c < build/bench_surface | tr -d ' ')
delta=$((surface - base))
printf 'Baseline Ear ELF: %s bytes\nWith contact+material Ear ELF: %s bytes\nIncremental linked ELF: %+d bytes\n' "$base" "$surface" "$delta"
printf 'This is a Linux test benchmark, NOT a final Windows EXE measurement.\n'
codebase=$(size build/bench_base | awk 'NR==2 {print $2+$3}')
codesurface=$(size build/bench_surface | awk 'NR==2 {print $2+$3}')
printf 'ELF linked data+bss delta: %+d bytes (this excludes code text)\n' "$((codesurface - codebase))"
size build/bench_base build/bench_surface

$CC -std=c11 -Os -Iinclude -Icontract -c src/ear_surface.c -o build/ear_surface.o
$CC -std=c11 -Os -Iinclude -Icontract -c src/ear_bones_contacts.c -o build/ear_bones_contacts.o
$CC -std=c11 -Os -Iinclude -Icontract -c src/surface_materials.c -o build/surface_materials.o
printf 'Standalone ELF object sections (not linked distribution bytes):\n'
size build/ear_surface.o build/ear_bones_contacts.o build/surface_materials.o || :

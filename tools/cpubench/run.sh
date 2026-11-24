#!/bin/bash
# cpubench: the i386 core on its own (tools/cpubench/bench.c), on the host and
# on a Cortex-M33 under qemu (mps2-an505, -icount: 1 ns = 1 M33 instruction).
# frank-react's tools/cpubench, for murm386's src/.
#
#   run.sh bench               host: the working core and the reference, side by side
#   run.sh arm [ref]           the guest benchmarks on the M33
#   run.sh fuzz [N] [SEEDS]    working core against the reference, N cases per seed
#   run.sh case SEED I         one fuzz case, full state from both cores
#   run.sh armfuzz SEED N [fuzzk]   the M33 build's cases against the host reference's
#
# FZMODE (fuzz): 0 flat 32-bit with paging, 1 paging off, 2 DS/ES/SS on one base
# other than 0, 3 real mode (16-bit code, segments sel << 4).
# The reference core is src/ as committed at $CPUBENCH_REF (default HEAD), so
# a working-tree change is always checked against it.
set -e
cd "$(dirname "$0")/../.."
ROOT=$PWD
OUT=$ROOT/build/cpubench
REF=${CPUBENCH_REF:-HEAD}
FZMODE=${FZMODE:-0}
export FZMODE
mkdir -p "$OUT/guest" "$OUT/ref"
CFLAGS_CORE="${CFLAGS_CORE:--O2 -w}"
CORE_FILES="i386.c i386.h fpu.c fpu.h fastloop.inc fastloop_m33.S i386ins.def simd.inc remote_mem.h codeprofile.h bbprofile.h cpu_port.h exec1.inc exec1_cold.h jit16.inc jit16.h"

# the reference core, from git
REFSHA=$(git rev-parse "$REF")
if [ ! -f "$OUT/ref/.sha" ] || [ "$(cat "$OUT/ref/.sha")" != "$REFSHA" ]; then
	rm -rf "$OUT/ref/src"
	mkdir -p "$OUT/ref/src"
	for f in $CORE_FILES; do git show "$REF:src/$f" > "$OUT/ref/src/$f" 2>/dev/null || true; done
	# (a host build of a reference older than cpu_port.h's host branch)
	cp src/cpu_port.h "$OUT/ref/src/cpu_port.h"
	echo "$REFSHA" > "$OUT/ref/.sha"
fi
# (bench.c's jit_note_write: no-ops without JIT16, for a reference older than it)
[ -s "$OUT/ref/src/jit16.h" ] || cp src/jit16.h "$OUT/ref/src/jit16.h"

guests16() {
	mkdir -p "$OUT/guest16"
	# (C guests: 32-bit code under 66h/67h in real mode, -m16; their *_start.S is the entry)
	for f in tools/cpubench/guest16/*.c; do
		n=$(basename "$f" .c)
		i686-elf-gcc -m16 -O2 -march=i386 -ffast-math -ffreestanding -nostdlib -fno-pic -fno-stack-protector \
			-fno-asynchronous-unwind-tables -fno-builtin -static -Wl,--no-warn-rwx-segments \
			-T tools/cpubench/guest16/link16c.ld -o "$OUT/guest16/$n.elf" \
			tools/cpubench/guest16/${n}_start.S "$f" -lgcc
		i686-elf-objcopy -O binary "$OUT/guest16/$n.elf" "$OUT/guest16/$n.bin"
	done
	for f in tools/cpubench/guest16/*.S; do
		case "$f" in *_start.S) continue;; esac
		n=$(basename "$f" .S)
		i686-elf-as --32 -o "$OUT/guest16/$n.o" "$f"
		i686-elf-ld -m elf_i386 -T tools/cpubench/guest16/link16.ld --oformat binary -o "$OUT/guest16/$n.bin" "$OUT/guest16/$n.o"
	done
}

guests() {
	for f in tools/cpubench/guest/*.c; do
		n=$(basename "$f" .c)
		if [ ! -f "$OUT/guest/$n.bin" ] || [ "$f" -nt "$OUT/guest/$n.bin" ] || [ tools/cpubench/guest/rt.h -nt "$OUT/guest/$n.bin" ]; then
			i686-elf-gcc -O2 -march=pentium -ffreestanding -nostdlib -fno-pic -fno-stack-protector \
				-fno-asynchronous-unwind-tables -fno-builtin -static -Wl,--no-warn-rwx-segments -T tools/cpubench/guest/link.ld \
				-o "$OUT/guest/$n.elf" "$f" -lgcc
			i686-elf-objcopy -O binary "$OUT/guest/$n.elf" "$OUT/guest/$n.bin"
		fi
	done
}

host() {    # host NAME SRCDIR [CFLAGS...]
	local name=$1 src=$2; shift 2
	cc $CFLAGS_CORE "$@" -I"$src" -c "$src/i386.c" -o "$OUT/$name-i386.o"
	cc $CFLAGS_CORE "$@" -I"$src" -c "$src/fpu.c" -o "$OUT/$name-fpu.o"
	cc -O2 "$@" -I"$src" -o "$OUT/x-$name" tools/cpubench/bench.c "$OUT/$name-i386.o" "$OUT/$name-fpu.o" -lm
}

case "${1:-bench}" in
bench)
	guests
	host cur src
	host ref "$OUT/ref/src"
	for i in 1 2 3; do
		echo "== ref"; "$OUT/x-ref" bench "$OUT"/guest/*.bin
		echo "== cur"; "$OUT/x-cur" bench "$OUT"/guest/*.bin
	done
	;;
fuzz)
	host cur src $CUR_FLAGS
	host ref "$OUT/ref/src" $REF_FLAGS
	N=${2:-20000}
	SEEDS=${3:-"1 2 3 4"}
	fail=0
	for s in $SEEDS; do
		"$OUT/x-ref" fuzz "$s" "$N" > "$OUT/fuzz-ref-$s.txt" &
		"$OUT/x-cur" fuzz "$s" "$N" > "$OUT/fuzz-cur-$s.txt" &
		wait
		d=$(diff "$OUT/fuzz-ref-$s.txt" "$OUT/fuzz-cur-$s.txt" | grep -c '^>' || true)
		echo "mode $FZMODE seed $s: $N cases, $d differ"
		if [ "$d" != 0 ]; then
			fail=1
			diff "$OUT/fuzz-ref-$s.txt" "$OUT/fuzz-cur-$s.txt" | grep '^>' | head -5
		fi
	done
	exit $fail
	;;
case)
	host cur src $CUR_FLAGS
	host ref "$OUT/ref/src" $REF_FLAGS
	"$OUT/x-ref" fuzzone "$2" "$3" vv
	"$OUT/x-cur" fuzzone "$2" "$3" vv
	;;
bench16)
	guests16
	host cur src
	host ref "$OUT/ref/src"
	echo "== ref"; "$OUT/x-ref" bench16 "$OUT"/guest16/*.bin
	echo "== cur"; "$OUT/x-cur" bench16 "$OUT"/guest16/*.bin
	;;
arm|armfuzz|arm16)
	guests
	GDIR=guest
	if [ "$1" = arm16 ]; then
		guests16
		GDIR=guest16
		ARM_FLAGS="$ARM_FLAGS -DARM_BENCH16"
	fi
	SRC=src
	{ [ "$1" = arm ] || [ "$1" = arm16 ]; } && [ "$2" = ref ] && SRC="$OUT/ref/src"
	EXTRA=
	if [ "$1" = armfuzz ]; then
		SEED=${2:-1}; N=${3:-3000}; MODE=${4:-fuzz}
		EXTRA="-DARM_FUZZ=$SEED -DARM_FUZZ_N=$N -DFZMODE_FIXED=$FZMODE -DFZJIT_FIXED=${FZJIT:-0}"
		[ "$MODE" = fuzzk ] && EXTRA="$EXTRA -DARM_FUZZ_K"
		host ref "$OUT/ref/src" $REF_FLAGS
		"$OUT/x-ref" $MODE "$SEED" "$N" > "$OUT/armfuzz-ref.txt"
	fi
	A="arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp"
	{
		echo '#include <stddef.h>'
		names=""
		for f in "$OUT"/$GDIR/*.bin; do
			n=$(basename "$f" .bin)
			[ -n "$ONLY" ] && [ "$n" != "$ONLY" ] && continue   # (ONLY=name: that image alone)
			echo "__asm__(\".section .rodata\\n.balign 4\\n.global img_$n\\nimg_$n:\\n.incbin \\\"$f\\\"\\n.global img_${n}_end\\nimg_${n}_end:\\n.text\");"
			echo "extern const unsigned char img_$n[], img_${n}_end[];"
			names="$names $n"
		done
		echo 'const char *const arm_bench_names[] = {'
		for n in $names; do echo "\"$n\","; done
		echo 'NULL};'
		echo 'const unsigned char *arm_image(const char *name, unsigned *size) {'
		for n in $names; do echo "if (!__builtin_strcmp(name, \"$n\")) { *size = img_${n}_end - img_$n; return img_$n; }"; done
		echo 'return 0; }'
	} > "$OUT/arm_images.c"
	$A $CFLAGS_CORE $ARM_FLAGS -g -I"$SRC" -c "$SRC/i386.c" -o "$OUT/arm-i386.o"
	$A $CFLAGS_CORE -I"$SRC" -c "$SRC/fpu.c" -o "$OUT/arm-fpu.o"
	$A -O2 -DCPUBENCH_ARM $EXTRA $ARM_FLAGS -I"$SRC" --specs=nano.specs -u _printf_float -nostartfiles -T tools/cpubench/arm/an505.ld \
		-o "$OUT/arm.elf" tools/cpubench/bench.c tools/cpubench/arm/arm_rt.c "$OUT/arm_images.c" \
		"$OUT/arm-i386.o" "$OUT/arm-fpu.o" $ARM_OBJS -lm
	arm-none-eabi-size "$OUT/arm.elf"
	timeout 3000 qemu-system-arm -M mps2-an505 -nographic -icount shift=0 \
		-semihosting-config enable=on,target=native -kernel "$OUT/arm.elf" > "$OUT/arm-out.txt"
	if [ "$1" = armfuzz ]; then
		tr -d '\r' < "$OUT/arm-out.txt" | grep -E '^[0-9]+ [0-9a-f]{8}$' > "$OUT/armfuzz-cur.txt"
		echo "M33 mode $FZMODE: $(wc -l < "$OUT/armfuzz-cur.txt") cases, $(diff "$OUT/armfuzz-ref.txt" "$OUT/armfuzz-cur.txt" | grep -c '^>' || true) differ"
		exit 0
	fi
	grep -v '^S ' "$OUT/arm-out.txt" || true
	;;
*)
	echo "usage: $0 bench|arm [ref]|fuzz [N] [SEEDS]|case SEED I|armfuzz SEED N [fuzzk]" >&2
	exit 2
	;;
esac

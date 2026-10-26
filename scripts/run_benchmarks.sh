#!/usr/bin/env bash
# Reruns every benchmark the README quotes, one after another, and saves
# the raw output and the CSVs in results/runs/<date>/ next to a note of
# the machine, compiler and commit it came from.
#
# Timings on a laptop move 10-20% between runs, so compare numbers from
# the same run, and close other programs first.
#
# usage: scripts/run_benchmarks.sh [fsync-dir]
#        ONLY=<regex> scripts/run_benchmarks.sh   runs only the steps whose
#                                                 names match, e.g. ONLY=engine
#
# fsync-dir is where the durability benchmarks put their files, and it
# has to be on a real disk. fsync does nothing on a tmpfs (WSL's /tmp is
# one), and on WSL, files under /mnt/c go through a translation layer
# that makes every fsync far slower than the disk itself. The default is
# a directory under $HOME.
set -euo pipefail
cd "$(dirname "$0")/.."
repo=$(pwd)
out=results/runs/$(date +%Y-%m-%d_%H%M)
fsync_dir=${1:-$HOME/learned_index_fsync}
mkdir -p "$out" "$fsync_dir/results"
# the benchmarks write their CSVs next to this run's text output
export LIDB_RESULTS_DIR="$repo/$out"

{
  echo "date:      $(date -Is)"
  echo "commit:    $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- cpp Makefile || echo ' plus uncommitted changes')"
  echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//') ($(nproc) threads)"
  echo "memory:    $(free -h | awk '/^Mem:/ {print $2}')"
  echo "kernel:    $(uname -sr)"
  echo "compiler:  $(${CXX:-g++} --version | head -1), -O2 -std=c++17"
  echo "load:      $(cut -d' ' -f1-3 /proc/loadavg) (1, 5 and 15 minute averages, at the start)"
  echo "fsync dir: $fsync_dir ($(df -T "$fsync_dir" | awk 'NR == 2 {print $2}'))"
} | tee "$out/machine.txt"

make -s all
[ -f data/uniform_1000000.bin ] || make data

step() {
  local name=$1
  shift
  [[ $name =~ ${ONLY:-.} ]] || return 0
  echo
  echo "== $name"
  "$@" 2>&1 | tee "$out/$name.txt"
}

step lookups_uniform ./build/benchmark data/uniform_1000000.bin 200000
step lookups_skewed ./build/benchmark data/skewed_1000000.bin 200000
step size_sweep ./build/full_benchmark
step pgm_comparison ./build/pgm_comparison
# at 10M keys the data no longer fits in cache, which changes the picture
[ -f data/uniform_10000000.bin ] || python3 python/data_gen.py --n 10000000 --outdir data
step db_lookups_uniform ./build/benchmark_db_lookups data/uniform_1000000.bin 7
step db_lookups_skewed ./build/benchmark_db_lookups data/skewed_1000000.bin 7
step db_lookups_uniform_10m ./build/benchmark_db_lookups data/uniform_10000000.bin 7
step db_lookups_skewed_10m ./build/benchmark_db_lookups data/skewed_10000000.bin 7
# The whole database, through SQL, on each index. The two are separate
# builds, so instead of interleaving within one run they take turns, three
# runs each, and summarize_engine.py takes the medians. The log goes to
# /dev/shm (a tmpfs) for everything but the durable-write test.
make -s build/btree/engine_benchmark
for data in uniform_1000000 skewed_1000000 uniform_10000000; do
  for round in 1 2 3; do
    step "engine_${data}_learned_$round" ./build/engine_benchmark "data/$data.bin" /dev/shm "$fsync_dir"
    step "engine_${data}_btree_$round" ./build/btree/engine_benchmark "data/$data.bin" /dev/shm "$fsync_dir"
  done
done

# Real datasets from SOSD (python/sosd_data.py downloads them, downsampled
# to 10M keys), if they're there: the storage-level benchmarks, and one
# run of the whole database on each index.
for path in data/sosd_*.bin; do
  [ -f "$path" ] || continue
  name=$(basename "$path" .bin)
  step "db_lookups_$name" ./build/benchmark_db_lookups "$path" 7
  step "inserts_$name" ./build/benchmark_inserts "$path"
  step "engine_${name}_learned_1" ./build/engine_benchmark "$path" /dev/shm "$fsync_dir"
  step "engine_${name}_btree_1" ./build/btree/engine_benchmark "$path" /dev/shm "$fsync_dir"
done
if ls "$out"/engine_*.txt > /dev/null 2>&1; then python3 scripts/summarize_engine.py "$out"; fi

step inserts_uniform ./build/benchmark_inserts data/uniform_1000000.bin
step inserts_skewed ./build/benchmark_inserts data/skewed_1000000.bin
step concentrated_inserts ./build/adversarial_test 9
step secondary_index ./build/executor_test
step bloom_filter ./build/bloom_filter_demo
step cache ./build/cache_demo
# these two write to results/ under the current directory, so run them
# from the fsync directory
step durability bash -c "cd '$fsync_dir' && '$repo/build/durability_demo'"
step checkpoint ./build/checkpoint_demo "$fsync_dir"

echo "load:      $(cut -d' ' -f1-3 /proc/loadavg) (at the end)" | tee -a "$out/machine.txt"
echo
echo "saved to $out"

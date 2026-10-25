#!/usr/bin/env bash
# Reruns every benchmark the README quotes, one after another, and saves
# the raw output in results/runs/<date>/ next to a note of the machine,
# compiler and commit it came from.
#
# Timings on a laptop move 10-20% between runs, so compare numbers from
# the same run, and close other programs first.
#
# usage: scripts/run_benchmarks.sh [fsync-dir]
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
  echo
  echo "== $name"
  "$@" 2>&1 | tee "$out/$name.txt"
}

step lookups_uniform ./build/benchmark data/uniform_1000000.bin 200000
step lookups_skewed ./build/benchmark data/skewed_1000000.bin 200000
step size_sweep ./build/full_benchmark
step pgm_comparison ./build/pgm_comparison
step db_lookups_uniform ./build/benchmark_db_lookups data/uniform_1000000.bin 7
step db_lookups_skewed ./build/benchmark_db_lookups data/skewed_1000000.bin 7
# at 10M keys the data no longer fits in cache, which changes the picture
[ -f data/uniform_10000000.bin ] || python3 python/data_gen.py --n 10000000 --outdir data
step db_lookups_uniform_10m ./build/benchmark_db_lookups data/uniform_10000000.bin 7
step db_lookups_skewed_10m ./build/benchmark_db_lookups data/skewed_10000000.bin 7
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

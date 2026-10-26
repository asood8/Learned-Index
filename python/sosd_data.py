"""Downloads real-world datasets from the SOSD learned-index benchmark
(Marcus et al., "Benchmarking Learned Indexes", PVLDB 2020) and
downsamples each one to a size this project's benchmarks can handle.

usage: python3 python/sosd_data.py [--n 10000000] [--outdir data] [names...]

The datasets are 200M or 800M sorted keys each. Each file is streamed,
decompressed (zstd: Python's standard library has it from 3.14; on older
versions the download goes through the curl and zstd tools), checked
against the MD5 that SOSD's own download script lists, and downsampled
on the way in by keeping every k-th key, the way SOSD shrinks its 800M
datasets to 200M. Nothing bigger than the output ever sits on disk.

The output is in this project's format: sorted, unique, little-endian
int64, no header. Two adjustments, both printed:
- duplicate keys are dropped (wiki_ts has many repeated timestamps, and
  every structure here needs unique keys);
- if any key is too big for an int64 below INT64_MAX (the gapped array
  uses INT64_MAX to mark an empty slot), every key is shifted right one
  bit, which keeps their order and the shape of the distribution.
"""

import argparse
import contextlib
import hashlib
import os
import subprocess
import sys
import urllib.request

import numpy as np

try:
    from compression import zstd
except ImportError:  # before Python 3.14
    zstd = None

DATAVERSE = "https://dataverse.harvard.edu/api/access/datafile/:persistentId?persistentId=doi:10.7910/DVN/JGVF9A/"
# name: (url, md5 of the decompressed file, bytes per key), from SOSD's scripts/download.sh
DATASETS = {
    "books": (DATAVERSE + "5YTV8K", "9f3e578671e5c0348cdddc9c68946770", 4),  # books_200M_uint32
    "fb": (DATAVERSE + "EATHF7", "3b0f820caa0d62150e87ce94ec989978", 8),  # fb_200M_uint64
    "wiki": (DATAVERSE + "SVN8PI", "4f1402b1c476d67f77d2da4955432f7d", 8),  # wiki_ts_200M_uint64
    "osm": (
        "https://www.dropbox.com/s/j1d4ufn4fyb4po2/osm_cellids_800M_uint64.zst?dl=1",
        "70670bf41196b9591e07d0128a281b9a",
        8,
    ),  # osm_cellids_800M_uint64
}
CHUNK_KEYS = 1 << 22
USER_AGENT = "learned-index-db sosd_data.py (github.com/asood8/Learned-Index)"


@contextlib.contextmanager
def download(url: str):
    """Yields the decompressed contents of url as a stream."""
    if zstd is not None:
        # Harvard Dataverse refuses urllib's default User-Agent (HTTP 403)
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request) as response, zstd.ZstdFile(response) as stream:
            yield stream
        return
    curl = subprocess.Popen(["curl", "-sSfL", "-A", USER_AGENT, url], stdout=subprocess.PIPE)
    unzstd = subprocess.Popen(["zstd", "-dcq"], stdin=curl.stdout, stdout=subprocess.PIPE)
    curl.stdout.close()  # so zstd sees the end of the input when curl exits
    try:
        yield unzstd.stdout
    finally:
        unzstd.stdout.close()
        if unzstd.wait() != 0 or curl.wait() != 0:
            sys.exit(f"downloading or decompressing {url} failed")


def fetch(name: str, target: int, outdir: str) -> None:
    url, md5_expected, width = DATASETS[name]
    dtype = np.dtype("<u4") if width == 4 else np.dtype("<u8")
    md5 = hashlib.md5()
    with download(url) as stream:
        header = stream.read(8)
        md5.update(header)
        count = int.from_bytes(header, "little")
        stride = max(1, count // target)
        kept = []
        seen = 0  # keys read so far
        while True:
            buf = stream.read(CHUNK_KEYS * width)
            if not buf:
                break
            md5.update(buf)
            keys = np.frombuffer(buf, dtype=dtype)
            first = (-seen) % stride  # index in this chunk of the next key to keep
            kept.append(keys[first::stride].copy())
            seen += len(keys)
            print(f"\r  {name}: {seen:,} of {count:,} keys read", end="", file=sys.stderr)
        print(file=sys.stderr)
    if seen != count:
        sys.exit(f"{name}: expected {count:,} keys, read {seen:,}")
    if md5.hexdigest() != md5_expected:
        sys.exit(f"{name}: MD5 {md5.hexdigest()} doesn't match SOSD's {md5_expected}")

    sample = np.concatenate(kept).astype(np.uint64)
    if np.any(sample[1:] < sample[:-1]):
        sys.exit(f"{name}: keys aren't sorted")
    shifted = bool(sample.max() >= np.uint64(2**63 - 1))
    if shifted:
        sample >>= np.uint64(1)
    unique = np.unique(sample).astype(np.int64)
    path = os.path.join(outdir, f"sosd_{name}_{target}.bin")
    unique.tofile(path)
    print(
        f"{name}: {count:,} keys, kept every {stride}th: {len(sample):,}, "
        f"{len(sample) - len(unique):,} duplicates dropped, "
        f"{'shifted right 1 bit, ' if shifted else ''}wrote {len(unique):,} keys to {path} (MD5 ok)"
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=10_000_000)
    ap.add_argument("--outdir", default="data")
    ap.add_argument("names", nargs="*", default=list(DATASETS))
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    for name in args.names:
        fetch(name, args.n, args.outdir)


if __name__ == "__main__":
    main()

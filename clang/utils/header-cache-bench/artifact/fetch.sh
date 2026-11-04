#!/bin/bash
# Clones the seven projects into BENCH_ROOT at the commits the paper's
# numbers use (clang/lib/ASTCache/HISTORY.md, "Running the benchmarks").
#
#   BENCH_ROOT=<empty directory> ./fetch.sh
set -e
: "${BENCH_ROOT:?set BENCH_ROOT to a directory for the seven checkouts}"
mkdir -p "$BENCH_ROOT"
while read -r name url commit; do
  [ -d "$BENCH_ROOT/$name/.git" ] || git clone -q "$url" "$BENCH_ROOT/$name"
  git -C "$BENCH_ROOT/$name" checkout -q "$commit"
  echo "$name $(git -C "$BENCH_ROOT/$name" log -1 --format='%h %cd' --date=short)"
done <<'LIST'
redis https://github.com/redis/redis.git b1b5c851671b44da8b01390a73adf6d871cc2041
nginx https://github.com/nginx/nginx.git 5f54125dde0d144476155d8964f6ec3d449f578d
curl https://github.com/curl/curl.git 0b5759caa299e2f86abc8b140c56e16fd03cc744
git https://github.com/git/git.git 010afd3166ddc64c9863b1506f12cbcdda0d4ea1
postgres https://github.com/postgres/postgres.git 086f6f1760140a0055c2faa8d6831fd3ebaf96b0
cpython https://github.com/python/cpython.git fe3a26f43fad1d6eed20172d7f63ee2931ae2ce1
ffmpeg https://github.com/FFmpeg/FFmpeg.git f25dd4e26f64b84cdd586c16dc522c7f023dafa3
LIST

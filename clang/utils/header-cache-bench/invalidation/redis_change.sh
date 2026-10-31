#!/bin/bash
# Invalidation test (user, 2026-10-31): between a Redis build and its
# rebuild, change two headers so that a stale copy would compile different
# code, and touch a third without changing it.
#  - sds.h: SDS_MAX_PREALLOC doubles (sds.c's growth code reads it).
#  - server.h: struct redisServer gets a new first member, 24 bytes, so
#    every later member moves, in every file that reads server. (An int
#    moved only pid: the padding after it took the int.)
#  - zmalloc.h: modification time only.
# The originals go to $BACKUP first; redis_undo.sh puts them back.
set -e
S=${SRC_ROOT:-/dev/shm/bench-src}/redis/src
BACKUP=/dev/shm/bench-src-backup-redis
mkdir -p $BACKUP
for f in sds.h server.h zmalloc.h; do cp -p $S/$f $BACKUP/$f; done
sed -i 's/^#define SDS_MAX_PREALLOC (1024\*1024)$/#define SDS_MAX_PREALLOC (1024*1024*2)/' $S/sds.h
grep -q 'SDS_MAX_PREALLOC (1024\*1024\*2)' $S/sds.h
sed -i 's/^struct redisServer {$/struct redisServer {\n    char hc_invalidation_test[24];/' $S/server.h
grep -q 'hc_invalidation_test' $S/server.h
touch $S/zmalloc.h
echo "changed sds.h, server.h; touched zmalloc.h"

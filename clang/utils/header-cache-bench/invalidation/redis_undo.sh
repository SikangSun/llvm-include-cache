#!/bin/bash
# Puts back the Redis headers redis_change.sh changed, with their times.
set -e
S=${SRC_ROOT:-/dev/shm/bench-src}/redis/src
BACKUP=/dev/shm/bench-src-backup-redis
for f in sds.h server.h zmalloc.h; do cp -p $BACKUP/$f $S/$f; done
echo "restored sds.h, server.h, zmalloc.h"

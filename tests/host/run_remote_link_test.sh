#!/bin/bash
#
# Copyright (C) 2026 Xiaomi Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Builds and runs the remote-control protocol test on the development host.
#
# The link, codec, and session layers are deliberately free of NuttX and MQTT
# dependencies, so they compile and run here. The MQTT transport and the channel
# are not covered: those need a board and a bridge.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

# cJSON lives in the NuttX apps tree next to the package.
CJSON_DIR="${CJSON_DIR:-$ROOT/../../apps/netutils/cjson/cJSON}"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
  echo "cJSON source not found at $CJSON_DIR" >&2
  echo "Set CJSON_DIR to the directory holding cJSON.c" >&2
  exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# cJSON is third-party: compile it without the strict flags used for our code.
gcc -std=gnu11 -O1 -I "$CJSON_DIR" -c "$CJSON_DIR/cJSON.c" -o "$OUT/cJSON.o"

gcc -std=gnu11 -O1 -g \
  -Wall -Wextra -Werror \
  -I "$ROOT/src" -I "$CJSON_DIR" \
  "$ROOT/src/remote/remote_link.c" \
  "$ROOT/src/remote/remote_codec.c" \
  "$ROOT/src/remote/remote_session.c" \
  "$HERE/test_remote_link.c" \
  "$OUT/cJSON.o" \
  -lpthread \
  -o "$OUT/test_remote_link"

"$OUT/test_remote_link"

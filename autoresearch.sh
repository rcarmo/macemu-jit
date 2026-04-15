#!/bin/bash
set -euo pipefail

# Autoresearch runner compatibility wrapper.
# Canonical harness lives at jit-test/run.sh.
exec "$(cd "$(dirname "$0")" && pwd)/jit-test/run.sh"

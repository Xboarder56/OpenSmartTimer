#!/usr/bin/env bash
# Isolated HTTPS stack gate. Never uses relay/.env or an existing Compose project.
set -euo pipefail
RELAY_TEST_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RELAY_TEST_PYTHON="${PYTHON:-$RELAY_TEST_ROOT/.venv/bin/python}"
if [[ ! -x "$RELAY_TEST_PYTHON" ]]; then
  echo "Create relay/.venv and install relay dependencies first; see relay/README.md." >&2
  exit 2
fi
exec "$RELAY_TEST_PYTHON" "$RELAY_TEST_ROOT/tests/stack_check.py" "$@"

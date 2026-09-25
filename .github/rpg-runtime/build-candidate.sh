#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
output=${1:?absolute empty output directory is required}
"$root/.github/rpg-runtime/build-web.sh" "$output"
python3 "$root/.github/rpg-runtime/candidate_descriptor.py" finalize "$output" --core-id lutro

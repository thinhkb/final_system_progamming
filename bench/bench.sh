#!/usr/bin/env bash
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-18080}"
PATH_TO_FETCH="${3:-/index.html}"
CLIENTS="${4:-120}"
URL="http://${HOST}:${PORT}${PATH_TO_FETCH}"
TMP_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

start_ns=$(date +%s%N)
for i in $(seq 1 "${CLIENTS}"); do
  (
    if curl -fsS --max-time 5 "${URL}" >/dev/null; then
      echo ok > "${TMP_DIR}/${i}.result"
    else
      echo fail > "${TMP_DIR}/${i}.result"
    fi
  ) &
done
wait
end_ns=$(date +%s%N)

successes=$(grep -l '^ok$' "${TMP_DIR}"/*.result | wc -l | tr -d ' ')
failures=$((CLIENTS - successes))
elapsed_ms=$(((end_ns - start_ns) / 1000000))
if [[ "${elapsed_ms}" -le 0 ]]; then
  elapsed_ms=1
fi
requests_per_second=$((CLIENTS * 1000 / elapsed_ms))

printf 'benchmark clients=%s successes=%s failures=%s elapsed_ms=%s requests_per_second=%s\n' \
  "${CLIENTS}" "${successes}" "${failures}" "${elapsed_ms}" "${requests_per_second}"

[[ "${failures}" -eq 0 ]]

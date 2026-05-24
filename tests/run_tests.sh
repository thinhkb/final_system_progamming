#!/usr/bin/env bash
set -euo pipefail

PORT=18080
HOST=127.0.0.1
BASE_URL="http://${HOST}:${PORT}"
SERVER_PID=""
cleanup() {
  if [[ -n "${SERVER_PID}" ]]; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

make
./httpd -p "${PORT}" -r www -t 4 -q 16 -l access.log > /tmp/httpd-test.log 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
  if curl -fsS "${BASE_URL}/index.html" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

curl -fsS "${BASE_URL}/index.html" | grep -q "System Programming HTTP Server"
curl -fsSI "${BASE_URL}/about.txt" | grep -q "^HTTP/1.1 200"
[[ "$(curl -fsSI "${BASE_URL}/about.txt" | tr -d '\r' | tail -n 1)" == "" ]]
status=$(curl -sSI "${BASE_URL}/missing.txt" | tr -d '\r' | head -n 1)
[[ "${status}" == "HTTP/1.1 404 Not Found" ]]
status=$(curl --path-as-is -sSI "${BASE_URL}/../Makefile" | tr -d '\r' | head -n 1)
[[ "${status}" == "HTTP/1.1 403 Forbidden" ]]
curl -fsSI "${BASE_URL}/assets/style.css" | grep -qi "^Content-Type: text/css"

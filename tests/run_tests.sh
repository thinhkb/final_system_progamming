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
./httpd -p "${PORT}" -r www -t 8 -q 128 -l access.log > /tmp/httpd-test.log 2>&1 &
SERVER_PID=$!

server_ready=0
for _ in $(seq 1 50); do
  if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    cat /tmp/httpd-test.log >&2
    exit 1
  fi
  if curl -fsS "${BASE_URL}/index.html" >/dev/null 2>&1; then
    server_ready=1
    break
  fi
  sleep 0.1
done
if [[ "${server_ready}" -ne 1 ]]; then
  cat /tmp/httpd-test.log >&2
  exit 1
fi

curl -fsS "${BASE_URL}/index.html" | grep -q "System Programming HTTP Server"
curl -fsS "${BASE_URL}/index.html?cache=false" | grep -q "System Programming HTTP Server"
curl -fsSI "${BASE_URL}/about.txt" | grep -q "^HTTP/1.1 200"
[[ "$(curl -fsSI "${BASE_URL}/about.txt" | tr -d '\r' | tail -n 1)" == "" ]]
status=$(curl -sSI "${BASE_URL}/missing.txt" | tr -d '\r' | head -n 1)
[[ "${status}" == "HTTP/1.1 404 Not Found" ]]
status=$(curl --path-as-is -sSI "${BASE_URL}/../Makefile" | tr -d '\r' | head -n 1)
[[ "${status}" == "HTTP/1.1 403 Forbidden" ]]
curl -fsSI "${BASE_URL}/assets/style.css" | grep -qi "^Content-Type: text/css"
curl -fsS "${BASE_URL}/listing/" | grep -q "a.txt"
curl -fsS "${BASE_URL}/listing/" | grep -q "b.txt"
curl -fsS "${BASE_URL}/listing/" | grep -q 'href="space%20name%20%231.txt"'
curl -fsS "${BASE_URL}/listing/space%20name%20%231.txt" | grep -q "URL encoding"
curl -fsSI "${BASE_URL}/listing/" | grep -qi "^Content-Type: text/html"
status=$(curl -sSI -X POST "${BASE_URL}/index.html" | tr -d '\r' | head -n 1)
[[ "${status}" == "HTTP/1.1 501 Not Implemented" ]]
status=$(curl -sSI -r 0-5 "${BASE_URL}/about.txt" | tr -d '\r' | head -n 1)
[[ "${status}" == "HTTP/1.1 206 Partial Content" ]]
partial_body=$(curl -fsS -r 0-5 "${BASE_URL}/about.txt")
[[ "${partial_body}" == "static" ]]
status=$(curl -sSI -r 9999-10000 "${BASE_URL}/about.txt" | tr -d '\r' | head -n 1)
[[ "${status}" == "HTTP/1.1 416 Range Not Satisfiable" ]]

keep_alive_response=$(
  {
    printf 'GET /about.txt HTTP/1.1\r\nHost: localhost\r\n\r\n'
    printf 'GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'
  } | nc "${HOST}" "${PORT}"
)
[[ "$(grep -c "HTTP/1.1 200 OK" <<< "${keep_alive_response}")" -eq 2 ]]

grep -q '"GET /index.html HTTP/1.1" 200' access.log
bash ./bench/bench.sh "${HOST}" "${PORT}" /index.html 120

#!/usr/bin/env bash
set -euo pipefail

port="${SENTUM_DASHBOARD_SMOKE_PORT:-18080}"
log_file="${SENTUM_DASHBOARD_SMOKE_LOG:-/tmp/sentum-dashboard.log}"
pid=""

print_diagnostics() {
    echo "--- dashboard smoke diagnostics ---" >&2
    if [[ -n "${pid}" ]]; then
        ps -o pid,ppid,stat,etime,wchan:32,comm -p "${pid}" >&2 || true
        ps -L -o pid,tid,stat,etime,wchan:32,comm -p "${pid}" >&2 || true
        if [[ -r "/proc/${pid}/status" ]]; then
            cat "/proc/${pid}/status" >&2 || true
        fi
    fi
    if [[ -f "${log_file}" ]]; then
        echo "--- dashboard log ---" >&2
        tail -n 200 "${log_file}" >&2 || true
    fi
}

cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -KILL "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rm -f "${log_file}"
./sentum dashboard --dashboard-port "${port}" >"${log_file}" 2>&1 &
pid=$!

ready=0
for _ in {1..30}; do
    if ! kill -0 "${pid}" 2>/dev/null; then
        echo "dashboard process exited before readiness" >&2
        print_diagnostics
        wait "${pid}" || true
        exit 1
    fi
    if curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
        "http://127.0.0.1:${port}/api/health" >/tmp/health.json; then
        ready=1
        break
    fi
    sleep 0.2
done

if [[ "${ready}" -ne 1 ]]; then
    echo "dashboard readiness deadline exceeded" >&2
    print_diagnostics
    exit 1
fi

curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
    "http://127.0.0.1:${port}/api/health" | grep '"read_only":true'
curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
    "http://127.0.0.1:${port}/api/status" | grep "\"dashboard_port\":${port}"
curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
    "http://127.0.0.1:${port}/api/research" | grep '"objective":"sharpe"'
curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
    "http://127.0.0.1:${port}/" | grep 'SENTUM'

kill -TERM "${pid}"
for _ in {1..20}; do
    if ! kill -0 "${pid}" 2>/dev/null; then
        set +e
        wait "${pid}"
        status=$?
        set -e
        if [[ "${status}" -eq 0 ]]; then
            pid=""
            trap - EXIT
            exit 0
        fi
        echo "dashboard exited with status ${status}" >&2
        print_diagnostics
        exit "${status}"
    fi
    sleep 0.1
done

echo "dashboard did not stop within the two-second lifecycle budget" >&2
print_diagnostics
exit 1

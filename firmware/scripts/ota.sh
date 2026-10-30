#!/usr/bin/env bash
# Install firmware on the eyeball over Wi-Fi, then wait for it to restart and report what it runs.
#
#   scripts/ota.sh [host]              build, then upload .pio/build/eyeball/firmware.bin
#   scripts/ota.sh --file BIN [host]   upload an already built image (used by `pio run -e ota -t upload`)
#
# The host defaults to $EYE_URL (as used by the tracker) or eyeball.local.
set -euo pipefail
cd "$(dirname "$0")/.."

bin=""
if [[ ${1:-} == --file ]]; then
    bin=$2
    shift 2
fi
host=${1:-${EYE_URL:-eyeball.local}}
host=${host#http://}
host=${host%%/*}
base="http://$host"
# IPv4 only: macOS stalls ~5 s on the mDNS IPv6 lookup for the ESP32, which has no IPv6.
curl_eye() { curl -4 --silent --show-error "$@"; }
status_field() { python3 -c "import json,sys; print(json.load(sys.stdin)['$1'])"; }

if [[ -z $bin ]]; then
    uvx --python 3.12 platformio run -e eyeball
    bin=.pio/build/eyeball/firmware.bin
fi

before=$(curl_eye --max-time 5 "$base/api/status") || {
    echo "Can't reach $base. Is the eye on, and on Wi-Fi (green LED)?" >&2
    exit 1
}
echo "Running $(status_field version <<<"$before") (built $(status_field built <<<"$before")); uploading $bin to $host"

response=$(curl_eye --progress-bar --max-time 300 -F "firmware=@$bin" "$base/api/update") || true
if ! grep -q '"ok":true' <<<"$response"; then
    echo "Update failed: ${response:-no response}" >&2
    exit 1
fi

echo -n "Installed; waiting for it to restart"
for _ in $(seq 60); do
    sleep 1.5
    echo -n .
    now=$(curl_eye --max-time 3 "$base/api/status" 2>/dev/null) || continue
    [[ $(status_field boot <<<"$now") == $(status_field boot <<<"$before") ]] && continue
    echo
    if [[ $(status_field built <<<"$now") == $(status_field built <<<"$before") ]]; then
        echo "It restarted but is still running the old firmware: the update was rolled back. See the log at $base" >&2
        exit 1
    fi
    echo "Now running $(status_field version <<<"$now") (built $(status_field built <<<"$now"))"
    exit 0
done
echo
echo "It hasn't come back after 90 s. Check its LED, and the serial log over USB." >&2
exit 1

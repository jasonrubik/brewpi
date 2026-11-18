#!/bin/bash
# arduino-deploy.sh
# Robust headless deploy: detect port, board, baud; free port; compile; upload; optional monitor.
# Usage: ./arduino-deploy.sh /path/to/sketchdir [monitor_timeout_seconds]
set -euo pipefail

SKETCH_DIR="${1:-}"
MONITOR_TIMEOUT="${2:-0}"   # seconds; 0 = no monitor
LOG_DIR="${HOME}/arduino-logs"
mkdir -p "${LOG_DIR}"

timestamp() { date +"%Y-%m-%d_%H%M%S"; }
logfile="${LOG_DIR}/deploy_$(timestamp).log"

echo "Starting deploy: $(date)" | tee -a "$logfile"
if [ -z "$SKETCH_DIR" ] || [ ! -d "$SKETCH_DIR" ]; then
  echo "Usage: $0 /path/to/sketchdir [monitor_timeout_seconds]" | tee -a "$logfile"
  exit 1
fi

# --- find .ino file
INO_FILE=$(find "$SKETCH_DIR" -maxdepth 1 -type f -name '*.ino' | head -n 1 || true)
if [ -z "$INO_FILE" ]; then
  echo "No .ino file found in ${SKETCH_DIR}" | tee -a "$logfile"
  exit 1
fi
echo "Using sketch: $INO_FILE" | tee -a "$logfile"

# --- detect port and board info via arduino-cli
BOARD_INFO="$(arduino-cli board list 2>/dev/null || true)"
if [ -z "$BOARD_INFO" ]; then
  echo "arduino-cli did not return board list; ensure arduino-cli is installed and accessible." | tee -a "$logfile"
  exit 1
fi

# pick the first line that contains ttyACM or ttyUSB (ignore header)
PORT_LINE=$(echo "$BOARD_INFO" | grep -E "ttyACM|ttyUSB" | head -n 1 || true)
if [ -z "$PORT_LINE" ]; then
  echo "No serial port reported by arduino-cli. Is the board connected?" | tee -a "$logfile"
  exit 1
fi

# parse port (first column) and FQBN (last column if present)
PORT=$(echo "$PORT_LINE" | awk '{print $1}')
FQBN=$(echo "$PORT_LINE" | awk '{print $NF}')
# --- validate FQBN and correct common truncated patterns ---
if [[ -z "$FQBN" ]] || [[ "$FQBN" == /dev/* ]] || [[ "$FQBN" =~ ^(Serial|USB)$ ]]; then
  FQBN="arduino:avr:uno"
  echo "No valid FQBN detected, defaulting to ${FQBN}" | tee -a "$logfile"
elif [[ "$FQBN" =~ ^arduino:avr$ ]]; then
  FQBN="${FQBN}:uno"
  echo "Truncated FQBN detected, corrected to ${FQBN}" | tee -a "$logfile"
else
  echo "Detected board FQBN: ${FQBN}" | tee -a "$logfile"
fi

echo "Detected port: $PORT" | tee -a "$logfile"

# --- detect baud from sketch (Serial.begin(NNNN)) ---
BAUD=$(grep -oP 'Serial\.begin\s*\(\s*\K[0-9]+' "$INO_FILE" | head -n 1 || true)
if [ -z "$BAUD" ]; then
  BAUD=9600
  echo "No Serial.begin found; defaulting BAUD=${BAUD}" | tee -a "$logfile"
else
  echo "Detected BAUD=${BAUD} from sketch" | tee -a "$logfile"
fi

# --- free serial port: prefer lsof, fallback to fuser ---
free_port() {
  if command -v lsof >/dev/null 2>&1; then
    PIDS="$(lsof -t "${PORT}" 2>/dev/null || true)"
  elif command -v fuser >/dev/null 2>&1; then
    PIDS="$(fuser -a "${PORT}" 2>/dev/null || true)"
  else
    echo "Warning: neither lsof nor fuser found. Cannot forcibly free port. Proceeding." | tee -a "$logfile"
    PIDS=""
  fi

  if [ -n "$PIDS" ]; then
    echo "Port ${PORT} busy. Terminating PIDs: $PIDS" | tee -a "$logfile"
    for pid in $PIDS; do
      # attempt graceful then force
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
      echo "  killed PID $pid" | tee -a "$logfile"
    done
    sleep 1
  else
    echo "Port ${PORT} is free" | tee -a "$logfile"
  fi
}

free_port

# --- compile
echo "Compiling ${SKETCH_DIR} (FQBN=${FQBN})" | tee -a "$logfile"
if arduino-cli compile --fqbn "${FQBN}" "${SKETCH_DIR}" 2>&1 | tee -a "$logfile"; then
  echo "Compile succeeded" | tee -a "$logfile"
else
  echo "Compile failed — see log above" | tee -a "$logfile"
  exit 1
fi

# --- upload with retry/backoff
MAX_ATTEMPTS=4
SLEEP_BASE=2
uploaded=0
for attempt in $(seq 1 $MAX_ATTEMPTS); do
  echo "Upload attempt $attempt/$MAX_ATTEMPTS to ${PORT}" | tee -a "$logfile"
  if arduino-cli upload -p "${PORT}" --fqbn "${FQBN}" "${SKETCH_DIR}" 2>&1 | tee -a "$logfile"; then
    echo "Upload succeeded" | tee -a "$logfile"
    uploaded=1
    break
  else
    waitsec=$((SLEEP_BASE * attempt))
    echo "Upload attempt $attempt failed; retrying in ${waitsec}s" | tee -a "$logfile"
    sleep "$waitsec"
    # try to free port again in case a process reopened it
    free_port
  fi
done

if [ "$uploaded" -ne 1 ]; then
  echo "Upload failed after ${MAX_ATTEMPTS} attempts. See ${logfile}" | tee -a "$logfile"
  exit 2
fi

# --- optional monitor using arduino-cli monitor (preferred)
if [ "${MONITOR_TIMEOUT}" -gt 0 ]; then
  echo "Launching serial monitor on ${PORT} at ${BAUD} baud for ${MONITOR_TIMEOUT} seconds" | tee -a "$logfile"
  # run monitor in background and kill after timeout
  arduino-cli monitor -p "${PORT}" -c "baud=${BAUD}" &
  MON_PID=$!
  sleep "${MONITOR_TIMEOUT}"
  kill "$MON_PID" 2>/dev/null || true
  wait "$MON_PID" 2>/dev/null || true
  echo "Monitor closed" | tee -a "$logfile"
else
  echo "No monitor requested" | tee -a "$logfile"
fi

echo "Deploy finished at $(date)" | tee -a "$logfile"
exit 0

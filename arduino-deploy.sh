#!/bin/bash
# Arduino Deploy Script with Auto-Port Detection and Cleanup
# Usage: ./arduino-deploy.sh /path/to/sketchdir

set -e

SKETCH_DIR="$1"
FQBN="arduino:avr:uno"

if [ -z "$SKETCH_DIR" ]; then
  echo "Usage: $0 /path/to/sketchdir"
  exit 1
fi

echo "Searching for Arduino..."
PORT=$(arduino-cli board list | grep "Arduino Uno" | awk '{print $1}')

if [ -z "$PORT" ]; then
  echo "No Arduino detected. Retrying with ttyACM0 fallback..."
  PORT="/dev/ttyACM0"
fi

if [ ! -e "$PORT" ]; then
  echo "Could not find Arduino on $PORT"
  exit 1
fi

echo "Found Arduino Uno on $PORT"

# --- Kill any processes using the port ---
echo "Checking for existing processes on $PORT..."
PIDS=$(sudo lsof -t "$PORT" 2>/dev/null || true)

if [ ! -z "$PIDS" ]; then
  echo "Port $PORT is busy. Terminating processes..."
  for PID in $PIDS; do
    PROC_NAME=$(ps -p $PID -o comm=)
    echo "Killing $PROC_NAME (PID $PID)"
    sudo kill -9 "$PID" || true
  done
  sleep 1
else
  echo "Port $PORT is free."
fi

echo "Compiling sketch in $SKETCH_DIR ..."
arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR" || {
  echo "Compilation failed!"
  exit 1
}

echo "Uploading to $PORT ..."
for attempt in {1..3}; do
  if arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"; then
    echo "Upload successful!"
    break
  else
    echo "Upload attempt $attempt failed. Retrying in 3s..."
    sleep 3
  fi
done

if [ "$attempt" -eq 3 ]; then
  echo "Upload failed after multiple attempts."
  exit 1
fi

# Optional: auto-open serial monitor if screen is installed
if command -v screen &>/dev/null; then
  echo "Opening serial monitor (press CTRL+A then K to exit)..."
  screen "$PORT" 9600
else
  echo "Install 'screen' to open the serial monitor automatically."
fi

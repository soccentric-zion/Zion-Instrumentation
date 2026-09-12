#!/bin/sh
# Arches Instrumentation — simulated device entrypoint (REQ §12).
#
# Brings up istate and the nine spokes in the declared startup order (§7.1):
#   istate -> faults, monitord, hardware -> alarms, logmanager, telemetry
#          -> networkd -> ota, backup
# On a real device systemd does this from the unit files; there is no systemd
# in the container, so the order is reproduced here rather than left to luck.
set -eu

RUN_DIR=/run/instrumentation
mkdir -p "$RUN_DIR" /pst/system /pst/state /pst/factory/identity

# A factory identity is normally written by the manufacturing tool. Without
# one istate boots unprovisioned, which is legal but makes the container
# less useful as a stand-in for a real device.
if [ ! -f /pst/factory/identity/identity.yaml ]; then
  cat > /pst/factory/identity/identity.yaml <<YAML
serial: SIM-$(head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n')
hw_revision: sim-rev-a
board_variant: x86-64
mfg_date: 1970-01-01
hostname_base: arches-sim
YAML
  head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > /pst/factory/identity/.secret
fi

pids=""
start() {
  name=$1
  shift
  echo "sim-device: starting $name"
  "/usr/bin/instr-$name" --platform simulated "$@" &
  pids="$pids $!"
}

# Stop everything on signal, in reverse order, so shutdown looks like a
# device shutting down rather than a container being killed.
trap 'echo "sim-device: stopping"; for p in $pids; do kill "$p" 2>/dev/null || true; done; wait; exit 0' INT TERM

start istate
# Wait for the hub sockets before dialling them: a spoke would retry anyway
# (that is what the backoff is for), but starting clean keeps the log honest.
for _ in $(seq 1 50); do
  [ -S "$RUN_DIR/monitord.sock" ] && break
  sleep 0.1
done

for d in faults monitord hardware; do start "$d"; done
sleep 0.5
for d in alarms logmanager telemetry; do start "$d"; done
sleep 0.5
start networkd
sleep 0.5
for d in ota backup; do start "$d"; done

echo "sim-device: up — try 'istate_ctrl status' or 'monitor_ctrl sample-once'"
wait

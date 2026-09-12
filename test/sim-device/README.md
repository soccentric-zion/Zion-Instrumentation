# Simulated device

The full daemon set in a container (REQ §12), so HQ and Configuration can be
developed without hardware.

```bash
docker build -f test/sim-device/Dockerfile -t arches/sim-device .
docker run --rm -it --name arches-sim arches/sim-device
```

It runs the shipped binaries against the simulated platform layer: real gRPC
over real Unix domain sockets, real registration handshake, real config
delivery from `/pst`. Only the hardware is simulated.

Talk to it the way you would talk to a device:

```bash
docker exec -it arches-sim istate_ctrl status
docker exec -it arches-sim istate_ctrl spokes
docker exec -it arches-sim monitor_ctrl sample-once
docker exec -it arches-sim alarm_ctrl raise id=0x10410007 owner=demo text="test alarm"
docker exec -it arches-sim alarm_ctrl list
```

To reach it from a Configuration UI running on the host, publish the socket
directory:

```bash
docker run --rm -it -v /tmp/arches-run:/run/instrumentation arches/sim-device
# Configuration then dials /tmp/arches-run/configuration.sock
```

`/pst` is a volume, so provisioning and configuration survive a restart —
which is the point of `/pst` on a real device too. Mount a named volume to
keep it, or let it go with the container to get a virgin first boot.

## What is not simulated

There is no systemd in the container, so istate's start/stop requests are
recorded rather than executed and the entrypoint starts the spokes in the
declared order instead. Everything above that — the hub-and-spoke topology,
the wire protocol, provisioning, the spool, alarms, OTA state machine — is
the code that ships.

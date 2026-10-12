# Architecture

## Data path

```text
Sender / OBU-side host

Camera
  |
  v
OpenCV -> GStreamer -> H.264 -> RTP
                              |
                              v
                         QUIC stream 4
                              |
                              v
                 picoquic MP-QUIC packetization
                              |
                    round-robin scheduler
                       /             \
                      /               \
                  Uu UDP             PC5 raw
                    |                   |
                    |                   |
                    +--------+----------+
                             |
                             v
                    Receiver / RSU-side host
                             |
                      QUIC stream recovery
                             |
                       RTP / metadata logs
```

The application inserts video and metadata into one QUIC stream. The RR
scheduler therefore operates at picoquic's packet-path selection point; the
application does not create a separate stream per access technology.

## Path establishment

The initial QUIC connection is created over the Uu path. After the connection
becomes ready, the sender probes the configured PC5 endpoint with
`picoquic_probe_new_path_ex()`. Once the path is validated, it becomes eligible
for RR data scheduling.

If a PC5 path disappears, the sender-side reprobe loop waits until the old path
object is gone and then attempts to add a new PC5 path. PC5 RSSI can accelerate
reprobe attempts, but periodic fallback probing prevents RSSI from becoming a
hard gate.

## ACK path

The receiver-side picoquic patch keeps ACK generation path-aware:

```text
DATA received on Uu  -> Uu PATH_ACK
DATA received on PC5 -> PC5 PATH_ACK
```

This matters for per-path RTT interpretation: the sender's path RTT should
represent the forward path plus the return ACK on the same access path, rather
than mixing a PC5 forward path with a Uu return path.

## What RR means here

RR alternates among paths that are:

- present in the picoquic connection;
- not marked as backup;
- not demoted/abandoned; and
- challenge-verified.

If only one path is usable, all data naturally falls back to that path. RR is
included only as a minimal transport demonstration; it is not the learned
TwinSteer scheduling policy.

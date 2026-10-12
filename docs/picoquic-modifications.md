# picoquic modifications

## Upstream base

The public tree is based on:

```text
private-octopus/picoquic
576ca78de2e6b1de99f4ad9d5a15bf748d1e2a58
```

The complete diff is stored in `patches/picoquic-twinsteer.patch`.

## 1. Round-robin scheduler

`picoquic/scheduler.c` and `picoquic/scheduler.h` implement one intentionally
small policy. `picoquic/paths.c` calls `scheduler_pick_path()` when normal
congestion-controlled application data has more than one eligible path.

The scheduler filters out backup, demoted, and unverified paths, then rotates
across the remaining candidates. It also exposes small diagnostic helpers used
by the sender terminal.

No MinRTT, redundant-copy, MAB, STORM, or DQN policy is included in this
public MP-QUIC runtime.

## 2. Same-path ACK

The receiver prototype required ACKs to return over the physical path on which
the corresponding multipath data was received. The patch therefore adds:

- `picoquic_is_ack_needed_on_path()`;
- `picoquic_format_ack_frame_on_path()`;
- a per-path `nb_path_ack_sent` counter for diagnostics; and
- path selection priority for a path whose own ACK context is ready.

`picoquic/sender.c` is updated so ACK formatting uses the selected path's ACK
context rather than the connection-wide default in the modified call sites.

## 3. Build integration

`picoquic/scheduler.c` is added to `PICOQUIC_LIBRARY_FILES` in picoquic's
`CMakeLists.txt`.

## Provenance of the ACK patch

The same-path ACK logic was originally tested in the receiver workspace based
on picoquic commit `7cafedfe53695357be230dec36a39952bf447898`. For this public
artifact it was ported to the newer sender base `576ca78...`, which is 191
upstream commits ahead in the captured repository history.

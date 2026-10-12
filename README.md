# TwinSteer MP-QUIC Reference Implementation

This repository provides the MP-QUIC communication runtime used by the
TwinSteer prototype for heterogeneous V2X transmission over **5G Uu** and
**NR-V2X PC5** paths.

The public artifact intentionally uses a **minimal round-robin (RR) packet
scheduler**. The learning/DQN component of TwinSteer is maintained separately
and is **not** part of this repository. This separation keeps the MP-QUIC
transport implementation easy to inspect, build, and reproduce without
presenting RR as the TwinSteer learning policy.

## What is included

- `sender/` — camera -> H.264/RTP -> MP-QUIC sender
- `receiver/` — MP-QUIC receiver, RTP/metadata recovery, and experiment logs
- `picoquic/` — picoquic source with the minimal RR scheduler and same-path ACK patch
- `tools/pc5_beacon.cpp` — optional RSU-side PC5 beacon utility used in coverage/reprobe experiments
- `scripts/` — build, certificate generation, and run helpers
- `configs/` — example experiment configuration
- `docs/` — architecture, dependencies, picoquic changes, and reproduction notes
- `patches/` — patch against the pinned upstream picoquic base

## Scope

The repository demonstrates the transport/runtime portion of the TwinSteer
prototype:

```text
Camera / telemetry
       |
       v
 MP-QUIC sender
       |
       | packet-level round robin
       +-------------------+
       |                   |
      Uu                  PC5
       |                   |
       +---------+---------+
                 |
                 v
          MP-QUIC receiver
```

It does **not** reproduce the Digital-Twin training pipeline or the learned
TwinSteer/DQN scheduling policy.

## picoquic modifications

The vendored picoquic tree is based on upstream commit:

```text
576ca78de2e6b1de99f4ad9d5a15bf748d1e2a58
```

The reference implementation adds two transport changes:

1. **Minimal round-robin data-path scheduler**
   - considers verified, non-backup, non-demoted paths;
   - alternates packet-path selection across available Uu/PC5 paths;
   - falls back to picoquic's default candidate when no usable multipath candidate exists.
2. **Same-path ACK handling**
   - tracks ACK state per multipath path;
   - when an ACK is due for a path, sends the PATH_ACK on that path before normal data-path selection.

See [`docs/picoquic-modifications.md`](docs/picoquic-modifications.md) and
[`patches/picoquic-twinsteer.patch`](patches/picoquic-twinsteer.patch).

## Requirements

The application layer depends on the vendor-provided **Ettifos SIRIUS SDK**.
The SDK is not redistributed here. The build expects a directory containing
its `inc/` and `lib/` trees and the libraries used by the prototype:

```text
libetf_seamless_sdk
libetfv2x_sdk
libefosv2xsdk_seamless_01
```

The host also needs CMake, a C/C++ toolchain, OpenSSL, gpsd development files,
Mosquitto development files, OpenCV 4, and GStreamer 1.0 development files.
See [`docs/dependencies.md`](docs/dependencies.md).

## Quick start

### 1. Prepare configuration

```bash
cp configs/example.env configs/local.env
$EDITOR configs/local.env
```

Edit the SIRIUS SDK path and Uu/PC5 addresses for the experiment hosts. The
role-specific SIRIUS configuration files are in `sender/` and `receiver/` and
should also be adjusted for the local OBU/RSU management addresses when needed.

### 2. Generate a test certificate

```bash
./scripts/generate_test_cert.sh
```

Do not commit private keys from experimental machines.

### 3. Build

```bash
./scripts/build.sh
```

The top-level CMake build uses picoquic's pinned picotls revision through
`PICOQUIC_FETCH_PTLS=ON`.

### 4. Run the receiver

On the receiver/RSU-side PC:

```bash
./scripts/run_receiver.sh
```

### 5. Run the sender

On the sender/OBU-side PC:

```bash
./scripts/run_sender.sh
```

When both paths are available, the sender terminal should show RR selection
counts for Uu and PC5 increasing approximately alternately. The exact number
of physical packets on each interface can differ because QUIC also emits ACK,
control, retransmission, and path-management packets.

The public RR implementation was hardware-validated on 2026-10-15 with the
deployed SIRIUS OBU/RSU setup. See [`docs/validation.md`](docs/validation.md)
for the observed checks and the remaining packet-level ACK validation item.

## Important measurement note

The receiver logs application timestamps generated on a different host from
the sender. One-way delay derived from `receiver_time - sender_time` is only
meaningful when the two system clocks are synchronized sufficiently tightly.
QUIC RTT samples do not have this cross-host clock-offset problem because RTT
is measured on a single endpoint clock.

## Repository citation

Until the final JSAC bibliographic record is available, the software can be
referenced as:

> TwinSteer Authors, "TwinSteer MP-QUIC Reference Implementation," 2026.
> [Online]. Available: https://github.com/TwinSteer/MPQUIC

`CITATION.cff` is included for GitHub's citation UI and should be updated with
the final paper metadata after publication.

## Third-party software

picoquic is redistributed under its upstream license. The Ettifos SIRIUS SDK
is an external dependency and is not included. See [`THIRD_PARTY.md`](THIRD_PARTY.md).

## License

The TwinSteer reference-implementation code added by this repository is
released under the MIT License. Third-party components retain their own
licenses.

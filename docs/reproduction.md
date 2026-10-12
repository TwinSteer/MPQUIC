# Reproduction guide

This guide reproduces the **MP-QUIC Uu/PC5 runtime with the minimal RR
scheduler**. It does not reproduce the TwinSteer Digital-Twin/DQN training
pipeline.

## 1. Prepare both hosts

Clone the same repository revision on the sender and receiver hosts. Install
the vendor SIRIUS SDK and host dependencies described in `dependencies.md`.

Copy the example configuration:

```bash
cp configs/example.env configs/local.env
```

Set the Uu receiver address/port, OBU and RSU PC5 addresses, sender gpsd host,
and SDK directory.

Edit each role's SIRIUS configuration files if the device management addresses
differ from the included lab example:

```text
sender/sdk_seamless.conf
sender/sdk_mqtt.conf
receiver/sdk_seamless.conf
receiver/sdk_mqtt.conf
```

## 2. Generate certificates

Run once on each checkout, or securely copy the same test certificate material
between the two hosts:

```bash
./scripts/generate_test_cert.sh
```

## 3. Build

```bash
./scripts/build.sh
```

## 4. Start receiver

```bash
./scripts/run_receiver.sh
```

The receiver opens the Uu UDP socket and SIRIUS PC5 raw interface, accepts the
QUIC connection, and writes experiment CSVs under `receiver/logs/`.

## 5. Optional PC5 beacon

For coverage/re-entry experiments, the RSU-side helper can emit the periodic
PC5 traffic used by the prototype's RSSI-based reprobe trigger:

```bash
./scripts/run_pc5_beacon.sh
```

This is not required for steady-state two-path RR transmission.

## 6. Start sender

```bash
./scripts/run_sender.sh
```

After the initial Uu QUIC connection is ready, the sender probes the PC5 path.
Once both paths are verified, the terminal should report RR selections on both
Uu and PC5.

## 7. Sanity checks

For a stationary two-path test:

1. Verify that `nb_paths` reaches 2 on the sender.
2. Verify that both paths are marked usable/verified.
3. Verify that RR selection counters for Uu and PC5 both increase.
4. Verify on the receiver that PATH_ACK counters increase on both paths.
5. Treat physical packet counts as diagnostics, not as an exact 50/50 RR
   proof: QUIC ACK/control/retransmission packets are not application data and
   do not have to follow the same distribution as RR-selected data packets.

## Timing caveat

The receiver's application one-way delay field uses a sender timestamp carried
inside the payload and a receiver host timestamp. Synchronize host clocks
(NTP/chrony/PTP as appropriate) before interpreting that value as one-way
latency. Sender-side QUIC RTT is measured with one endpoint clock and is not
subject to the same cross-host offset.

## Path-label caveat for application logs

The sender's RTP envelope retains a `tx_path` field for compatibility with the
experimental logging format. Under this RR reference implementation, scheduling
occurs after stream insertion at the QUIC packet layer, so that field is only a
**diagnostic snapshot of the most recently selected path**. It must not be used
as authoritative proof that an entire RTP payload traversed one physical path.
For RR transport verification, use picoquic's scheduler selection counters and
low-level per-interface QUIC packet counters instead.

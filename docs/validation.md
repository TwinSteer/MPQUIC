# Validation status

## Repository preparation

- Sender picoquic base: `576ca78de2e6b1de99f4ad9d5a15bf748d1e2a58`.
- Receiver same-path ACK changes were ported to the same newer base.
- The public sender contains only the minimal RR scheduler; the learning/DQN
  component is not included.
- Modified picoquic sources were syntax-checked during repository preparation.

## Hardware validation — 2026-10-15

The public repository was cloned and built independently on the deployed
sender and receiver Linux hosts using the vendor SIRIUS SDK. Both application
targets configured, compiled, linked, and executed successfully.

With the OBU and RSU powered on:

1. SIRIUS/Seamless initialization completed on both endpoints.
2. The initial QUIC connection over Uu completed with multipath enabled.
3. The sender successfully probed and activated the PC5 path.
4. The sender reported two usable paths and approximately alternating RR
   selections. One observed snapshot was `Uu 875 / PC5 869`.
5. The receiver simultaneously observed traffic on both Uu and PC5.
6. Per-path PATH_ACK counters increased on both paths; observed one-second
   samples were close to balanced (for example `Uu 23 / PC5 24`).
7. RTP/video traffic was recovered while both paths were active.

The RR reference intentionally does **not** report per-packet path changes as
"handovers". When round robin is active, consecutive RTP packets are expected
to alternate paths, so the time between packets arriving on different paths is
not a handover-delay metric.

## Remaining validation before a paper release tag

- Capture a packet-level trace that directly confirms each PATH_ACK is emitted
  on the same physical path on which its acknowledged packet was received.
- Test PC5 loss, path demotion, and re-entry if those behaviors are part of the
  published prototype claim.
- Re-run the complete sender/receiver build after any source cleanup and record
  the resulting commit hash.

After those checks, create an immutable tag such as `jsac-artifact-v1.0` and
reference that tag/commit from the paper or release notes.

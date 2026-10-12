# picoquic patch provenance

The vendored `picoquic/` tree is based on upstream commit:

`576ca78de2e6b1de99f4ad9d5a15bf748d1e2a58`

`picoquic-twinsteer.patch` records the TwinSteer MP-QUIC changes relative to
that base. The receiver same-path ACK logic was originally developed on an
earlier prototype base (`7cafedfe53695357be230dec36a39952bf447898`) and was
ported cleanly to the newer base used here.

Additional review diffs:

- `sender-rr-cleanup.patch` — current experimental sender -> public RR-only sender.
- `receiver-config-cleanup.patch` — receiver changes made for public runtime configuration.

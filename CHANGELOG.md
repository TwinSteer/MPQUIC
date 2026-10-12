# Changelog

## 0.1.1 - 2026-10-15

- Record successful sender/receiver hardware validation with OBU/RSU.
- Remove misleading per-packet "handover delay" logging from the RR reference.
- Make sender ACTIVE/standby state reflect actual per-path traffic, while showing
  the last RR-selected path separately.
- Remove the unused `is_nominal_ack_path` local warning in picoquic `sender.c`.
- Fix a stray character in the receiver MQTT example configuration.

## 0.1.0 - 2026-10-08

Initial public-artifact preparation:

- unified sender/receiver picoquic changes on upstream base `576ca78...`;
- retained receiver same-path PATH_ACK handling;
- replaced the multi-policy research scheduler with a minimal RR-only scheduler;
- removed DQN client/inference/model dependencies from the sender runtime;
- changed the sender to a single QUIC stream with packet-level RR scheduling;
- added command-line Uu/PC5 endpoint configuration;
- added CMake build, run scripts, certificate helper, documentation, citation,
  and third-party notices.

# Third-party components

## picoquic

Source: https://github.com/private-octopus/picoquic

The vendored tree is based on upstream commit
`576ca78de2e6b1de99f4ad9d5a15bf748d1e2a58` and retains the upstream license
and per-file notices. TwinSteer-specific modifications are listed in
`docs/picoquic-modifications.md`.

## picotls

Source: https://github.com/h2o/picotls

picoquic's CMake configuration fetches its pinned picotls revision during a
clean build. picotls is not vendored directly in this repository.

## Ettifos SIRIUS SDK

The SIRIUS SDK is proprietary/vendor-provided software and is not redistributed
here. Users must obtain an appropriate SDK release independently and comply
with its license terms.

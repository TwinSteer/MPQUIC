# Dependencies

## Operating system

The original prototype was developed on x86-64 Linux hosts. A recent Ubuntu
release with GCC/G++ and CMake is the intended environment.

## Ettifos SIRIUS SDK

The vendor SDK is required for the PC5 raw send/receive API and radio metrics.
It is intentionally not redistributed in this repository.

Set `SIRIUS_SDK_DIR` to the SDK root containing, at minimum:

```text
<SIRIUS_SDK_DIR>/inc/
<SIRIUS_SDK_DIR>/lib/
```

The application links the following vendor libraries:

```text
etf_seamless_sdk
etfv2x_sdk
efosv2xsdk_seamless_01
```

The exact SDK package/version must be compatible with the deployed SIRIUS
OBU/RSU software. The role-specific `sdk_seamless.conf` and `sdk_mqtt.conf`
files are kept beside each application because the SDK reads its runtime
configuration from the working directory in the original prototype setup.

## Open-source host dependencies

Typical Ubuntu development packages include:

- CMake and build-essential
- OpenSSL development package
- libgps development package
- libmosquitto development package
- OpenCV 4 development package
- GStreamer 1.0 and gstreamer-app development packages

The sender uses OpenCV/GStreamer/gpsd. The receiver does not require
OpenCV/GStreamer but links gps/mosquitto because of SIRIUS SDK dependencies.

## picoquic and picotls

The repository vendors the patched picoquic source. During CMake configure,
picoquic fetches the picotls commit pinned by the selected upstream picoquic
revision. Internet access is therefore required on the first clean build
unless picotls is provided through an alternate local CMake setup.

# Native (in-tree) libfprint port — GXFP5187

This branch (`native-libfprint`) holds the **non-TOD** variant of the driver: the
same reverse-engineered protocol and open matcher as `main`, restructured to be
built **inside the libfprint source tree** rather than as a TOD (Touch-on-Device)
shared module.

> **Status: hardware-validated bring-up (MateBook X Pro, GXFP5187).** Built into a
> current libfprint `main` checkout (the patch applies cleanly there; on 1.94.x the
> meson layout differs), `fprint-list-supported-devices` lists `GXFP5187` and the
> native driver, run against the real sensor, completes the whole non-biometric
> path: probe → open → GPIO reset → firmware read (`GF3288_ST411SEC_APP_11033`) →
> PSK read → **TLS-PSK handshake up (`PSK-AES128-CBC-SHA256`)** → enrol started
> (finger requested). Full enrol/verify matching wasn't run in this harness (needs
> a finger), but the SPI transport, reset, PSK and TLS all work natively via
> `FpiSpiTransfer`. The tested day-to-day variant is still the TOD driver on `main`.
>
> **Requires one upstream libfprint fix.** `fpi_spi_transfer_submit_sync()` calls
> `g_propagate_error(error, err)` unconditionally, so every *successful* synchronous
> transfer trips `g_propagate_error: assertion 'src != NULL' failed`. Async-only SPI
> drivers never hit it; this driver uses `submit_sync` in its worker threads and
> surfaces it (~1400 criticals in one enrol). One-line fix — guard with `if (err)` —
> silences it completely (patch: `libfprint-submit-sync-nullerror.patch`); worth
> sending upstream alongside the driver MR.

## Why a separate variant

TOD ships the driver as a standalone shared object that Ubuntu's libfprint loads
at runtime. It is the fastest path to a working driver, but it is Ubuntu-specific:
Arch, Fedora and Manjaro build libfprint without TOD. The native variant targets
those distributions by living in libfprint's own driver table.

The whole difference from `main` is:

- the TOD entry point (`fpi_tod_shared_driver_get_type`) is removed — the device
  type `fpi_device_goodixtls_get_type` is already registered the in-tree way;
- the SPI transport uses libfprint's `FpiSpiTransfer` / `submit_sync` instead of
  raw `ioctl(SPI_IOC_MESSAGE)`;
- `gx_dev_open` is offloaded to a worker so no blocking SPI runs on the main loop
  (the rest of the driver already used `FpiSsm` + worker offload);
- the reset GPIO is a compiled default with an environment override (see below),
  since the ACPI `_CRS` GpioIo discovery — the proper portable path — is not done
  yet.

Because the TOD entry point is gone, the standalone build/deploy scaffolding from
`main` (`meson.build`, `install.sh`, the udev rule and the spidev conf) does not
apply here and has been dropped; integration is via libfprint's own build,
described below.

## Integrating into a libfprint checkout

Copy the driver sources into a libfprint tree:

```
libfprint/drivers/goodixtls/
    goodixtls.c  goodixtls.h
    goodix_tls.c goodix_tls.h
    goodix_sift.c goodix_sift.h
    config_pcap.c
```

Then wire the two meson files.

`libfprint/meson.build`, in the `driver_sources` map:

```meson
    'goodixtls' : files(
        'drivers/goodixtls/goodixtls.c',
        'drivers/goodixtls/goodix_tls.c',
        'drivers/goodixtls/goodix_sift.c',
        'drivers/goodixtls/config_pcap.c',
    ),
```

`meson.build` (top level), in the `drivers_info` map, next to the other SPI
driver:

```meson
    'goodixtls': { 'spi': true, 'helper': ['udev', 'openssl'], 'optional': not have_spi },
```

libfprint generates the udev rules from the driver's `id_table`
(`MODALIAS==acpi:GXFP5187:`), so nothing needs to be shipped for device matching.

## Reset GPIO

The reset line defaults to the MateBook X Pro (`MACH-WX9`): `/dev/gpiochip0`,
line 58. Other units in the family expose the reset on a different chip/line
(a MateBook 13, for one, uses line 264). Until the driver discovers the line from
the ACPI `_CRS` GpioIo — the proper, portable path, still open — override it at
runtime:

```
GOODIXTLS_RESET_CHIP=/dev/gpiochipN
GOODIXTLS_RESET_LINE=<line>
```

Whatever runs the driver (fprintd, or your own harness) must be allowed to open
the gpiochip. `fprintd.service` ships a `DeviceAllow=` list that does **not**
include gpiochip, and once any `DeviceAllow=` is present systemd enforces a closed
device policy that not even root bypasses — the reset then fails with `EPERM`
while SPI keeps working, so the only symptom is a sensor that looks like it has a
protocol-timing bug. The driver now logs that case instead of failing silently;
to fix it, grant the node in a drop-in:

```
# /etc/systemd/system/fprintd.service.d/goodixtls.conf
[Service]
DeviceAllow=/dev/gpiochip0 rw
```

## Extending to the rest of the Milan-SPI family

This variant is meant to grow into a single driver for the family (`GXFP5187`,
`GXFP51A0`, `GXFP51C0`, …) rather than a driver per sensor. The natural mechanism
is libfprint's `id_table`: list each ACPI id and carry the per-model parameters
(FDT calibration, reset line, config blob) alongside it. Support for a given
sensor should be added **only once it has been validated on real hardware** —
bring-up and TLS handshakes exist for several family members in the community, but
end-to-end enrolment/verification through this driver has been confirmed only on
the 5187 so far. See the matching/enrolment discussion in issue #5.

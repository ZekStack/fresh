# Fresh v0.3.3-rc.1 release notes

This release candidate adds native quad-I/O reads to the ESP-IDF external SPI
NOR backend. `FreshSPIFlashReadMode::FastRead` remains the default. Select
`FreshSPIFlashReadMode::QuadIO` to use QIO. In managed mode, configure the WP and
HOLD pins alongside MOSI, MISO, SCLK, and CS. In external mode, provide a bus
that the application has already initialized for quad I/O. See the [storage
guide](storage.md) for configuration examples.

The release candidate supports firmware integration and board qualification.
Before deploying MX25L25645G with a 32 MiB partition, verify the JEDEC ID,
quad-enable behavior, and persistent read, write, and erase across the 16 MiB
address boundary and at the last sector using the pinned ESP-IDF build. Also
verify blank-media provisioning, mount and unmount, and power-loss recovery.

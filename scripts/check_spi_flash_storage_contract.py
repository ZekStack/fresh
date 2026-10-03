#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "src/storage/FreshSPIFlashStorage.h").read_text(encoding="utf-8")
source = (ROOT / "src/storage/FreshSPIFlashStorage.cpp").read_text(encoding="utf-8")
format_source = (ROOT / "src/FreshFormat.cpp").read_text(encoding="utf-8")
storage_header = (ROOT / "src/FreshStorage.h").read_text(encoding="utf-8")
example = (ROOT / "examples/SPIFlashStorage/SPIFlashStorage.ino").read_text(encoding="utf-8")

assert "FreshStorageType::SPIFlash" in source
assert "SPIFlash" in storage_header
assert "FreshSPIBusOwnership::Managed" in header
assert "FreshSPIBusOwnership::External" in source
assert "formatIfBlank" in header
assert "formatOnMountFailure" in header

for api in (
    "spi_bus_initialize",
    "spi_bus_add_flash_device",
    "esp_flash_init",
    "esp_flash_read_id",
    "esp_flash_get_size",
    "esp_partition_register_external",
    "esp_partition_read",
    "esp_vfs_fat_spiflash_mount_rw_wl",
    "esp_vfs_fat_spiflash_unmount_rw_wl",
    "esp_partition_deregister_external",
    "spi_bus_remove_flash_device",
):
    assert api in source, api

assert "esp_vfs_fat_spiflash_format_cfg_rw_wl" in format_source
assert "openBackend" not in source
assert "existsBackend" not in source
assert "listDirectoryBackend" not in source

unmount = source[source.index("FreshResult FreshSPIFlashStorage::unmount()"):]
assert unmount.index("releaseFilesystem();") < unmount.index("releasePartition();")
assert unmount.index("releasePartition();") < unmount.index("releaseFlash();")
assert unmount.index("releaseFlash();") < unmount.index("releaseManagedSPIBus();")

assert "formatIfBlank = true" in example
assert "formatOnMountFailure = false" in example
print("FreshSPIFlashStorage contract OK")

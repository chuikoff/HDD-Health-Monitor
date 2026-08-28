# DriveMonitor

**Fork of [arisohandriputra/HDD-Health-Monitor](https://github.com/arisohandriputra/HDD-Health-Monitor)** — MIT License.

DriveMonitor is a renamed fork of HDDHealth Monitor. MIT does not require a rename; this one uses the DriveMonitor product name. Original copyright: Ari Sohandri Putra / ARImetic Inc.

**100% Free and Open Source Software (FOSS)**

---

**Support:** Windows 11/10/8.1/8/7/Vista

## What this fork changes

- Skips `IOCTL_STORAGE_PROTOCOL_COMMAND` on the default NVMe path (avoids `nvme.sys` BSOD `DRIVER_IRQL_NOT_LESS_OR_EQUAL`)
- Russian UI (UTF-16), Victoria-style SMART table (ID / Параметр / Значение / Худший / Порог / RAW / Статус)
- USB Realtek/JMicron/ASMedia: SAT first (SATA SSD), then one vendor passthrough (NVMe). No native NVMe IOCTL on USB
- USB adapter name from VID/PID (e.g. Realtek 0BDA:9201), not the disk model
- Unified SMART headline for every drive: **Запас · Износ · Записано** (missing values are —)
- SMART is read once at start, on hotplug, and via **Перечитать**. No live monitoring, tray, graph, or surface test
- Close on X quits the app. Window is resizable. Protocol shown (e.g. NVMe 1.2.1, SATA 6 Гбит/с)
- Slight liquid-glass look (Mica / rounded corners on Windows 11; ignored on older Windows)
- Vendor-aware SMART names/RAW (Seagate, WD, Samsung, Kingston/Phison, ADATA, Toshiba, Micron, Hynix, Intel)
- No tray, no unused refresh timers, no `IOCTL_STORAGE_PROTOCOL_COMMAND` in the tree
- Needs administrator rights

## Author

Fork: **chuikoff** — https://boosty.to/chuikoff

Upstream: **Ari Sohandri Putra** (ARImetic Inc.). MIT copyright stays in [LICENSE](./LICENSE).

## License

This project is released under the [MIT License](./LICENSE).

## What it does

DriveMonitor is a low-level Windows utility that reads raw
S.M.A.R.T. data directly from physical drives via `DeviceIoControl` and
presents it through a GUI. It supports:

- **ATA / SATA** drives via `IOCTL_ATA_PASS_THROUGH_DIRECT`
- **USB** bridges (Realtek RTL9210 SAT + 0xE4, JMicron, ASMedia, …) via `IOCTL_SCSI_PASS_THROUGH_DIRECT` / SAT
- **Internal NVMe** via SCSI miniport / storage query — **not** `IOCTL_STORAGE_PROTOCOL_COMMAND`

## Building

### Prerequisites

- **MinGW-w64** or **TDM-GCC** (any recent GCC with C++ support).

### Build

```bash
# Native Windows build (in a MinGW / MSYS2 shell)
make
```
```bash
# Cross-compile from Linux
make
```

The output binary is `bin/DriveMonitor.exe`.

### Clean

```bash
make clean
```

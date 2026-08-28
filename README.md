# HDDHealth Monitor

**Fork of [arisohandriputra/HDD-Health-Monitor](https://github.com/arisohandriputra/HDD-Health-Monitor)** — MIT License.

This fork keeps the original name and UI. MIT does not require a rename or a redesign. Original copyright: Ari Sohandri Putra / ARImetic Inc.

**100% Free and Open Source Software (FOSS)**

---

**Support:** Windows 11/10/8.1/8/7/Vista

## What this fork changes

- Skips `IOCTL_STORAGE_PROTOCOL_COMMAND` on the default NVMe path (avoids `nvme.sys` BSOD `DRIVER_IRQL_NOT_LESS_OR_EQUAL`)
- Russian UI (UTF-16), Victoria-style SMART table (ID / Параметр / Значение / Худший / Порог / RAW / Статус)
- USB Realtek/JMicron/ASMedia: SAT first (SATA SSD), then one vendor passthrough (NVMe). No native NVMe IOCTL on USB
- USB adapter name from VID/PID (e.g. Realtek 0BDA:9201), not the disk model
- NVMe Health Log like CrystalDiskInfo: spare, wear, host writes in TB. ATA SSD: remaining life + TBW (A9/E9/F1)
- SMART is read once at start, on hotplug, and via **Перечитать**. No live monitoring, tray, graph, or surface test
- Close on X quits the app. Window is resizable. Protocol shown (e.g. NVMe 1.2.1, SATA 6 Гбит/с)
- Needs administrator rights

## Author

**Ari Sohandri Putra** (upstream)

If you find the original tool useful, consider supporting the author via GitHub Sponsors:

> https://github.com/sponsors/arisohandriputra/

Fork changes: [chuikoff/HDD-Health-Monitor](https://github.com/chuikoff/HDD-Health-Monitor)

## License

This project is released under the [MIT License](./LICENSE).

## What it does

HDDHealth Monitor is a low-level Windows utility that reads raw
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

The output binary is `bin/HDDHealthMonitor.exe`.

### Clean

```bash
make clean
```

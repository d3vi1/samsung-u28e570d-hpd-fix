# Linux MStar ISP flashers — source comparison

Source comparison dated 2026-11-06. No Linux hardware flashing was performed.

The closest match for a Linux replacement of the NVIDIA transport is **flashrom's `mstarddc_spi` programmer**. The other two projects below implement the same ISP protocol through different physical bridges; they are useful references, not verified Samsung U28E570DS replacements.

## Reference protocol

[`windows/NvidiaMstarRead.cs`](../windows/NvidiaMstarRead.cs) uses seven-bit I²C address `0x49`, enters with ASCII `MSTAR`, sends `0x10` followed by an SPI command, sends `0x11` before a read, ends the transaction with `0x12`, and exits ISP with `0x24`. `NvidiaMstarSpi.cs` uses the same envelope for SPI write-enable `0x06`, sector erase `0x20`, and page program `0x02`; status polling uses SPI `0x05`.

ISP address `0x49` corresponds to wire address bytes `0x92`/`0x93`. Debug/SERDB is a separate interface at seven-bit `0x59`, wire `0xB2`/`0xB3`. A byte `0x10` sent to SERDB has register-access semantics, not the ISP SPI-envelope semantics. None of these flashers is merely a brightness/DDC VCP application.

## 1. flashrom — direct Linux I²C transport

Repository: [flashrom/flashrom](https://github.com/flashrom/flashrom).

Inspected commit: `30dc47c886517fa46ce205500c2d2bf30623f96b` (2026-11-05).

- [Protocol constants and transaction implementation](https://github.com/flashrom/flashrom/blob/30dc47c886517fa46ce205500c2d2bf30623f96b/programmers/mstarddc_spi.c#L29): exact `0x10`, `0x11`, `0x12`, `0x24` match.
- [ISP entry](https://github.com/flashrom/flashrom/blob/30dc47c886517fa46ce205500c2d2bf30623f96b/programmers/mstarddc_spi.c#L197): address supplied by caller; `MSTAR` entry.
- [SPI master registration](https://github.com/flashrom/flashrom/blob/30dc47c886517fa46ce205500c2d2bf30623f96b/programmers/mstarddc_spi.c#L131): read and page-write operations, maximum data lengths of 256 bytes. This is an actual flasher backend.

Uses `/dev/i2c-*`, Linux `I2C_SLAVE` and `I2C_RDWR`. Important transport difference: it sends `0x11` and receives data as a combined I²C transaction; our validated C# reference uses separate command/read transfers. Default shutdown resets the device; `noreset` is an explicit option. It does not enter SERDB.

Compatibility still depends on the Linux GPU/adapter exposing this monitor's bus and allowing address `0x49`, the necessary transaction types and payload lengths. Recognizing the protocol is not a test of this Samsung, its flash geometry, or a Thunderbolt/DP route. A build must include `mstarddc_spi`.

## 2. OpenIPC SNANDer-MStar — external CH341A I²C

Repository: [OpenIPC/snander-mstar](https://github.com/OpenIPC/snander-mstar).

Inspected commit: `b0a09454fe3150b1da91a26acdb548d0409f736c` (2024-04-18).

- [ISP addresses and `0x10/11/12` commands](https://github.com/OpenIPC/snander-mstar/blob/b0a09454fe3150b1da91a26acdb548d0409f736c/src/ch341a_i2c.c#L20).
- [Command/read implementation](https://github.com/OpenIPC/snander-mstar/blob/b0a09454fe3150b1da91a26acdb548d0409f736c/src/ch341a_i2c.c#L104), [transaction end](https://github.com/OpenIPC/snander-mstar/blob/b0a09454fe3150b1da91a26acdb548d0409f736c/src/ch341a_i2c.c#L189), and [`MSTAR` entry](https://github.com/OpenIPC/snander-mstar/blob/b0a09454fe3150b1da91a26acdb548d0409f736c/src/ch341a_i2c.c#L258).
- [Linux build job](https://github.com/OpenIPC/snander-mstar/blob/b0a09454fe3150b1da91a26acdb548d0409f736c/.github/workflows/main.yml#L15) and [read/write/erase CLI](https://github.com/OpenIPC/snander-mstar/blob/b0a09454fe3150b1da91a26acdb548d0409f736c/readme.txt).

It uses libusb and a CH341A adapter rather than a GPU DDC device. The same SPI envelope is present. It also has a conditional SERDB sequence that changes UART routing; those register assumptions come from its target platform and have not been qualified for MST9U01V. It is therefore an implementation reference and an alternative transport architecture, not a drop-in command for this Samsung.

## 3. Dough monitor tools / specflash — VL822 USB bridge

Repository: [niklas389/dough-monitor-tools](https://github.com/niklas389/dough-monitor-tools).

Inspected commit: `3e7627c40ab7f98f082313748efa91e3b182664a` (2026-09-03).

- [`specflash.c` ISP/SERDB entry and exit](https://github.com/niklas389/dough-monitor-tools/blob/3e7627c40ab7f98f082313748efa91e3b182664a/specflash.c#L122).
- [SPI transaction, erase, program and read code](https://github.com/niklas389/dough-monitor-tools/blob/3e7627c40ab7f98f082313748efa91e3b182664a/specflash.c#L200).
- [Target and Linux transport](https://github.com/niklas389/dough-monitor-tools/blob/3e7627c40ab7f98f082313748efa91e3b182664a/specflash.c#L1).

`specflash` reads, verifies and writes. It uses the identical `MSTAR` / `0x10` / `0x11` / `0x12` / `0x24` ISP protocol, wrapped in vendor USB requests to a VIA VL822 hub. Its target is Dough/Eve ES07D03 with an MST9U4 scaler, not this Samsung. The implementation includes device-specific debug/register writes, flash enabling and fixed 64 KiB erase operations (`0xD8`), unlike our reference's 4 KiB erase (`0x20`). Its transport and preparation sequence must not be assumed portable to MST9U01V.

## Exclusions and practical conclusion

[memues/mstar-monitor-firmware-dumper](https://github.com/memues/mstar-monitor-firmware-dumper) is a dump wrapper around flashrom, not a separate write implementation. [dipcore/mstar-bin-tool](https://github.com/dipcore/mstar-bin-tool) manipulates firmware containers offline; it is not an ISP flasher. Brightness utilities such as `ddcutil` or `asdbctl` do not provide these SPI erase/program operations.

For Linux GPU/DDC transport, the closest match is **flashrom + `mstarddc_spi`**. The other two show independent implementations of the same underlying MStar ISP messages, with clearly different hardware assumptions. No Linux route has been tested on the user's Samsung in this investigation.

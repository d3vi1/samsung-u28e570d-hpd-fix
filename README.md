# Samsung U28E570D DisplayPort standby fix — firmware 1005.0

**1005.0 is an unofficial, experimental modification of Samsung firmware 1004.5.** In the first Mac/TS4 test, the patched monitor stayed in display sleep for **7 minutes 9.746 seconds with zero logged disconnects**, while a stock monitor reconnected **47 times**. The operator confirmed normal image return after manual wake. The monitors used different TS4 outputs, so this is a bounded first result, not a same-unit A/B experiment or general compatibility certification. See the [hardware qualification log](evidence/HARDWARE-RESULTS.md).

This project investigates repeated DisplayPort disconnect/reconnect events while a Samsung LU28E570DS/EN attempts to enter standby. The observed setup was an M2 Max Mac connected through a CalDigit TS4. Mac-side logs recorded approximately 520 ms of HPD LOW every 9.02 seconds while the display remained logically asleep. Those logs do not establish the electrical HPD level at the monitor connector or prove which firmware routine ran.

The version number is a community build identifier, not a Samsung release. This repository contains the tested firmware package, reproducible patch/build/unpack tools, Windows/NVIDIA C# flashing reference code and redacted evidence. Per-monitor full SPI backups and private capture logs are excluded.

## Download

The exact tested image is [firmware/m-E5728UMDB-1005.0-experimental.1.bin](firmware/m-E5728UMDB-1005.0-experimental.1.bin), also attached to the [1005.0-experimental.1 prerelease](https://github.com/d3vi1/samsung-u28e570d-hpd-fix/releases/tag/1005.0-experimental.1). Its firmware-ID string is `m-E5728UMDB-1005.0`, and its service-menu checksum is **3899** (hexadecimal). Verify it with [firmware/SHA256SUMS](firmware/SHA256SUMS).

This is a **581,028-byte update package**, not a complete 2 MiB SPI image. The flashing reference constructs a full target from the exact unit's verified backup, preserving all bytes beyond the package. Do not pad this package and write it over a whole flash chip.

## Supported input

The patch is specific to the examined MST9U01V firmware image for this monitor family. Similar model names or scaler chips are not sufficient evidence of compatibility.

Obtain the original image from [Samsung's 1004.5 archive](https://downloadcenter.samsung.com/content/FM/202308/20230802123043536/m-E5728UMDB.zip). The **extracted firmware binary**, not the ZIP, must match:

```text
Size:   578748 bytes
SHA256: 60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b
```

The prebuilt package is provided for the examined firmware family. The builder below reproduces it from the official image. Building and unpacking are offline operations; hardware programming is a separate step.

## Build offline

Requires Python 3.10 or later and the exact stock BIN above. There are no third-party Python dependencies.

```sh
python3 build.py path/to/stock-1004.5.bin candidate-1005.0.bin
python3 tests/test_build.py path/to/stock-1004.5.bin
python3 tools/firmware_container.py inspect candidate-1005.0.bin
```

To inspect and unpack either supported package, then repack a body without modifying the input:

```sh
python3 tools/firmware_container.py unpack path/to/stock-1004.5.bin unpacked-stock
python3 tools/firmware_container.py repack path/to/stock-1004.5.bin \
  unpacked-stock/application.bin reconstructed-stock.bin --reuse-original-stream
# For an intentionally modified body, omit --reuse-original-stream:
python3 tools/firmware_container.py repack path/to/stock-1004.5.bin \
  modified-application.bin repacked.bin
```

`unpack` validates the container before extracting `application.bin`, `application.deflate` and `manifest.json` into a new directory. The original-stream reconstruction must match the stock BIN byte for byte. Generic `repack` validates structure and length; only `build.py` applies and checks the reviewed 1005.0 application delta.

The builder refuses a different stock hash, unexpected original instruction bytes, overlapping patches, a changed application length, or existing output files. It writes the BIN and a JSON manifest. It does not flash or communicate with a monitor.

Reference build with Python zlib runtime **1.2.12**:

```text
Build:          1005.0-experimental.1
BIN size:       581028 bytes
BIN SHA256:     3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4
Body SHA256:    a4d6791e80e9050f4800e3fcf9fa2fd1af0f84dac0d99ee0b18f2dc17712d3b0
Service sum16:  3899
```

A different zlib implementation may emit a different compressed stream and BIN hash. The expanded body must always match the pinned patched-body hash. Hardware testing must pin the exact produced BIN and full, monitor-specific flash image; do not substitute an unreviewed build because its version label matches.

The delta changes 87 actual application bytes across three declared spans: a four-byte branch, a 264-byte zero-padding span containing the guarded stub, and the six-byte version label. Original bootstrap code and PM51 are preserved; the main bootstrap's embedded compressed-length field changes when the stream size changes. All four container CRCs are rebuilt. See [packaging](evidence/PACKAGING.md), [stub review](evidence/stub-review.txt) and [CPU emulation result](evidence/emulation-result.txt).

The Ghidra scripts under `tools/` and `tests/` document assembly and p-code validation using [eb4x/ghidra-plugin-aeon](https://github.com/eb4x/ghidra-plugin-aeon). They operate on a stock application imported at `0x203800`; the read-only assembler script emits `hook.bin` and `stub.bin`, then the emulator script reads those two files. They are optional for normal Python builds, which use the reviewed delta.

## Windows/NVIDIA flashing reference

The optional [Windows laboratory implementation](windows/experimental/README.md) includes an independent C# container validator, an exact full-image transition planner, a rollback engine, simulated-failure tests and a disabled NVAPI hardware adapter. Its 21 simulator cases passed on PowerShell 7 and Windows PowerShell 5.1. Hardware execution remains disabled in the repository; the reviewed local experiment uses a separate, explicit device profile.

The transport is in [NvidiaMstarRead.cs](windows/NvidiaMstarRead.cs) and [NvidiaMstarSpi.cs](windows/NvidiaMstarSpi.cs); the transaction engine is [FlashTransition.cs](windows/experimental/FlashTransition.cs). NVAPI carries raw I²C messages over the GPU's monitor connection. The MStar ISP bridge then carries SPI flash commands; this is separate from ordinary DDC/CI VCP requests at address `0x37`.

The successful first programming run changed **109 sectors of 4 KiB**, verified each sector, wrote the header sector last, then verified the complete 2 MiB target and unchanged flash status. The package/bootstrap distinction and rollback treatment matter: replacing a longer candidate with a shorter stock package must also restore the tail from the same unit's backup. The code does not promise recovery from power loss or lost ISP access.

## Linux flashers using the same MStar ISP protocol

The shared protocol uses seven-bit I²C address **`0x49`** (wire address bytes `0x92`/`0x93`), ASCII **`MSTAR`** to enter ISP, `0x10` followed by an SPI command, `0x11` before reading, `0x12` to finish the SPI transaction and `0x24` to exit/reset. SPI commands then include read `0x03`, write-enable `0x06`, page program `0x02` and status `0x05`. SERDB at seven-bit address `0x59` is a separate debug interface; identical opcode bytes there do not have the same meaning.

| Project | Linux transport and relevance |
|---|---|
| [flashrom — mstarddc_spi](https://github.com/flashrom/flashrom/blob/30dc47c886517fa46ce205500c2d2bf30623f96b/programmers/mstarddc_spi.c) | Actual read/write backend over `/dev/i2c-*`; closest replacement for GPU/DDC transport. Uses the same ISP entry, transaction and exit messages. |
| [OpenIPC/snander-mstar](https://github.com/OpenIPC/snander-mstar) | Actual flasher through a CH341A/libusb I²C adapter. Same ISP envelope; optional SERDB setup is platform-specific. |
| [Dough monitor tools — specflash](https://github.com/niklas389/dough-monitor-tools/blob/3e7627c40ab7f98f082313748efa91e3b182664a/specflash.c) | Actual flasher through a VIA VL822 USB bridge. Same ISP messages, but targets a Dough/Eve monitor and another MStar scaler. |

These Linux paths were **not tested on this Samsung**. In particular, flashrom combines its `0x11` command and read in one I²C transaction, whereas the tested NVIDIA path uses separate transfers. Bus access, payload limits, flash geometry, device preparation and shutdown/reset behavior must be checked for the actual transport. `specflash` also uses 64 KiB erase and device-specific register writes, unlike the reviewed 4 KiB sector transition here. See the [commit-pinned source comparison](evidence/LINUX-FLASHERS.md).

## Read the running firmware identifier on macOS

The optional helper sends only the fixed C3/0 identification query. By default it requires exactly one active external display and one DCP AV service. With multiple external displays, an explicit `--registry-id ID` selects one observed DCP AV service; the helper still refuses to query while any external display is asleep/inactive. It uses private IOAV APIs and has been exercised on the investigation's M2 Max/TS4 setup; compatibility with other Macs is not established.

```sh
clang -Wall -Wextra -framework Foundation -framework IOKit -framework CoreGraphics \
  macos/read_firmware_id.m -o /tmp/samsung-read-firmware-id
/tmp/samsung-read-firmware-id
```

This is an active DDC query: run it while awake, before or after a passive standby capture, never as a polling loop during sleep. It validates the frame and checksum. The version distinguishes the two test firmwares when the DisplayPort EDID uses a generic serial; it does not replace physical unit identification.

## What the experiment changes

Stock startup writes HPD LOW at AEON address `0x240A2B`, even though it has already imported the PM51 wake mailbox. Later initialization requests HPD HIGH. Normal internal standby mode skips a separate explicit HPD LOW call; internal power-off mode executes it.

### Why the shared startup path is the patch target

The examined MST9U01V image contains an AEON R2 main application and an 8051-compatible PM51 power-management program. The PM51 detector publishes input activity as event `0x0A`, with the input port, while the key/JOG path publishes event `0x0E`. Both can wake the main application. A button wake and a DP activity wake therefore do not require different reset vectors; they require the common startup to use the retained wake context before changing the DP connection state.

The recovered order is:

1. PM51 packs ten bytes at XDATA `0x507C`, publishes them through RIU `0x103380..0x103389` with bytes swapped within each word, and sets RIU marker `0x10181E` to `2`.
2. AEON startup `0x2409AE` calls `0x230527` → `0x20854A`, importing the mailbox to RAM `0x35A82E..0x35A837` when the marker is valid.
3. At `0x240A24` / `0x240A28` / `0x240A2B`, startup reads the GPIO output, clears bit 5 with `AND 0xDF`, and writes it back. This HPD LOW operation has no wake-reason guard in stock 1004.5.
4. Only later does policy routine `0x240203` interpret wake reasons; subsequent DP initialization requests HPD HIGH through `0x20934F`.

This ordering loses the distinction between initializing a new connection and resuming an already connected sleeping sink. The patch moves a narrow preservation decision to the point where HPD would otherwise be cleared. The stable-standby hardware result supports this explanation; we did not trace the processor's instruction pointer during the original loop.

The candidate patch preserves the already asserted HPD at this one startup point only when **all** these conditions hold:

| Condition | Required value |
|---|---|
| PM51 mailbox marker, RIU `0x10181E` | `2` |
| Wake reason, 16-bit RAM `0x35A82E` | `0x000A` — input activity |
| Wake port, 16-bit RAM `0x35A830` | `0` — DP in this image |
| Special forced-off field, 16-bit RAM `0x35A836` | `0` |
| HPD output latch, bit 5 at MMIO `0xA0000840` | Already HIGH |
| HPD output-enable field, bit 5 at `0xA0000844` | Already `0`, selecting output |

All other cases execute the original startup operation. The patch does not force a LOW or tri-stated HPD HIGH. Key/JOG wake reasons, cold-start reasons, the separate recovery pulse routines and the remainder of DP initialization retain their existing paths. The internal PM mode numbers are unrelated to DPCD power-state values.

The builder operates offline. It does not access DDC, issue live GPIO commands, or program configuration EEPROM. The intended firmware change guards one existing HPD GPIO operation; normal firmware configuration and counter writes are not globally disabled.

### Exact binary changes and control flow

| Runtime address | Change |
|---|---|
| `0x240A24` | Replace original four-byte `LBZ` (`F0 E6 08 40`) with a branch to `0x2EB500` |
| `[0x2EB500, 0x2EB608)` | Guarded stub in an examined 264-byte zero-padding span |
| `0x3303E5` | Change six-byte version label `1004.5` to `1005.0` |

The stub reads the marker through MMIO `0xA020303C`, checks the little-endian mailbox words and checks the current GPIO output/direction. If all six conditions pass, it branches to `0x240A2F`, after the LOW write. Otherwise the fallback at `0x2EB600` executes the displaced load and returns to `0x240A28`, preserving the stock clear/store sequence. The stub preserves `r5`, `r6` and the condition flag, uses `r7` as the original scratch register, and makes no calls or stack accesses.

Only **87 byte positions** differ in the expanded application; the larger declared stub span includes unchanged zero padding. The original PM51 program is untouched. The stub lies inside the decompressed application `[0x203800, 0x33B110)`, while CRT0 clears only `[0x33B110, 0x35B460)`, so the startup BSS clear does not erase it. See [the exact delta](patches/1005.0-experimental.1.json), [loader analysis](evidence/LOADER-BSS.md), [assembly](evidence/stub-assembly.txt) and [720-case p-code test](evidence/emulation-result.txt).

### Timing and alternative paths

The approximately **9.02-second** cycle is the interval measured at WindowServer. It is not a proven firmware timer constant: monitor wake/startup, input scanning, the TS4 and macOS can all contribute latency. The monitor's nominal timing can therefore be shorter than the host-observed period.

There is a separate nominal 500 ms HPD pulse helper (`0x242BA2` → `0x22D5D0`) associated with DP version/capability changes and reset/default paths. Normal startup applies the DP version through a lower-level routine without unconditionally using that wrapper. Input-selection paths also request other long pulses. These paths remain unchanged; matching a measured approximately 520 ms host interval to a 500 ms software constant alone would not prove causation.

The service-menu HotPlugTime value `9` is interpreted as 900 ms on its examined eligible ports; its port mask excludes DP0 in this image. It does not explain a nine-second DP cycle. The extra model-dependent delay investigated through EEPROM offset `0x8E` was also not used as a patch target: the normal Information model text identified the examined suffix as `/`, outside the relevant `L`/`Y` branch. No configuration EEPROM change was needed.

## Firmware container, compression and checksums

The package has a bootstrap region, an image header, a 64 KiB PM51 slot, an uncompressed AEON head and a raw-DEFLATE application. Offsets below are **file offsets** unless explicitly described as runtime addresses. All intervals are end-exclusive.

| Offset / region | Meaning |
|---|---|
| `[0, 0x10000)` | Original bootstrap region, preserved |
| `0x10000`, LE32 | Main image file start `0x20080` |
| `0x10004`, LE32 | Main load address `0x200000` |
| `0x10008`, LE32 | Load address plus stored main-image length |
| `0x1000C`, LE32 | Main end `E`, immediately after inner CRC16 |
| `0x10014`, LE32 | Main CRC32 |
| `0x10020`, LE32 | Final file CRC32 offset `C` |
| `0x10024`, LE32 | CRC16 coverage start `0x10000` |
| `[0x10080, 0x20080)` | PM51 slot, preserved |
| `[0x20080, 0x23880)` | Uncompressed main head (`0x3800` bytes) |
| `0x211C7` | Compression marker `BE AD DE` |
| `0x211CA`, LE32 | Stored main-image length `E - 0x20080` |
| `0x211CE`, LE32 | Uncompressed head length `0x3800` |
| `[0x23880, E - 6)` | Raw DEFLATE stream, without zlib/gzip wrapper |
| `E - 6`, LE32 | Expanded application size `0x137910` |
| `E - 2`, BE16 | Inner CRC16 |
| `[E, A)` | Zero padding; `A = (E + 7) & ~7` |
| `[A, A + 6)` | Trailer marker `55 AA AB CD 00 00` |
| `A + 6`, BE16 | Outer CRC16 |
| `C = A + 8`, BE32 | Final file CRC32; package length is `C + 4` |

The application expands to runtime base `0x203800`, with length `0x137910`. The original body hash is `43d99a1e97a11735a95b5547fa0cbe297100753481c453876d970d747bc442d4`.

Both CRC algorithms are **MSB-first, initial value 0, no reflection, no final XOR**. CRC16 uses polynomial `0x8005`; CRC32 uses `0x04C11DB7`. Python's ordinary `zlib.crc32()` is not the algorithm used here. Field byte order differs from CRC bit-processing order.

Repacking must follow this dependency order:

1. Compress the application as raw DEFLATE; update the header lengths/pointers and the embedded length at `0x211CA`.
2. Zero the main CRC32 field at `0x10014` and append the expanded-size LE32 after the stream.
3. Compute inner CRC16 over `[0x10000, E - 2)` and append it big-endian.
4. Pad to eight-byte alignment, append the six-byte marker, then compute outer CRC16 over `[0x10000, A + 6)` with the header CRC32 still zero. Append it big-endian.
5. Compute main CRC32 over `[0x20080, E)` and fill `0x10014` little-endian.
6. Compute final CRC32 over `[0, C)` and append it big-endian.

For the published 1005.0 image, the main/inner/outer/final checksums are **`9D87C56E` / `F774` / `BBB7` / `B77081C5`**. The separate service-menu sum is the sum of every package byte modulo 65536: **`0x3899`**. It is not a CRC and does not replace SHA-256 verification. The constant outer CRC16 observed across the examined images follows from the inner CRC's zero residue followed by fixed padding/marker bytes, rather than a magic checksum exemption.

The repacker updates only the length word in the otherwise preserved main head, rebuilds all integrity fields, then decompresses and verifies the result. It rejects packages extending beyond `0x90000`; this conservative format bound is not a substitute for checking the physical flash layout. Full field values, reconstruction evidence and corroborating MStar source links are in [PACKAGING.md](evidence/PACKAGING.md).

## What remains unknown

Reason `0x0A` identifies input activity; it does not independently prove that the previous state was DP sleep. The additional conditions narrow the experiment but do not establish mailbox freshness across every reset scenario.

Preserving HPD also does **not** establish continuous AUX availability. The subsequent firmware initialization still reloads EDID and initializes DP receiver hardware. Its effect on live AUX/DPCD service is not fully understood. Suppressing hotplug events while breaking later wake or AUX communication would be a failed result.

The observed loop has not been traced to execution of `0x240A2B`. Other timed and recovery HPD paths exist. This experiment tests one specific hypothesis; it is not a compliance certification or a universal workaround.

[VESA DisplayPort 1.2, §5.2.4–5.2.5](https://glenwing.github.io/docs/DP-1.2.pdf#page=454) specifies asserted HPD during standby/power saving, distinguishes short IRQ pulses from long hotplug pulses, and requires AUX availability. These requirements motivate the investigation; they do not validate this patch.

## Qualification status

| Evidence | Current status |
|---|---|
| Stock startup, mailbox and DP wake paths | Statically analyzed |
| Guarded patch design | Reviewed as an experiment |
| Final binary and container verification | Built; 7 integration checks and 9 container-format checks passed |
| Hook/stub review and CPU emulation | Independent scoped review passed; 720 state combinations passed |
| Loader/BSS treatment of stub | Verified: BSS clearing excludes the stub |
| Physical HPD/AUX capture of the original loop | Not obtained |
| Patched firmware flashed or booted | Verified on two units; first unit's initial DDC settling issue and second unit's immediate successful check recorded separately |
| Initial Mac/TS4 standby and wake | 429.746217 seconds, zero patched-path disconnects; image returned after manual wake |
| Windows/HDMI standby and wake on second unit | Not executed: capture preparation ended before display-off; no HDMI standby result |
| Broader regression matrix / electrical AUX compliance | Not established |

Offline validation must verify the exact input hash, patch-site bytes, instruction boundaries, branch destinations, register preservation, unchanged unrelated regions, and decompression round-trip of the final container. Predicate tests must cover every guard's failure as well as the intended preserve case. These checks must be reported separately from hardware tests.

## Hardware test plan

Before any test flash, have a verified **2 MiB full SPI backup from the exact monitor being tested**, with its own serial-specific contents and a usable recovery method. **Never restore another monitor's dump.** A stock update package is not a substitute for that backup. A full SPI backup does not necessarily include separate configuration EEPROM or other non-SPI storage.

Use one monitor at a time. Record the complete model, original version, host/OS, DP mode, dock/adapters and cable. Keep serials and full dumps private. Obtain a stock baseline under the same conditions. Record screen/backlight behavior together with host logs; host timestamps alone cannot distinguish monitor timing from dock and OS latency.

| Test | Required observation |
|---|---|
| Cold power-on and ordinary startup | Normal detection, image and controls |
| Display sleep using the original reproduction steps | Sustained standby without repeated backlight/OSD cycles; no new disconnect loop |
| Wake after short and prolonged standby | Image returns reliably; resolution and refresh rate are retained |
| System sleep and wake | Reliable recovery without an extra monitor power cycle |
| JOG wake, power-off/on and input switching | Existing controls and source selection work |
| Physical DP unplug/replug, awake and asleep | Genuine reconnection is detected and recovers |
| Supported DP 1.1/1.2 settings | Repeat sleep/wake and reconnect tests in each supported mode |
| Direct DP path versus the original TS4 path, when available | Record results separately to distinguish adapter-dependent behavior |
| HDMI and return to DP | Existing HDMI behavior and subsequent DP recovery work |
| Post-wake EDID/DPCD communication | Correct identification and responsive AUX; no persistent read failures |
| Restore the same unit's original backup | Original startup and display operation are recovered |

For the standby reproduction, avoid repeated DDC/AUX polling: it may itself affect wake behavior. Perform communication checks after deliberate wake or as a separate, explicitly documented experiment. A hardware analyzer can establish whether HPD and AUX remain correct at the monitor connector.

For broader qualification, capture at least ten minutes of standby and repeat at least ten sleep/wake cycles, followed by a longer idle test. Record the actual durations and counts, including failures. The first published run ended earlier through intentional keyboard activity; its exact duration and single wake observation are reported above. That result does not establish long-term reliability or complete AUX compliance.

When reporting a result, include the candidate image hash, test matrix results, reproduction steps and relevant redacted logs. Distinguish measured electrical signals, host software events and visual observations. A successful report should demonstrate both stable standby and reliable subsequent wake.

## License and firmware ownership

The project's original build tools, patch code and documentation are provided under the [MIT license](LICENSE). Samsung's code in the original and modified firmware remains Samsung's material; the project license does not relicense it. Third-party tools retain their own licenses. The official source package is linked above; monitor-specific backups are not distributed.

## Sponsoring

Support the work through [GitHub Sponsors: d3vi1](https://github.com/sponsors/d3vi1). The repository's Sponsor configuration is [.github/FUNDING.yml](.github/FUNDING.yml); details are in [SPONSORING.md](.github/SPONSORING.md).

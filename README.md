# Samsung U28E570D HPD standby experiment

**1005.0 is an unofficial, experimental modification of Samsung firmware 1004.5. Programming, full read-back and boot have been verified on one unit; the DisplayPort standby fix is not yet confirmed.** The version number is a community build identifier, not a Samsung release. See the [hardware qualification log](evidence/HARDWARE-RESULTS.md).

This project investigates repeated DisplayPort disconnect/reconnect events while a Samsung LU28E570DS/EN attempts to enter standby. The observed setup was an M2 Max Mac connected through a CalDigit TS4. Mac-side logs recorded approximately 520 ms of HPD LOW every 9.02 seconds while the display remained logically asleep. Those logs do not establish the electrical HPD level at the monitor connector or prove which firmware routine ran.

Repository: `d3vi1/samsung-u28e570d-hpd-fix`. Preparation remains private until hardware testing is recorded; no public release is qualified yet.

## Supported input

The patch is specific to the examined MST9U01V firmware image for this monitor family. Similar model names or scaler chips are not sufficient evidence of compatibility.

Obtain the original image from [Samsung's 1004.5 archive](https://downloadcenter.samsung.com/content/FM/202308/20230802123043536/m-E5728UMDB.zip). The **extracted firmware binary**, not the ZIP, must match:

```text
Size:   578748 bytes
SHA256: 60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b
```

The intended distribution contains the patch/build script, tests and analysis, rather than a complete vendor firmware image or anyone's monitor backup. The builder below produces a locally generated candidate. Flashing is separate and is not performed by this repository.

## Build offline

Requires Python 3.10 or later and the exact stock BIN above. There are no third-party Python dependencies.

```sh
python3 build.py path/to/stock-1004.5.bin candidate-1005.0.bin
python3 tests/test_build.py path/to/stock-1004.5.bin
python3 tools/firmware_container.py inspect candidate-1005.0.bin
```

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

## Windows laboratory tools

The optional [Windows laboratory implementation](windows/experimental/README.md) includes an independent C# container validator, an exact full-image transition planner, a rollback engine, simulated-failure tests and a disabled NVAPI hardware adapter. Its 21 simulator cases passed on PowerShell 7 and Windows PowerShell 5.1. Hardware execution remains disabled in the repository; the reviewed local experiment uses a separate, explicit device profile.

## Read the running firmware identifier on macOS

The optional helper sends only the fixed C3/0 identification query. It requires exactly one active external display and one DCP AV service, and refuses to query an asleep/inactive display. It uses private IOAV APIs and has been exercised on the investigation's M2 Max/TS4 setup; compatibility with other Macs is not established.

```sh
clang -Wall -Wextra -framework Foundation -framework IOKit -framework CoreGraphics \
  macos/read_firmware_id.m -o /tmp/samsung-read-firmware-id
/tmp/samsung-read-firmware-id
```

This is an active DDC query: run it while awake, before or after a passive standby capture, never as a polling loop during sleep. It validates the frame and checksum. The version distinguishes the two test firmwares when the DisplayPort EDID uses a generic serial; it does not replace physical unit identification.

## What the experiment changes

Stock startup writes HPD LOW at AEON address `0x240A2B`, even though it has already imported the PM51 wake mailbox. Later initialization requests HPD HIGH. Normal internal standby mode skips a separate explicit HPD LOW call; internal power-off mode executes it.

The candidate patch preserves the already asserted HPD at this one startup point only when **all** these conditions hold:

| Condition | Required value |
|---|---|
| PM51 mailbox marker, RIU `0x10181E` | `2` |
| Wake reason, 16-bit RAM `0x35A82E` | `0x000A` — input activity |
| Wake port, 16-bit RAM `0x35A830` | `0` — DP in this image |
| Special forced-off field, 16-bit RAM `0x35A836` | `0` |
| HPD output latch, bit 5 at MMIO base + `0x840` | Already HIGH |
| HPD output-enable field, bit 5 at base + `0x844` | Already `0`, selecting output |

All other cases execute the original startup operation. The patch does not force a LOW or tri-stated HPD HIGH. Key/JOG wake reasons, cold-start reasons, the separate recovery pulse routines and the remainder of DP initialization retain their existing paths. The internal PM mode numbers are unrelated to DPCD power-state values.

The builder operates offline. It does not access DDC, issue live GPIO commands, or program configuration EEPROM. The intended firmware change guards one existing HPD GPIO operation; normal firmware configuration and counter writes are not globally disabled.

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
| Patched firmware flashed or booted | Verified on one unit; initial DDC settling issue and later valid checks recorded separately |
| Standby-loop fix and regression matrix | Not tested |

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

As an initial qualification run, capture at least ten minutes of standby and repeat at least ten sleep/wake cycles, followed by a longer idle test. Record the actual durations and counts, including failures. Do not treat one successful wake or the absence of Mac hotplug notifications as sufficient evidence.

When reporting a result, include the candidate image hash, test matrix results, reproduction steps and relevant redacted logs. Distinguish measured electrical signals, host software events and visual observations. A successful report should demonstrate both stable standby and reliable subsequent wake.

## License and firmware ownership

The project's original build tools, patch code and documentation are provided under the [MIT license](LICENSE). This license does not apply to Samsung's firmware or third-party tools. Obtain the base firmware from Samsung; monitor-specific backups are not part of this repository.

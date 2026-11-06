# Hardware qualification log

Build: **1005.0-experimental.1**. This log separates programming/boot evidence, the first standby observation and the remaining qualification limits.

## Programming and boot — 2026-11-05

One Samsung LU28E570DS/EN was tested through the reviewed Windows/NVIDIA laboratory path. Device identifiers, full dumps and private run logs are excluded from this repository.

- The complete 2 MiB preflight matched the pinned 1004.5 baseline.
- All 109 changed 4 KiB sectors were programmed and individually verified, with the header sector last.
- No programming errors, retries or rollback were reported.
- The final complete 2 MiB read matched `570adc6fda032365587a183450887bdfb6bbe967afbd31648d472e4fc8b3de75`; flash status registers were unchanged.
- ISP exit/reset was sent at 23:18:27 Europe/Bucharest.
- Live EDID passed immediately afterward. The first DDC C8 response, approximately three seconds after reset, was invalid. The original run's immediate post-boot check therefore failed; this result is not rewritten as an immediate success.
- Later, after settling, C8 passed before and after the firmware-ID query. Two identical, checksum-valid C3/0 responses identified the running application as `m-E5728UMDB-1005.0`. Live EDID passed again. No extra reset or rollback was used to obtain these results.
- The queried settings matched before/after: brightness/contrast 0/0, RGB 50/50/50, preset 2, raw input value 5. The 0/0 settings were intentional service-menu preparation. This comparison is not a complete configuration-EEPROM dump.

Observed firmware-ID response:

```text
6E 94 C3 00 6D 2D 45 35 37 32 38 55 4D 44 42 2D 31 30 30 35 2E 30 4D
```

The corresponding read request is `51 82 C3 00 7E` to I2C address `0x37`. Static analysis of handler `0x243A53` shows that subcommand 0 copies 18 bytes from initialized application address `0x3303D9` into the response; it contains no configuration-EEPROM call. Querying is performed only in normal operation, after leaving ISP.

## DisplayPort standby/wake — 2026-11-06

The operator connected both monitors to the M2 Max Mac through the CalDigit TS4. The stock unit remained on native DisplayPort; the patched unit used downstream USB-C to DisplayPort, with brightness/contrast 0/0. A checksum-valid C3/0 query identified 1005.0 before capture. No DDC/AUX queries were sent by the capture tools during sleep.

The controlled interval begins with `Display state: Off` at **06:38:51.736071 EET** and ends at the keyboard activity tickle at **06:46:01.482288 EET**: **429.746217 seconds**. The operator confirmed that this wake was intentional. It was not a completed ten-minute run.

| Observation during that interval | Stock 1004.5 | Patched 1005.0 |
|---|---:|---:|
| WindowServer disconnect/reconnect pairs | 47 | 0 |
| Mean interval between stock disconnects | 9.021928 s | No events |
| Patched display present, inactive and asleep | — | All 428 samples |
| Operator's visual observation | Screen/backlight cycles | Only blue standby LED blinks |

Some driver/Thunderbolt messages are absent. The counts above use the complete WindowServer pairs; events from different layers are not added together. Eight complete stock Thunderbolt LOW/HIGH pairs measure 515.445–522.988 ms. These are host timestamps, not electrical HPD measurements.

The patched monitor had one interface termination and WindowServer OUT/IN **during the intentional wake**, at 06:46:02.981330 and 06:46:05.280691, lasting **2.299361 seconds**. This is retained in the result, not counted as a standby failure or omitted. The operator confirmed normal image return without cable or power intervention. After capture stopped, another active DDC query returned the same checksum-valid `m-E5728UMDB-1005.0` identifier.

The capture remained healthy: no stderr errors, 21,869 log records and 428 registry samples in the controlled sleep window. Redacted machine-readable results are in [standby-20261106.json](standby-20261106.json); the stock WindowServer event offsets are in [stock-windowserver-events.csv](stock-windowserver-events.csv).

**Limits:** this compares different physical units and different TS4 output paths, not the same unit before and after the patch. Cached VRR capabilities and brightness/contrast differ. The original symptom trace used the stock unit at 100/50. There is no electrical HPD/AUX analyzer capture, same-unit rollback comparison, repeated wake-cycle qualification or long-term reliability result. Stable standby and successful wake were observed in this specific first test; complete DisplayPort compliance is not established.

## Second unit programming and boot — 2026-11-06

The stock comparison unit from the Mac test was subsequently connected to the Windows/NVIDIA host through HDMI 1 at 30 Hz and upgraded using the same immutable candidate. Its own identity, backup and legacy flash profile were verified before programming; its profile uses SR1/SR2 without assuming SR3 availability.

- Full 2 MiB preflight matched the expected 1004.5 baseline at 07:06:33 Europe/Bucharest.
- All 109 changed 4 KiB sectors passed individual verification by 07:15:02, with header sector `0x10000` last. There were zero reported errors, retries or rollbacks.
- The final complete 2 MiB read matched `570adc6fda032365587a183450887bdfb6bbe967afbd31648d472e4fc8b3de75`; SR1/SR2 remained `00/00`.
- ISP exit occurred at **07:27:26.703 Europe/Bucharest**. This unit's immediate post-boot check passed with process exit code 0; it did not reproduce the first unit's initial failed DDC check.
- The immediate C8 probe passed, but the initial settings capture could not validate its VCP `0xD6` reply. A later settled capture read `D6 = 1`. Thus the successful post-boot process result does not mean every initial DDC read succeeded.
- Two identical, checksum-valid C3/0 replies identified the running application as `m-E5728UMDB-1005.0`. C8 replies before and after those queries were valid and retained privately.
- EDID and the settled queried settings matched before/after: brightness/contrast **100/50**, RGB **50/50/50**, preset **2**, raw input value **5**. This is not a complete configuration-EEPROM comparison.

## Windows/HDMI capture preparation — 2026-11-06

**No standby test was executed.** A passive Win32 listener and an awake OS snapshot identified the HDMI monitor at 3840×2160, 30 Hz. Display-off was never requested, so there is no sleep interval, flicker observation or wake result to evaluate.

The first started ETW session was stopped cleanly when its PowerShell 5.1 supervisor encountered a null `Process.ExitCode` from `Start-Process`. The supervisor was changed to use `System.Diagnostics.Process`; four dummy-process tests passed, covering exit codes 0/7 and large stdout/stderr. The subsequent corrected launch was cancelled at the Windows UAC prompt and did not start an ETW session. The passive preparation listener was then stopped cleanly. These are capture-tool preparation results, not HDMI firmware qualification.

A new capture requires the operator at the PC to accept UAC, observe standby and wake the displays manually. There is no comparable pre-upgrade Windows/HDMI capture. Any future HDMI result must be reported separately from DisplayPort qualification on this second unit; the firmware image remains unchanged.

The sanitized [capture tools and preparation results](../windows/standby/PREPARATION-RESULT.md) are now available. Subsequent offline review corrected descendant-process cleanup, argument quoting and readiness/shutdown record publication. At source commit `aa96a390ff8fc8fad586c6df2feae12e8df831a8`, the process and publication suites each passed eight cases per shell; publication tests covered 48 concurrent first-appearance reads in total. These are software checks and add no HDMI standby coverage.

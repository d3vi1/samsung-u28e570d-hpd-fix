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

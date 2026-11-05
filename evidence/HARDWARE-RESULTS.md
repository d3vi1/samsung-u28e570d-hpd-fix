# Hardware qualification log

Build: **1005.0-experimental.1**. This log separates programming/boot evidence from the still-pending DisplayPort standby test.

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

## DisplayPort standby/wake

**Pending.** Successful programming and a live version response do not demonstrate elimination of the standby loop or continuous AUX service.

The original Mac/TS4 passive trace used the other monitor. That trace is a useful symptom reference, but it is not a same-unit A/B comparison. The visual observations must also record brightness/contrast: the original trace used 100/50, while this unit was flashed at 0/0.

The next test reconnects the patched unit through the original TS4 native DisplayPort cable/path, captures passive host logs during controlled display sleep, and checks both sustained standby and subsequent wake. No repeated DDC/AUX polling is used during the sleep interval.

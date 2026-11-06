# Samsung package validation contract

The offline parser supports the observed `m-E5728UMDB` container layout. A Samsung BIN package and a complete SPI dump are separate input kinds. A dump must be exactly `0x200000` bytes; explicit extraction takes its package prefix using the header length and validates that prefix. Ordinary package validation rejects a complete dump or extra trailing bytes.

This version limits packages to `0x90000` bytes and expanded applications to 8 MiB. The package bound is a conservative supported-format limit, not a claim that the remaining flash is unused. A valid container is not permission to program it.

## Layout

All ranges below are half-open. Header words are little-endian unless stated otherwise.

| Field or range | Contract |
|---|---|
| `[0, 0x10000)` | Bootstrap; protected by image policy |
| `0x10000` | Main start, exactly `0x20080` |
| `0x10004` | Load address, exactly `0x200000` |
| `0x10008` | Load address plus `mainEnd - mainStart` |
| `0x1000C` | `mainEnd`, exclusive |
| `0x10010` | Zero flags word |
| `0x10014` | Main CRC32, little-endian |
| `0x10020` | Final CRC32 offset: `align8(mainEnd) + 8` |
| `0x10024` | CRC16 coverage start, exactly `0x10000` |
| `[0x10080, 0x20080)` | Complete PM51 slot; protected by image policy |
| `[0x20080, 0x23880)` | Uncompressed loader head |
| `0x211C7` | Three bytes `BE AD DE` |
| `0x211CA` | `mainEnd - 0x20080` |
| `0x211CE` | Uncompressed head size, exactly `0x3800` |
| `[0x23880, mainEnd - 6)` | Raw DEFLATE stream, without a zlib/gzip wrapper |
| `mainEnd - 6` | Expanded byte length, little-endian uint32 |
| `mainEnd - 2` | Inner CRC16, big-endian |
| `[mainEnd, align8(mainEnd))` | Zero padding |
| `align8(mainEnd)` | Six bytes `55 AA AB CD 00 00` |
| Final CRC offset minus 2 | Outer CRC16, big-endian |
| Final CRC offset | Whole-container CRC32, big-endian |

The file ends exactly four bytes after the final CRC offset. Bounds are checked before slicing or expansion.

## Integrity and DEFLATE

Both CRC algorithms are non-reflected, start at zero and use no final XOR. CRC32 uses polynomial `0x04C11DB7`; CRC16 uses `0x8005`.

- Main CRC32 covers `[0x20080, mainEnd)`.
- Whole-container CRC32 covers `[0, finalCrcOffset)`. The residue over the complete file must also be zero.
- Inner CRC16 covers `[0x10000, mainEnd - 2)`.
- Outer CRC16 covers `[0x10000, finalCrcOffset - 2)`.
- Both CRC16 calculations treat bytes `[0x10014, 0x10018)` as zero.

The parser supports stored, fixed-Huffman and dynamic-Huffman DEFLATE blocks. It checks code trees, repeat counts, back-references, the final block, exact consumed-byte boundary and exact expanded size. Truncation, unused bytes before the trailer and expansion beyond the declared length or 8 MiB are rejected. Unused bits in the last consumed compressed byte do not constitute extra bytes.

SHA256 values identify the complete package and expanded application. The service checksum is the sum of package bytes modulo 65536; it is supplementary evidence. The application version is extracted when the recognizable label exists; an absent label is reported as `unknown` and does not by itself invalidate the container.

## Policy and backup boundaries

Programming policy separately checks approved hashes and immutable bootstrap, PM51, header and loader bytes. Package overlay clones the verified full source image and replaces only the package prefix. Own-dump firmware restore also protects `[0x90000, 0x200000)`; it is not an arbitrary chip-restoration command. Sector erase/program operations restore complete sector images, including unchanged protected bytes that share a sector with a permitted header change.

SPI main-array backup does not establish a backup of separate configuration EEPROM, OTP/security storage or all calibration data. This application does not enable EEPROM coding or restoration.

## Offline conformance utility

Build `tests/package_conformance.cpp` against `monitorfw_core`, with `include` and `tests` on the include path. It performs no transport discovery or monitor access:

```text
package_conformance --self-test
package_conformance --package firmware.bin
package_conformance --dump own-spi-backup.bin
```

Self-tests contain only generated test data, including raw stored/fixed/dynamic streams. The fixed/dynamic vectors were generated with Python zlib and carry a known expanded SHA256. File mode emits JSON metadata for comparison with an independent validator and never rewrites its input. Keep real firmware, dump files and unit identifiers outside the public test fixtures.

Preparation checks passed 17 synthetic cases, including multiple stored blocks, a missing optional version label, an empty dynamic tree, truncation, extra compressed bytes, missing final blocks and incorrect expanded lengths. Local packages for 1002.0, 1003.1, 1004.5 and 1005.0-experimental.1 also matched the existing Python/zlib validator on all 12 compared length, hash and checksum fields. The older versions use an application length of 1,257,000 bytes; the newer two use 1,276,176. For this comparison only, the Python validator's originally fixed application-length constant was selected in memory for each known version; its source was unchanged. No firmware binaries or device dumps are included with these tests. These results establish format compatibility for those inputs, not arbitrary-input completeness or executable/hardware safety.

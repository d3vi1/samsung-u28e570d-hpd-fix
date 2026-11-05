# Samsung m-E5728UMDB 1004.5 container reconstruction

This investigation validates offline container construction. It does not qualify a functional firmware change or demonstrate bootloader execution on hardware. No monitor accesses occurred.

## Input identity and layout

Official BIN: 578,748 bytes, SHA-256 `60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b`.

| Field / region | Stock value | Meaning established by matching the binary |
|---|---|---|
| `0x10000` LE32 | `0x20080` | Main image flash start |
| `0x10004` LE32 | `0x200000` | Main load base |
| `0x10008` LE32 | `0x26D42A` | Load base plus stored main-image length |
| `0x1000C` LE32 | `0x8D4AA` | Main-image end, exclusive |
| `0x10014` LE32 | `0x9A36F923` | CRC32 of stored main image |
| `0x10020` LE32 | `0x8D4B8` | Final file CRC32 position; file length minus four |
| `0x10024` LE32 | `0x10000` | CRC16 coverage start, corroborated by matching CRCs |
| `0x10080..0x20080` | 64 KiB | PM51 slot, preserved |
| `0x20080..0x23880` | 14 KiB | Uncompressed main head |
| `0x211C7` three bytes | `BE AD DE` | Raw-DEFLATE compression marker |
| `0x211CA` LE32 | `0x6D42A` | Stored main-image length, also requiring update |
| `0x211CE` LE32 | `0x3800` | Uncompressed main-head length |
| `0x23880..0x8D4A4` | 433,188 bytes | Raw-DEFLATE application body |
| `0x8D4A4` LE32 | `0x137910` | Expanded body size |
| `0x8D4A8` BE16 | `0x95D5` | Inner CRC16 |
| `0x8D4AA..0x8D4B0` | six zero bytes | Padding to eight-byte boundary |
| `0x8D4B0..0x8D4B6` | `55 AA AB CD 00 00` | Outer trailer marker |
| `0x8D4B6` BE16 | `0xBBB7` | Outer CRC16 |
| `0x8D4B8` BE32 | `0x90B3850E` | Final file CRC32 |

Expanded body SHA-256 is `43d99a1e97a11735a95b5547fa0cbe297100753481c453876d970d747bc442d4`; body runtime base is `0x203800`. Its version text `m-E5728UMDB-1004.5` is at body offset `0x12CBD9`. This is a located display string, not proof that every update/version check is string-based.

## Integrity ordering

Both CRCs are MSB-first, initial zero, with no reflection or final XOR. CRC16 uses polynomial `0x8005`; CRC32 uses `0x04C11DB7`.

1. Update the three variable header lengths/pointers and the embedded compressed length.
2. Temporarily zero the main CRC32 field at `0x10014`.
3. Append the expanded-body LE32 length after the raw stream.
4. Compute inner CRC16 over `[0x10000, innerCRCposition)` and append it big-endian.
5. Pad to an eight-byte boundary and append `55 AA AB CD 00 00`.
6. Compute outer CRC16 over `[0x10000, outerCRCposition)` and append it big-endian.
7. Compute main CRC32 over `[0x20080, mainEnd)` and write it little-endian into `0x10014`.
8. Compute file CRC32 over the whole preceding container and append it big-endian.

Zeroing the header CRC32 during CRC16 computation is essential. Applying ordinary zlib CRC32, or computing the CRC16s after inserting the header CRC32, gives incorrect results. The service-menu checksum `0x48D0` equals the sum of all stock container bytes modulo 65536; it is separate from the four integrity fields.

The observed `0xBBB7` outer CRC remains constant in the three supplied firmware tail samples because the preceding inner CRC produces zero residue, followed by zeros and a fixed marker. Treating it as unexplained magic is unnecessary.

## Tool and validation

`container.py` pins the supported official input SHA-256, accepts only the original body length, validates every field above, and can rebuild the container from a caller-supplied modified body. It never modifies the supplied stock image.

```sh
python3 work/1005-packaging/container.py inspect work/samsung/stock-1004.5.bin
python3 work/1005-packaging/test_container.py
python3 work/1005-packaging/container.py repack work/samsung/stock-1004.5.bin PATCHED_APP OUTPUT.bin
```

Nine checks passed. Reusing the original compressed stream reconstructs the official image byte for byte. Recompression preserves the exact expanded body, bootstrap and PM51, and is deterministic with the tested zlib 1.2.12. Its unchanged-body container is 580,972 bytes, SHA-256 `10a017e52a2a160dbcb386d90ba1971649814d08c2bc3855b7737c2cdfda92de`.

The tool rejects growth past `0x90000`, the next 64 KiB boundary after the original container. This avoids extending into a new 64 KiB region, but is not proof of physical flash ownership or of a particular flasher's erase behavior. The razvan-pc owner must construct any full flash candidate against the exact current backup and preserve unrelated flash content. No personal full-flash backup belongs in a community patch distribution.

The four checksum matches, exact reconstruction and decompression establish format correctness. They do not establish that the old on-device decompressor accepts every stream emitted by the newer zlib library, nor that a modified application preserves AUX or HPD behavior. Those remain separate qualification gates.

## Corroborating public code

The implementation here is independently written from field matching; no source was copied. Public MStar code corroborates raw-DEFLATE use, the six-byte size/CRC trailer, CRC polynomials and CRC packaging order:

- <https://raw.githubusercontent.com/neuschaefer/mstar-mboot/master/sboot/src/MSDecompress.c>
- <https://raw.githubusercontent.com/Toysoft/mstar/master/Chakra3_017a0d6_20170731/scripts/BinIDPackFiles_Compress.py>

Those sources concern other MStar builds; local byte-for-byte matching is the evidence for this Samsung container.

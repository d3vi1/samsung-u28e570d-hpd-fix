# Published firmware package

`m-E5728UMDB-1005.0-experimental.1.bin` is the exact unofficial package used
for the documented experiment: 581,028 bytes, service sum16 `0x3899`.

Verify the file with `SHA256SUMS` (run from this directory):

```sh
shasum -a 256 -c SHA256SUMS
```

It is an update package, not a complete 2 MiB flash dump. The full target must
preserve unrelated bytes from the same physical monitor's verified backup.

`build-manifest.json` records offline construction; its `hardware_tested: false`
describes what the builder itself did. Subsequent hardware observations are in
[HARDWARE-RESULTS.md](../evidence/HARDWARE-RESULTS.md).

Samsung's code in this modified image remains Samsung's material. The project's
MIT license applies to the original tooling, patch code and documentation, not
to the underlying vendor firmware.

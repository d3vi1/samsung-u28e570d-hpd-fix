# Bounded loader / BSS check

Scope: determine whether the proposed stub at runtime `[0x2EB500, 0x2EB608)` is included in application expansion and whether CRT0 erases it. Static analysis only; no monitor operations.

Input `work/1005-build/bootstrap.bin` is the unchanged main head from official file offsets `[0x20080, 0x23880)`. It maps to runtime `[0x200000, 0x203800)`. Concatenating it with the existing expanded application creates `full-main.bin`; this was imported into a separate Ghidra project, `work/1005-packaging/loader-audit`, with `AEON:LE:32:R2` and base `0x200000`. The shared `app.bin` program was not changed.

The container's expanded application length is `0x137910`; therefore the application occupies `[0x203800, 0x33B110)`. The proposed cave is inside this body and included in the decompressed byte sequence.

CRT0 at `0x200100` performs cache/control initialization, then the following explicit memory clear:

```text
0x2001AC  movhi r3,0x33
0x2001B0  ori   r3,r3,0xB110   ; start = 0x33B110
0x2001B4  movhi r4,0x35
0x2001B8  ori   r4,r4,0xB460   ; end = 0x35B460
0x2001BC  sfleu r4,r3
0x2001BF  bf    0x2001CA       ; exit when start >= end
0x2001C2  sw    0(r3),r0
0x2001C5  addi  r3,r3,4
0x2001C8  j     0x2001BC
```

Thus CRT0 clears exactly `[0x33B110, 0x35B460)`, excluding the cave. It then initializes the stack and jumps to `0x264614` at `0x2001D5`. This is consistent with the previously inspected application entry that eventually calls `0x2409AE`.

Conclusion: the proposed cave is included in expansion and is not erased by this CRT0 zero loop. This check does not prove absence of all later application writes, dynamic references, or interrupt paths; those belong to the separate cave/control-flow review. It also does not execute the on-device bootloader's decompressor.

Full decoded bootstrap evidence: `work/1005-packaging/bootstrap-disasm.txt`.

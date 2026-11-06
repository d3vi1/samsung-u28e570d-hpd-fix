# macOS IOAV raw-I²C candidate

This is a static-analysis result from the investigation Mac, recorded on
2026-11-06. No I²C transfer, ISP entry, display-power change or flash write was
performed to obtain it. The application implements the resulting candidate
behind an explicit experimental read-only gate.

## Host ABI and raw addressing

The local userspace `IOAVServiceReadI2C` and `IOAVServiceWriteI2C` wrappers pass
two scalar arguments, the chip address and data address, to user-client selectors
24 and 25. They pass the payload unchanged and accept lengths up to 4096 bytes.
The read wrapper does not expose an independently meaningful actual-read count.

The examined `AppleDPTXController` implementation distinguishes
`dataAddress = UINT32_MAX` from ordinary offsets. This sentinel skips the
subaddress operation on both reads and writes. Offset zero instead requests a
literal zero subaddress. The chip address is passed unshifted; I²C direction is
encoded separately in the AUX operation.

The candidate raw mapping is therefore:

```text
chipAddress = 0x49                 # seven-bit MStar ISP address
dataAddress = 0xFFFFFFFF           # no-subaddress candidate
payload = complete outbound packet, including its first byte
```

Single-byte commands remain one-byte payloads. Splitting their first byte into
`dataAddress`, adding a dummy zero byte, or treating offset zero as raw mode
would describe different transactions in the examined local controller.

## Apple Silicon boundary

`DCPAVServiceProxy` forwards these scalars and the complete data buffer unchanged
to the DCP firmware. Its read and write IPC operations are 9 and 10. Static
dispatch inspection links the IOAV service interface to those proxy methods.
The proxy itself does not interpret the no-subaddress sentinel or generate the
physical I²C waveform.

The `AppleDPTXController` code and DCP proxy are distinct implementations. The
sentinel's local semantics establish a concrete candidate, but do not establish
the remote DCP firmware's behavior, the selected TS4 route, or the monitor's
response. Neither the wrapper nor the examined proxy supplies an independent
short-transfer count. Independent read passes and randomized receive-buffer
prefill can expose some failures, but cannot turn that limitation into a length
guarantee.

## Provenance and limits

The analyzed boot kernel Mach-O was extracted locally from the IMG4/LZFSE
container. Its size was 125059072 bytes and SHA-256 was
`ef4712e54102b35c11665c46fa7543a3fafc0c419723f583cb0ac891016f531a`.
Symbols and virtual addresses were resolved in the embedded fileset images.
The proprietary kernel binary and disassembly are not distributed in this
repository.

Useful anchors in that exact image:

| Anchor | Virtual address |
|---|---|
| IOAV user-client read handler | `0xfffffe000a9ec928` |
| IOAV user-client write handler | `0xfffffe000a9ec970` |
| DCP proxy read | `0xfffffe000a3c41cc` |
| DCP proxy write | `0xfffffe000a3c4364` |
| AppleDPTX read sentinel comparison | `0xfffffe000936cbc8` |
| AppleDPTX write sentinel comparison | `0xfffffe000936d2d8` |
| AppleDPTX subaddress helper | `0xfffffe000936ce94` |

These addresses and private APIs are build-specific. Their existence does not
qualify a public API, promise compatibility with another macOS release, or
authorize enabling the application's flash-write capability. Transport
qualification must establish the exact selected route, raw command framing,
payload limits, error handling and complete read integrity separately.

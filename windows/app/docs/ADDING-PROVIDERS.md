# Adding an I2C provider

The application separates the GPU transport from the MStar SPI bridge. NVIDIA is the only implemented provider in this development version. Intel and AMD are listed as unavailable; their entries do not probe a driver and do not fall back to NVIDIA. Hardware access remains disabled in the default GUI build.

`listProviders()` returns metadata. `createProvider(id)` creates a lazy object for the explicit IDs `nvidia`, `intel`, or `amd`. Neither operation loads a driver DLL, enumerates a GPU, reads EDID, or sends I2C. Only `II2cProvider::enumerate()` and `open(endpoint)` may initialize a driver or access hardware. An unavailable implementation throws a descriptive error from either method.

A new implementation must supply its own `II2cProvider` factory in the registry. Changing the `implemented` metadata flag is insufficient: the NVIDIA factory is selected only by the exact `nvidia` ID, and the other current entries create `UnavailableProvider` objects with no vendor delegation.

Each `DisplayEndpoint` carries its `providerId` and exact transport locator. An implementation must reject endpoints from another provider and revalidate the selected endpoint before returning a transport. The WinRT broker must also pass an explicit provider ID; provider selection is not permission to probe other GPUs.

## Required transport contract

`II2cTransport::capabilities()` returns metadata without I/O. The capabilities distinguish raw reads, separate writes followed by raw reads, and combined prefix/read transactions. They expose maximum read, write, prefix, and combined-read lengths. `validateI2cRequest()` is the common pure validator; new implementations must call it or enforce equivalent stricter bounds before reaching the driver.

All addresses are seven-bit addresses. `read(address, prefix, length)` returns exactly `length` bytes or throws. Empty-prefix reads are raw reads; nonempty prefixes require declared combined-read support. `write(address, bytes)` sends one bounded packet and throws on any transport error. There is no implicit retry: a failure may occur after the slave accepted a write. No method may silently set monitor controls, rewrite EDID, alter flash status registers, or reset the monitor. Destructors release local resources without monitor I/O.

`liveEdid()` must obtain the selected monitor's actual bytes and validate the transport's positional/repeated-read behavior. A successful driver return alone is insufficient evidence that the addressed slave answered. It must not substitute a cached EDID for a live check. `decodeMonitorEdid()` validates the reviewed SAM/U28E570 identity and marks missing, generic, or duplicate serials as ambiguous. `LU28E570DS/EN` is the reviewed firmware compatibility profile; the regional suffix is not established by EDID alone.

## MStar requirements

The MStar backend receives only `II2cTransport`; it has no GPU-vendor branches. Its current implementation requires separate writes/raw reads, a maximum read of at least 256 bytes, and a maximum write of at least 37 bytes. It rejects an insufficient contract before entry or any other transaction. Programming uses up to 32 data bytes in each bridge packet even though the physical SPI page size is 256 bytes. A new provider must not claim capabilities based solely on driver success codes.

The reviewed sequence is live identity check, `MSTAR` entry, SPI transactions framed by `10` / optional `11` and raw read / `12` cleanup. The backend attempts `12` even when the initial command failed. It does not retry a mutation or reset from a destructor. The common transaction engine owns backup, policy checks, retries at sector level, restoration, full verification, and permission to reset.

The flash profiles are EF4015, 2 MiB, 4096-byte sectors, and 256-byte hardware pages. A 16-byte SFDP prefix selects either the reviewed legacy two-register status profile or the reviewed JV three-register profile. All 64 SFDP bytes actually read are retained and compared. Do not invent the remaining bytes or assume every EF4015 chip has the same status-register layout. Status registers are read, not reprogrammed.

## NVIDIA implementation and compatibility

The NVIDIA transport uses the public V3 I2C ABI with pack 8 and cdecl entry points. Compile-time checks cover the native pointer-width layout: 44 bytes on x86 and 64 bytes on x64. It declares 4096-byte raw read/write limits, a four-byte prefix limit, and a conservative 16-byte combined-read limit. The original NVIDIA sample distinguishes a raw read address with low bit 1 from a combined read with low bit 0; prefix data may otherwise be ignored. No older structure fallback is attempted after a failure.

This application uses an experimental dynamic QueryInterface binding. NVIDIA documents the SDK static stub as the supported integration. Current NVIDIA documentation lists Windows 10 and later; a build targeting XP, 7, or 8 does not establish driver or hardware compatibility. Existing historical C# observations are not validation of the new C++ backend or other GPU providers.

- [NVIDIA I2C API documentation](https://docs.nvidia.com/nvapi/group__i2capi.html)
- [NVIDIA public ABI definitions](https://github.com/NVIDIA/nvapi/blob/main/nvapi.h)
- [NVIDIA interface IDs](https://github.com/NVIDIA/nvapi/blob/main/nvapi_interface.h)
- [NVIDIA I2C sample](https://github.com/NVIDIA/nvapi/blob/main/Sample_Code/i2c/i2c.cpp)
- [NVIDIA integration concepts](https://docs.nvidia.com/nvapi/concepts.html)
- [NVIDIA system requirements](https://docs.nvidia.com/nvapi/sysreq.html)

## Offline tests before hardware evaluation

Add a scripted transport that rejects unexpected packets and simulates errors before and after side effects. Test exact lengths, short reads, driver failures, capability limits, positional reads, ambiguous identity, cleanup after failure, and zero destructor/reset traffic. Registry tests may create any provider and inspect metadata; they must never enumerate or open the NVIDIA provider. Intel and AMD tests currently verify explicit unavailable errors only.

Build both pointer widths to exercise the native ABI assertions. Record the compiler and Windows versions actually tested. Any future hardware evaluation needs a separately authorized plan with an exact endpoint, a verified backup, recovery provisions, and evidence of the real transport's behavior. Passing the offline suite does not provide that evidence.

# Windows source-bundle transformations

This directory contains source code and documentation only. Active laboratory run sources were not edited while preparing this copy.

## Included files

```text
.gitignore
STAGING-NOTES.md
NvidiaMstarRead.cs
NvidiaMstarSpi.cs
experimental/README.md
experimental/FirmwarePackage.cs
experimental/FlashTransition.cs
experimental/ExperimentalPreparation.cs
experimental/NvidiaExperimentalAdapter.cs
experimental/Invoke-Experimental10050.ps1
experimental/Test-FirmwarePackage.ps1
experimental/Test-FlashTransition.ps1
experimental/Test-ExperimentalPreparation.ps1
```

## Transformations

- Initially copied the package validator, transition engine and simulator without functional changes. A subsequent repository-only engine correction revokes reset permission before final target/status verification on every path, including an identical-source/target transition. Two corresponding simulator regressions were added; active laboratory-run sources were not edited.
- Copied the generic NVIDIA reader without per-unit values or path substitutions; it contains no embedded monitor serial profile.
- Extracted `SetThreadExecutionState`, `SpiWriteCommand`, `WaitReady`, `EnableWrite`, `ReadRange` and `WriteSector` verbatim into `NvidiaMstarSpi.cs`, with the imports and partial-class wrapper they require. Historical per-unit hashes, serial profiles and the `Upgrade10045` entry point are excluded.
- Forced `HardwareEnabled=false` and replaced the selected device with `LOCAL_PROFILE_REQUIRED`. The policy also explicitly rejects that placeholder. Exact package, expanded-image, full-image and both plan hashes remain unchanged.
- Derived the mutex and recovery-marker names from the local-profile serial instead of retaining a private serial literal.
- Changed launcher and preparation-test dependencies to `NvidiaMstarSpi.cs`.
- Replaced private baseline/reference/fixture paths with `inputs/baseline-1004.5-full.bin`, `inputs/official-1004.5.bin` and `inputs/fixtures/{1002.0,1003.1}.bin`.
- Added a disabled-placeholder assertion to the preparation test. Its candidate input remains a locally generated artifact.
- Rewrote the Windows README for a laboratory source distribution, linked its upstream README as `../../README.md`, and described generated evidence without links to private run artifacts.
- Ignored local inputs, firmware, artifacts, upgrades, binary files and transient output. No firmware or hardware-session data is included.

Staging validation consists of compilation without constructing a NVIDIA object, running the in-memory simulator, checking that the extracted primitives are unchanged, checking retained pins, and scanning the tree for private identifiers and paths. Fixture-dependent parser/preparation suites require the excluded local inputs.

## Staging validation results

- All staged production sources compiled together under PowerShell 7.6.5. No NVIDIA instance was constructed; the disabled policy rejected entry.
- All 21 staged simulator cases passed under PowerShell 7.6.5 and Windows PowerShell 5.1, including no-op final-image corruption and final-status failure.
- The generic reader and parser match their source copies exactly. The engine and simulator differ only by the repository-only no-op reset-permission correction and its two regression cases. All six extracted SPI/execution-state members remain verbatim, and all seven artifact/plan SHA-256 pins are preserved.
- The 13-file tree contains no firmware binaries or private identifiers/paths. An independent read-only documentation/privacy review found no remaining issue after clarifying fixture prerequisites.
- Fixture-dependent parser/preparation suites were not run against this staged tree because their inputs are intentionally excluded.

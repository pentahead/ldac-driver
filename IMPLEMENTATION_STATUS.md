# Implementation status: BtA2dpSrc (independent Windows 11 LDAC/SBC A2DP source)

_Last updated: 2026-10-14. Everything below reflects actual build and test results in this repository._

## Summary
- **Implemented and verified offline:**
  - the complete user-mode stack: AVDTP source, LDAC (libldac) and SBC (libsbc) encoders, negotiation, LDAC→SBC fallback, real-time stream engine, service, IPC, CLI, configuration UI, per-device driver switcher, lifecycle scripts
  - 47/47 tests pass, 10/10 repeated runs
- **Verified on this machine's hardware (read-only):**
  - Bluetooth radio and paired-device enumeration, including which driver each headset's A2DP devnode uses
  - SetupAPI parsing of our driver INF against a real A2DP devnode (dry run)
  - WASAPI loopback capture at 96 kHz streamed through LDAC into the simulated sink for 22 s with 0 underruns
- **Not done:**
  - **Kernel L2CAP driver:** source written, but **unbuilt** (no WDK here) and untested
  - **Virtual audio endpoint driver:** evaluated, not implemented
  - **End-to-end playback on a real headset:** **unverified**

## Milestones

| # | Milestone | Status | Evidence |
|---|---|---|---|
| 1 | Build foundation | **Done** | `scripts\build.ps1` (CMake + MSVC 14.36, SDK 10.0.20348), pinned deps (`fetch-deps.ps1`), CI workflow; zero warnings at /W4 in project code |
| 2 | Audio endpoint | **Partial** | Kernel endpoint (ACX preferred, WaveRT fallback) evaluated and **not implemented**: no WDK, no way to test. Functional interim path: WASAPI loopback source, verified on hardware |
| 3 | Bluetooth integration | **Partial** | Discovery and SDP via documented APIs (verified). AVDTP over an `IL2capConnector` abstraction (verified with loopback). Kernel L2CAP proxy + IOCTL contract written, **unbuilt**. Constraint documented: no user-mode L2CAP API on Windows |
| 4 | Codec pipeline | **Done (offline)** | LDAC frames/bitrates verified for all rates, depths and EQMIDs; SBC round trip 59.9 dB SNR; selection, fallback and ABR wired; diagnostics in status |
| 5 | End-to-end audio | **Offline only** | Service + engine + real encoders + simulated sink, with a live loopback source; measurements in docs/TESTING.md. **Hardware playback unverified** |
| 6 | Configuration and packaging | **Done (dev)** | UI (screenshots verified), persistent validated settings, `install-dev`/`uninstall-dev` (rollback first), `bta2dp-switch` with journal, `package.ps1`; signing documented |

## Definition of Done

| # | Criterion | Status |
|---|---|---|
| 1 | Builds reproducibly | ✅ user mode (pinned deps, script, CI). ⚠️ driver: unbuilt locally |
| 2 | Endpoint appears in Windows | ❌ not implemented (loopback source used instead) |
| 3 | PCM processed without corruption | ✅ ring integrity test (2 M frames), SBC decode SNR, tone purity 1.0000 through the full pipeline |
| 4 | Device discovered and configured | ✅ discovery/SDP on real hardware. ⚠️ AVDTP configuration verified only against the simulator |
| 5 | LDAC negotiated, real LDAC frames transmitted | ✅ offline: real libldac frames validated by the sink simulator. ❌ over the air: unverified |
| 6 | SBC fallback works | ✅ 4 fallback paths tested (no LDAC, rejected configuration, init failure, mid-stream failure) |
| 7 | Status reflects runtime | ✅ codec reported only when packets are confirmed; fallback reason; tested |
| 8 | Failures handled without destabilising Windows | ✅ user mode: link loss, timeouts, encoder failure, reconnect tested. ⚠️ driver: needs Driver Verifier run |
| 9 | Tests, limitations, install/rollback docs | ✅ docs/, this file |

## Blockers and exact next actions
1. **Build the driver.** Pick one: install the WDK; run `scripts\fetch-wdk-nuget.ps1` (project-local); or push to GitHub (CI). Then fix compile errors. The code was written without a compiler.
2. **Test machine:**
   - enable test signing yourself (docs/SIGNING.md)
   - `install-dev.ps1 -Device <addr>` using a non-critical headset (do not use the SOUNDPEATS Engine 4, which is currently bound to the original AltA2DP driver)
   - `bta2dpctl connect`, then check `status.sinkCodecs`, `codecConfirmed` and audio
   - `uninstall-dev.ps1`
3. **Virtual endpoint (ACX):** implement after the transport is proven on hardware (docs/DRIVER.md).
4. **Legal (before any distribution):** Sony LDAC certification; Bluetooth SIG qualification; attestation signing.

## Known limitations
- No sink-initiated L2CAP channels in driver v1; the source always initiates.
- Kernel transport sends are synchronous per SDU (simple; adequate for about 200 packets/s).
- No AAC or aptX.
- Single active session.
- The loopback source requires audio to play on another endpoint.
- The original AltA2DP binaries were not modified. The SOUNDPEATS Engine 4 on this PC still uses AltA2DP (`oem44.inf`); nothing was installed, switched or loaded.

## Changes made to this PC during development
- `C:\ProgramData\BtA2dpSrc\` was created by service test runs: `settings.json`, `service.log`.
- The driver journal file is created only by a real `use`; no real switch was run.
- No services were registered, no drivers staged or installed, no boot settings changed.

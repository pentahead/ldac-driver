# Kernel components

## BtA2dpL2cap.sys (L2CAP proxy): source complete, NOT BUILT, NOT TESTED

| Item | Value |
|---|---|
| Model | KMDF 1.15, function driver for `BTHENUM\{0000110b-...}` (one remote sink) |
| Interfaces used | `GUID_BTHDDI_PROFILE_DRIVER_INTERFACE` (BthAllocateBrb/BthFreeBrb), `IOCTL_INTERNAL_BTH_SUBMIT_BRB` (BRB_L2CA_OPEN_CHANNEL / CLOSE_CHANNEL / ACL_TRANSFER), `IOCTL_INTERNAL_BTHENUM_GET_DEVINFO` |
| User contract | `include/bta2dp_ioctl.h` v1: GET_INFO, OPEN_CHANNEL, CLOSE_CHANNEL, SEND (METHOD_IN_DIRECT), RECEIVE (METHOD_OUT_DIRECT, pends; remote close = 0-byte completion), QUERY_CHANNEL |
| Security | Device SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)` (SYSTEM + Administrators). Fixed-size input structures are copied before use; MDL lengths are bounded |
| Cancellation | User-mode receive timeouts use `CancelIoEx`. The original request's cancel routine cancels the in-flight BRB child request (context and reference pattern) |
| v1 limitations | No sink-initiated L2CAP (ACCEPT returns `STATUS_NOT_SUPPORTED`), no flush-timeout configuration, max 4 channels per device |
| INF | `Class=Bluetooth`, `PnpLockdown`, DriverVer intentionally dated 2006 so the in-box driver wins rank ties. Installed only per device via `bta2dp-switch` |

Verified so far: SetupAPI parses the INF and matches it to a real A2DP sink devnode on this machine (`bta2dp-switch use <addr> src\driver\l2cap\BtA2dpL2cap.inf --dry-run`). The user-mode client (`KernelL2capConnector`) compiles and correctly reports "driver not installed".

### Validation plan (test machine only)
1. Build with the WDK. Fix compile errors (expect some: this code was written without a compiler).
2. Run Static Driver Verifier and CodeQL, and enable Driver Verifier (standard + IRQL + I/O verification) for `BtA2dpL2cap.sys`.
3. Enable test signing on a dedicated machine yourself (SIGNING.md), then run `install-dev.ps1 -Device <addr>`.
4. `bta2dpctl info` should list the driver instance with the right address. Then `bta2dpctl connect <addr>` and check `status`: `sinkCodecs` shows real capabilities, and `codecConfirmed` = true.
5. Run `uninstall-dev.ps1` and confirm the headset is back on `microsoft_bluetooth_a2dp_src.inf` with its normal Windows endpoint.

## Virtual audio endpoint: evaluated, NOT IMPLEMENTED

**ACX first (preferred).** ACX (Audio Class eXtensions, KMDF-based) is Microsoft's recommended model for new Windows 11 audio drivers. A *virtual* render circuit (no hardware) suits this project. A shared-memory ring would carry PCM to `bta2dp-service`, using the same `PcmRing` semantics (SPSC, overflow counted, never torn). Requirements: a WDK with ACX headers, Windows 11 with the ACX runtime, and HLK audio tests.

**Fallback: PortCls WaveRT** (the SYSVAD virtual-endpoint pattern). Use it if ACX availability on a target build is insufficient.

**Why it is not here:** without a WDK it cannot be compiled or exercised, and an untested audio miniport is high-risk (a system-wide audio glitch or crash surface). The functional interim path is the WASAPI loopback source, which has been verified on hardware.

**Integration contract when it lands:**
- a new `IPcmSource` implementation (`EndpointRingSource`)
- settings `source = "endpoint"` (already accepted; it currently reports "requires the BtA2dpAudio virtual endpoint driver")
- format negotiation limited to the codec's negotiated rate and bit depth

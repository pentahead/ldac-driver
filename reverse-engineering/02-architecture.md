# 02 — Architecture and Data Flow

Findings about the **original product only**. Recommendations are in [05-rebuild-plan.md](05-rebuild-plan.md).

## 1. Summary

AltA2DP is a **single monolithic KMDF + PortCls kernel driver**. It replaces Microsoft's `BthA2dp.sys` on a headset's `BTHENUM\{0000110B}` (A2DP Sink) devnode and does all of the following **in kernel mode**:

1. Exposes a WaveCyclic render endpoint and a Topology filter to the Windows audio stack.
2. Implements the **A2DP source role and AVDTP signalling itself** over L2CAP, using the documented **BthDdi profile-driver interface** (BRBs).
3. Runs the **codec encoders** (SBC, FDK-AAC, libldac, aptX/HD/LL) on a real-time-priority system thread with the FPU/SIMD state saved.
4. Sends media packets over the AVDTP media L2CAP channel with `BRB_L2CA_ACL_TRANSFER`.

User mode is only control plane: configuration UI, a small helper service, and an installer.

```mermaid
graph TB
  subgraph UM[User mode]
    APP[Apps] --> AE[Audio Engine audiodg.exe<br/>+ MS in-box SysFX APOs]
    CFG[AltA2dpConfig.exe]
    SVC[AltA2dpSVC.exe]
    INST[AltA2dpDriverInstaller.exe]
  end
  subgraph KM[Kernel mode: AltA2DP.sys]
    WAVE[WaveCyclic miniport<br/>16/24-bit PCM]
    TOPO[Topology miniport<br/>Volume/Mute/DAC nodes]
    PROP[KS property handlers<br/>{3DAF2019} + KSPROPSETID_BtAudio + Jack]
    TX[TX thread prio 22<br/>timer-paced]
    ENC[Encoders: SBC, FDK-AAC,<br/>libldac, aptX/HD/LL]
    ABR[ABR / buffer control]
    AVDTP[AVDTP signalling<br/>DISCOVER/GET_(ALL_)CAPS/SET_CONFIG/<br/>RECONFIG/OPEN/START/DELAY_REPORT]
    SDP[SDP client<br/>AVRCP 0x110E lookup]
    BRB[BthDdi BRB layer]
  end
  BT[BthEnum / BthPort / HCI]
  HS[(Headset A2DP Sink)]
  AE -- KS streaming --> WAVE --> TX
  TX --> ENC --> BRB
  ABR -.-> ENC
  AVDTP --> BRB
  SDP --> BRB
  BRB -- IOCTL_INTERNAL_BTH_SUBMIT_BRB --> BT --> HS
  CFG -- IOCTL_KS_PROPERTY --> PROP
  SVC -- IOCTL_KS_PROPERTY --> PROP
  CFG -. registry .-> KM
```

## 2. Kernel vs user responsibilities

| Responsibility | Location | Evidence | Status |
|---|---|---|---|
| Audio endpoint (WaveCyclic + Topology) | Kernel | `CLSID_PortWaveCyclic`, `CLSID_PortTopology`, `IID_IMiniportWaveCyclic`, `IID_IMiniportTopology` in `.rdata` (E-09); PortCls imports | VERIFIED HIGH |
| A2DP/AVDTP signalling | Kernel (custom, not Microsoft's) | Hand-built SET_CONFIGURATION PDU at `0x1400833A0` / `0x14009104B` (E-05); capability parser at `0x140087E40` (E-06) | VERIFIED HIGH |
| L2CAP transport | Kernel, via BthDdi | `GUID_BTHDDI_PROFILE_DRIVER_INTERFACE` ref at `0x14007C607`; `IOCTL_INTERNAL_BTH_SUBMIT_BRB` `0x410003` at `0x14008A9D7`; BRB type `0x105` (ACL_TRANSFER) at `0x14008B0BE` (E-03, E-04) | VERIFIED HIGH |
| PSMs | AVDTP `0x19`, AVCTP `0x17`, AVCTP-browse `0x1B` immediates | `0x140009E43`, `0x140009CCF`, `0x140005A1B` (E-04) | INFERRED MEDIUM (constants seen; call sites not fully traced) |
| SDP | Kernel | `GUID_BTHDDI_SDP_PARSE_INTERFACE` ref `0x14007C6D3`; AVRCP UUID `0x110E` (E-03) | VERIFIED (interface use); purpose INFERRED |
| Codec encoding | Kernel | libldac tables (E-07), aptX tables, FDK-AAC library-info strings (E-08), `KeSaveExtendedProcessorState` ×3 call sites | VERIFIED HIGH |
| Media packet pacing | Kernel system thread, priority 22 | `PsCreateSystemThread` `0x14007E0C0`; `KeSetPriorityThread(…,0x16)` `0x14008E8EA`; `ExSetTimer` (E-10) | VERIFIED HIGH |
| AVRCP / absolute volume | **Microsoft's** AVRCP stack (separate `BTHENUM\{110E}` devnode); AltA2DP reads `…\Bluetooth\Audio\AVRCP\CT\DisableAbsoluteVolume` and asks the service to restart the AVRCP devnode | E-12, E-13 | INFERRED MEDIUM |
| HFP / microphone (SCO) | **Not handled.** INF binds only `0x110B`. The driver only *observes* SCO (`ScoActive`) | E-01, E-11 | INFERRED MEDIUM |
| Configuration UI, licensing, diagnostics | User (Config) | E-14…E-17 | VERIFIED |
| Driver switching per device | User (Installer, elevated) | E-18 | VERIFIED |

## 3. Control-plane interfaces

### 3.1 Custom KS property set: `{3DAF2019-F75B-42D2-9103-4068BFE8C590}` (E-12)
- Sent with `IOCTL_KS_PROPERTY` (`0x2F0003`) by **both** `AltA2dpSVC.exe` (`0x140003E64`) and `AltA2dpConfig.exe` (`0x1401405DB`).
- Observed request: `KSPROPERTY{Set={3DAF2019…}, Id=1, Flags=KSPROPERTY_TYPE_GET}`; output buffer **12 bytes**. Content is UNKNOWN. A plausible reading (INFERRED LOW) is a BD_ADDR (8) plus flags/state (4), because the service then matches it against `DEVPKEY_Bluetooth_DeviceAddress`.
- Config issues a second `IOCTL_KS_PROPERTY` with an 0x58-byte input (`0x140119F5F`); its set/Id is UNKNOWN.
- The driver also uses the **same GUID as a device-interface property key**. At `0x140086E95` it calls `IoGetDeviceInterfacePropertyData(key={3DAF2019…})` with type `0x1003` (DEVPROP_TYPE_BINARY) over a multi-SZ interface list. This suggests instances publish state on their interfaces so they can discover each other (e.g. several paired headsets). Status INFERRED MEDIUM.
- Interface class enumerated by the service: `{F3A7ED1D-2D33-4733-AF84-279F8ADF2D13}` (`0x14000BFE8` in SVC). This is a private device interface registered by the driver (`IoRegisterDeviceInterface`). INFERRED HIGH.

### 3.2 Microsoft `KSPROPSETID_BtAudio` `{7FA06C40-B8F6-4C7E-8556-E8C33A12E54D}`
- Present in the driver (handler region near `0x14008A357`) and Config.
- Config calls `IKsControl::KsProperty(BtAudio, Id 0 = KSPROPERTY_ONESHOT_RECONNECT, GET)` at `0x14006ED1F` for the **Reconnect** button. This is the documented OS mechanism (VERIFIED HIGH).

### 3.3 Registry
- `HKLM\SYSTEM\CurrentControlSet\Services\AltA2DP\Parameters\Devices\<%016I64x BD_ADDR>\…`, with sub-keys `Capability` and `Current`. See 03 §6. The key path comes from strings `Devices`, `Capability`, `Current`, `%016I64x` (driver and Config) plus Config's `SYSTEM\CurrentControlSet\Services\AltA2DP\Parameters\Devices\Current`. INFERRED HIGH.
- `HKLM\SYSTEM\CurrentControlSet\Services\AltA2dpSVC\Parameters`: the driver holds the full path plus value `Trigger`; the service watches the key (`RegNotifyChangeKeyValue(…, subtree, LAST_SET|THREAD_AGNOSTIC)` at `0x140002366`). This is a **kernel → user signalling channel via the registry**. INFERRED HIGH.
- `HKCU\Software\Luculent Systems\Alternative A2DP Driver`: UI state (window placement, warning suppression).

### 3.4 Service behaviour (`AltA2dpSVC.exe`, VERIFIED from disassembly)
1. Creates a stop event, a waitable timer and a registry-change event, then loops on `WaitForMultipleObjects(3, …, 5000 ms)` (`0x1400022D2`).
2. On a registry change it re-arms the notification and debounces with the timer (`GetTickCount64`).
3. Work routine (`0x1400032B8`):
   - Enumerate interfaces `{F3A7ED1D…}` and query `{3DAF2019}` Id 1 on each.
   - Enumerate `BTHENUM` device IDs and select those prefixed `BTHENUM\{0000110E-…}` (AVRCP).
   - Read `DEVPKEY_Bluetooth_DeviceAddress` (`{2BD67D8B…},1`), parse it with `_wcstoui64`, and look it up in a `std::map`.
   - On a match: `CM_Disable_DevNode` → `Sleep` → `CM_Enable_DevNode` (`0x14000389F`–`0x1400038EF`).
4. Writes three status values back to its own Parameters key (`0x1400041F6`…`0x1400042D9`).

**Interpretation (INFERRED MEDIUM):** after a stream is (re)configured, the driver asks the service to restart Microsoft's AVRCP devnode for the same headset. This makes AVRCP and absolute volume re-bind consistently with the replaced A2DP driver.

### 3.5 Installer behaviour (VERIFIED imports and strings; flow INFERRED)
`SetupCopyOEMInfW(Driver\AltA2DP.inf)` → enumerate candidate drivers for the `BTHENUM` devnode → `DiInstallDevice` (picks AltA2DP **or** returns to BthA2dp: Config offers None/Windows/Alt/Other/LE Audio) → optionally toggle the BT radio (`Windows.Devices.Radios`, with the "DisableRadioToggleWarning" setting) and/or `IOCTL_BTH_DISCONNECT_DEVICE` (`0x41000C`, `0x140008577`) to force reconnection.

## 4. Sequence: connect → stream (evidence-backed where marked)

```mermaid
sequenceDiagram
  participant PnP as PnP / BthEnum
  participant D as AltA2DP.sys
  participant HS as Headset (SNK)
  participant AE as Audio Engine
  participant S as AltA2dpSVC
  PnP->>D: AddDevice on BTHENUM\{110B} (INF)
  D->>PnP: Query BthDdi profile + SDP interfaces [E-03]
  D->>D: Load per-device settings (Parameters\Devices\<addr>) [E-11]
  D->>HS: L2CAP connect PSM 0x19 (signalling) [E-04, MEDIUM]
  D->>HS: AVDTP DISCOVER (DiscoverDelay*/SnkDiscoverTO) [E-11, INFERRED]
  D->>HS: GET_ALL_CAPABILITIES or GET_CAPABILITIES (DisableGetAllCapabilities) [E-11]
  HS-->>D: SEP caps (SBC/AAC/vendor) parsed by codec table [E-06]
  D->>D: Write Capability masks; choose codec from 'Codec' + masks
  D->>HS: SET_CONFIGURATION (transport + codec + delay-report) [E-05]
  D->>HS: OPEN, then L2CAP media channel
  D->>PnP: PcRegisterSubdevice Wave/Topology; data ranges match codec rate/bit depth [E-09, INFERRED]
  D->>S: write Trigger (registry) [E-13]
  S->>D: KS {3DAF2019} Id1 GET
  S->>PnP: Disable/Enable BTHENUM\{110E} devnode
  AE->>D: Open pin, set format, RUN
  D->>HS: AVDTP START
  loop every TxPeriod (high-res timer, prio-22 thread)
    AE->>D: PCM into cyclic DMA buffer
    D->>D: Encode (FPU saved) and packetize
    D->>HS: BRB_L2CA_ACL_TRANSFER (media) [E-04]
    D->>D: ABR / TxTime statistics → Current\* [E-11]
  end
```

AVDTP signal IDs beyond SET_CONFIGURATION (0x03) are **not individually located** in code. Their use is inferred from option names (`UseAvdtpReconfigureCmd`, `DisableGetAllCapabilities`, `SnkDiscoverTO`) and the AVDTP specification.

## 5. Audio buffer, format and timing handling

| Aspect | Finding | Status |
|---|---|---|
| Port type | WaveCyclic (not WaveRT): the audio engine copies into a cyclic buffer that the driver drains | VERIFIED |
| Formats | 16 data ranges in **writable `.data`** at file `0xBC320`–`0xBC848`, each a fixed single rate. Placement in `.data` suggests the driver patches or limits ranges to the negotiated codec config ("Allow OS/app to choose bit resolution from 16/24 bit" in UI; `DisableFormatChangeEvent` option) | VERIFIED (ranges); INFERRED (runtime patching) |
| Channels | Max 2 (stereo); mono or dual-channel are codec modes, not PCM layouts | VERIFIED |
| Timing source | `KeQueryInterruptTimePrecise`, `ExAllocateTimer/ExSetTimer` (high-resolution), `KeSetTimer`, `KeWaitForMultipleObjects` | VERIFIED (imports) |
| TX scheduling knobs | `TxPeriod`, `TxSendAhead`, `TxMaxSize`, `TxMaxDuration`, `TxFlushTO`, `TxLatency`, `TxBuffer{Low,High,Critical}Threshold`, `TxTime{High,Critical}{Duration,Count}`; LL variants `LlTCL/LlICL/LlGWBL/LlBufferLevel/LlFlushTO/LlLatency/LlMaxSize` | VERIFIED (names); semantics INFERRED |
| Latency reporting | AVDTP Delay Reporting capability (`08 00`) is included in SET_CONFIGURATION; `Delay` status value | VERIFIED (capability) |
| Volume | Topology Volume/Mute nodes (`KSNODETYPE_VOLUME`, `KSNODETYPE_MUTE`); `VolumeLevel`, `VolumeInitializeDelay`; reads OS `DisableAbsoluteVolume` | VERIFIED (nodes and strings) |
| Jack / form factor | `KSPROPSETID_Jack`; `FormFactor` setting; Speaker/Headphones node types | VERIFIED (GUIDs and string) |

## 6. Open architecture questions
1. Exact AVDTP state machine and the handling of SUSPEND/ABORT and incoming (sink-initiated) connections.
2. Whether L2CAP channels are opened with enhanced retransmission/flush-timeout parameters (`TxFlushTO`/`LlFlushTO` suggest per-channel flush timeout via `BRB_L2CA_UPDATE_CHANNEL`, `0x106` immediate seen; INFERRED LOW).
3. Layout of the 12-byte `{3DAF2019}` Id 1 payload and the other property IDs.
4. How Config changes reach the running driver: registry polling, a notification, or a KS SET. The UI says bitrate changes take effect "when the next media packet is transmitted", so the driver must pick up changes live.

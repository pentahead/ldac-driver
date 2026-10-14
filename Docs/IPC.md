# Service IPC API (`\\.\pipe\BtA2dpSrc`)

The protocol is newline-delimited JSON. Each request is `{"cmd": ...}` and each response is `{"ok": bool, "error"?: string, ...}`.

| cmd | Arguments | Response fields |
|---|---|---|
| `status` | - | `status` (see below), `simulate` |
| `info` | - | `version`, `transport`, `ioctlVersion`, `encoders{SBC,LDAC}`, `radioPresent`, `radioName`, `driverInstances[]`, `driverStatus` |
| `devices` | - | `devices[]`: address, name, connected, a2dpSink, `driver{present,service,instanceId,started,problemCode}`, `transportReady` |
| `probe` | `address` | `sdp{found,l2capPsm,avdtpVersion,a2dpVersion,supportedFeatures,serviceName}` |
| `get_settings` | - | `settings` |
| `set_settings` | `settings{...}` (partial) | `settings` (saved), `warnings[]`. Rejected atomically on any invalid value |
| `connect` | `address`? (default: settings.deviceAddress) | `address` |
| `disconnect` | - | - |
| `logs` | `after`, `max` | `entries[]{seq,t,level,component,message}` |
| `endpoints` | - | `endpoints[]{id,name,default}` (WASAPI render) |
| `selftest` | - | `results[]` per encoder: ok, packets, codecFrames, kbps, nominalKbps, cpuMsPerSecondAudio |

## `status` object

| Field | Meaning |
|---|---|
| `state` | One of `idle`, `connecting`, `negotiating`, `streaming`, `suspended`, `error` |
| `device`, `transport` | Target device address and the transport in use |
| `sinkCodecs[]` | `{seid, codec, description, usable}`, filled once AVDTP capabilities are read |
| `configuration` | The negotiated configuration |
| `activeCodec`, `codecConfirmed` | Set only when media packets are actually flowing |
| `fallbackReason`, `lastError` | Why SBC was used instead of LDAC; the most recent error |
| `sinkDelayMs`, `sinkDelayReporting` | Delay report from the sink, and whether the sink supports delay reporting |
| `reconnects`, `restartRequired` | Reconnect count; whether changed settings need a reconnect to apply |
| `engine{...}` | packets, bytes, codecFrames, pcmFramesSent, silenceFramesInserted, droppedSourceFrames, underrunEvents, sendErrors, measuredKbps, nominalKbps, quality, encodeAvgUs, encodeMaxUs, sinkBufferMs, queueDepth, runtimeMs |

## Settings schema (`%ProgramData%\BtA2dpSrc\settings.json`)

| Key | Allowed values |
|---|---|
| `codec` | `auto`, `ldac`, `sbc` |
| `ldacQuality` | `abr`, `hq`, `sq`, `mq` |
| `sampleRate` | `0` (auto), `16000`, `32000`, `44100`, `48000`, `88200`, `96000` |
| `bitsPerSample` | `0`, `16`, `24` |
| `sbc` | `{maxBitpool 2..250, blocks 4/8/12/16, subbands 4/8, allocation loudness/snr}` |
| `sendAheadMs` | 20..300 |
| `source` | `loopback`, `tone`, `endpoint` |
| `sourceEndpointId` | WASAPI endpoint id |
| `deviceAddress` | Target device address |
| `logLevel` | Logging level |

Applying settings changes:
- **Live:** LDAC fixed quality (HQ/SQ/MQ) and SBC bitpool take effect without reconnecting.
- **Needs reconnect:** other changes set `restartRequired`.

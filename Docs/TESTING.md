# Testing

Run `.\scripts\build.ps1 -Test`, or `build\bin\Release\bta_tests.exe [--filter X] [--verbose]`.

## Test inventory (47 tests)

| Suite | What is verified |
|---|---|
| Json, Types, Settings | JSON parse/serialise/limits; BD_ADDR parsing; settings validation (invalid input rejected atomically); atomic save/load |
| CodecCaps | SBC/LDAC information-element parse/build (LDAC = vendor 0x12D, codec 0xAA, LOSC 10); negotiation: rate preference, source-rate match, 44.1k-family choice, joint stereo, bitpool limits, invalid ranges, 192 kHz rejection; single-value check for sink-chosen configuration |
| Avdtp | Header coding, fragmentation + reassembly at small MTU, malformed-sequence rejection, DISCOVER coding, capability strict/lenient parsing |
| Media | RTP sequence/timestamp/SSRC, payload header frame count, LDAC frame header (verified against real libldac output) |
| Pcm, PcmRing | Saturating conversions, channel remap/downmix, resampler (rate accuracy ±0.2 %, tone purity > 0.99), ring wrap/overflow, 2 M-frame concurrent producer/consumer integrity |
| SbcEncoder | libsbc encode → decode round trip: **59.9 dB SNR** at bitpool 53; frame and MTU limits; live bitpool change |
| LdacEncoder | Real libldac frames for 44.1/48/88.2/96 kHz, 16/24-bit, HQ/SQ/MQ: every packet's frames walk exactly; measured bitrate = nominal (990/660/330, 909, 606 kbps ±5 %); MTU < 679 rejected; live EQMID change |
| Session (integration) | Controller + AVDTP + engine + real encoders over loopback L2CAP into SimulatedSink: LDAC negotiated and streamed with 0 malformed / 0 sequence errors and real-time pacing; SBC when sink lacks LDAC (decoded tone purity 1.0000); SBC by preference; fallback when sink rejects LDAC; fallback when LDAC init fails; **fallback mid-stream** on LDAC encoder failure; AVDTP 1.2 sink (GET_CAPABILITIES); signalling timeout; remote SUSPEND/START; link loss + automatic reconnect; connect failure; live LDAC quality change and restart-required detection |
| Sdp, ServiceCore, Ipc | SDP record parsing; full command set in simulate mode; named-pipe round trip and shutdown |
| Hardware (read-only, skip if absent) | Bluetooth radio + paired-device enumeration with real driver binding; kernel transport reports "driver missing"; WASAPI loopback capture init |

Stability: the suite passed **10/10 consecutive runs** after fixing two concurrency defects that the reconnect test found. One was in the simulator (channel lifetime). The other was a real use-after-free of the media channel in `AvdtpSource` on link loss.

## Measurements (this machine, simulated sink)

| Scenario | Result |
|---|---|
| WASAPI loopback (live 96 kHz endpoint) → LDAC 96 kHz/24-bit HQ ABR, 22 s | 2,101,376 frames in 21.85 s runtime (real time + 40 ms send-ahead), 0 underruns, 0 dropped frames, 0 send errors, 1007 kbps on the wire |
| Encode cost per 128-frame call | LDAC 48k HQ ≈ 14 µs, LDAC 96k HQ ≈ 10 µs, SBC 48k ≈ 9 µs (≈0.4-0.7 % of one core); occasional 3 ms maxima from thread preemption |
| Service process CPU | Below `Get-Process` resolution (< 0.2 % of one core) |

## Not verified (needs hardware path)
Real headset playback, the real AVDTP exchange, latency at the ear, and RF interruptions all require `BtA2dpL2cap.sys` to be built, signed and switched onto a headset. See DRIVER.md.

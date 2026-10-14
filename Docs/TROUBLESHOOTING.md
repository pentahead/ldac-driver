# Troubleshooting

| Symptom | Cause / action |
|---|---|
| `service is not running (pipe not found)` | Start it: `sc start BtA2dpSrc`, or `bta2dp-service --console` |
| `BtA2dpL2cap driver not installed or not bound` | Expected until the driver is built, signed and switched onto the headset (DRIVER.md). Use `--simulate` to exercise everything else |
| Device listed as `needs BtA2dpL2cap` | That headset still uses `BthA2dp` (or another vendor driver such as AltA2DP). `bta2dp-switch list` shows the exact INF |
| Active codec shows `-` (not confirmed) | No media packets are flowing yet. Check `state` and `lastError` |
| SBC used, `fallbackReason` set | The reason is precise: sink lacks LDAC, sink rejected the configuration, encoder init failed, user preference, or an encoder error mid-stream |
| `restartRequired: true` | Sample rate, codec, bit depth or source changes need `disconnect` + `connect` |
| No sound from loopback source | Play audio on the endpoint chosen under "Capture from". Loopback captures what Windows renders there |
| Choppy audio | Raise `sendAheadMs`. Check `engine.underrunEvents`, `sendErrors`, `queueDepth`. LDAC ABR lowers quality automatically on congestion |
| Logs | `%ProgramData%\BtA2dpSrc\service.log`, `bta2dpctl logs`, or the UI log pane |
| Restore Windows audio for a headset | `bta2dp-switch restore <addr>`, or `uninstall-dev.ps1` |

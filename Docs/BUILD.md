# Build and development setup

## Requirements
- Windows 10/11 x64, Visual Studio 2022 ("Desktop development with C++"), Git. The bundled CMake is used automatically.
- Optional, for drivers only: Windows Driver Kit. See "Drivers" below.

## User-mode stack (verified in this repository)
```powershell
.\scripts\fetch-deps.ps1          # pinned libldac + libsbc (Apache-2.0)
.\scripts\build.ps1 -Config Release -Test
```
Outputs in `build\bin\Release`:

| Binary | Purpose |
|---|---|
| `bta2dp-service.exe` | Service (`--console [--simulate] [--verbose]`, `install`, `uninstall`) |
| `bta2dpctl.exe` | CLI (`status`, `info`, `devices`, `probe`, `connect`, `set k=v`, `simulate`, `local-devices`) |
| `bta2dp-config.exe` | Configuration UI |
| `bta2dp-switch.exe` | Per-device A2DP driver switcher (`list`, `drivers`, `use --dry-run`, `restore`) |
| `bta_tests.exe` | Unit and integration tests (`--filter`, `--verbose`) |

Notes on third-party build integration (upstream sources are not modified):
- libsbc uses C23 empty initialisers, which MSVC 14.36 rejects. CMake writes a build-tree copy of `sbc.c` with `{ }` changed to `{ 0 }`.
- `__builtin_clz` and `__attribute__` come from a force-included shim (`third_party/compat/msvc_builtins.h`).
- libldac's unused `<unistd.h>` is satisfied by an empty shim header.

## Try it without hardware
```powershell
build\bin\Release\bta2dpctl.exe simulate 5 auto      # LDAC end-to-end into the simulated sink
build\bin\Release\bta2dp-service.exe --console --simulate
build\bin\Release\bta2dp-config.exe                    # UI against the simulated service
```

## Drivers
The local environment used for this work has **no WDK**, so `src/driver` is unbuilt. Three ways to build it:
1. Install the WDK and its VS extension, then run `.\scripts\build-drivers.ps1 [-TestSign]`.
2. Run `.\scripts\fetch-wdk-nuget.ps1`. This is opt-in: the WDK NuGet packages are restored into `.\packages` with no system install. Then run `build-drivers.ps1`.
3. Push to GitHub. `.github/workflows/build.yml` builds and tests user mode and builds the driver if the runner has a WDK.

See DRIVER.md and SIGNING.md before installing anything.

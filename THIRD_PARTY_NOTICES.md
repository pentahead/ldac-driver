# Third-party components

| Component | Source (pinned) | Licence | Use |
|---|---|---|---|
| libldac (Sony) | android.googlesource.com/platform/external/libldac @ `eeee1a3f5f8df1282e3a6d297085885fd886737b` | Apache-2.0 | LDAC encoder (`src/ldacBT.c`, `src/ldaclib.c`) and ABR helper (`abr/`) |
| libsbc (Google) | github.com/google/libsbc @ `6e505650145c9973d08a0bdd5e5f5e1914305e40` | Apache-2.0 | SBC encoder; decoder used only by tests and the simulator |

## Required notices
- **libldac NOTICE (verbatim summary):** "Taking the certification process is required to use LDAC in your products", see https://www.sony.net/Products/LDAC/aosp/. Development and testing under Apache-2.0 are possible; **distributing a product that uses LDAC requires Sony's certification**. "LDAC" is a trademark of Sony Corporation, and this project is not affiliated with or endorsed by Sony.
- **Build-time modification (Apache-2.0 §4b):** for MSVC compatibility, the build system writes a copy of libsbc `src/sbc.c` in which the two C23 empty initialisers `{ }` are replaced by `{ 0 }`. Upstream files are unchanged. Force-included shims provide `__builtin_clz`, `__attribute__` and an empty `<unistd.h>`.
- Full licence texts: `third_party/libldac/LICENSE`, `third_party/libldac/NOTICE`, `third_party/libsbc/LICENSE`. `scripts/package.ps1` copies them into packages.

## Not included
- AAC and aptX encoders are not included (patent/licensing review pending).
- No code, data or resources from AltA2DP are used.

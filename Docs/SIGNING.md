# Signing, security and deployment

- **Production:** Windows 11 loads third-party kernel drivers only when they are Microsoft-signed. Get an EV code-signing certificate, then submit the package to the Partner Center for **attestation signing**, or WHQL (HLK) for broader distribution. The driver is built with `/INTEGRITYCHECK` and must pass HVCI/Memory Integrity.
- **Development:** test-sign with `build-drivers.ps1 -TestSign`, on a **dedicated test machine** where *you* enabled test signing:
  ```
  bcdedit /set testsigning on      (elevated; usually requires Secure Boot off; reboot)
  ```
  The project's scripts never change boot configuration, Secure Boot or signing policy. `install-dev.ps1` refuses to install an unsigned driver unless test signing is already on.
- **Codec licensing (product distribution):** see THIRD_PARTY_NOTICES.md. Shipping LDAC requires Sony's certification process (libldac NOTICE).
- **Service account:** `bta2dp-service` runs as LocalSystem because it opens the driver (SYSTEM/Administrators only).
- **IPC security:** the named pipe allows SYSTEM, Administrators and interactive users read/write (the UI runs unelevated) and rejects remote clients. All settings are validated and clamped; invalid input never changes the stored settings.

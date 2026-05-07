# Script Recipes

These scripts live under `scripts/` inside the skill.

## 1. Build host

Dry run:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\build-host.ps1 -DryRun
```

Normal build:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\build-host.ps1
```

Reconfigure only:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\build-host.ps1 -Reconfigure -ConfigureOnly
```

## 2. Build firmware

Dry run:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\build-firmware.ps1 -DryRun
```

Build:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\build-firmware.ps1
```

## 3. Upload firmware

Dry run:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\upload-firmware.ps1 -Port COM3 -DryRun
```

Upload:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\upload-firmware.ps1 -Port COM3
```

## 4. Serial smoke

This sends a blank wake-up and then `$$`, captures the reply, and writes a log file.

Dry run:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\serial-smoke.ps1 -Port COM3 -DryRun
```

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .codex\skills\grbl-host-firmware-loop\scripts\serial-smoke.ps1 -Port COM3
```

## Notes

- `serial-smoke.ps1` requires Python with `pyserial`.
- `upload-firmware.ps1` does not pre-build separately; it relies on `platformio run -t upload`.
- Default upload port is `COM3`, matching the current project notes. Override it when needed.

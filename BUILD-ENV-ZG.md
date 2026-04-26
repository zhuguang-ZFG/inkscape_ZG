# ZG Windows Build Environment

## First-time setup

1. Install MSYS2 to `C:\msys64`.
2. Run:

```bat
setup-zg-build-env.cmd
```

This script will:

- install the required MSYS2 `UCRT64` packages from `msys2-ucrt64-packages.txt`
- configure `inkscape\build-zg`
- generate `build.ninja` and `compile_commands.json`

If you also want a full MSYS2 package upgrade before installing build dependencies:

```bat
setup-zg-build-env.cmd --upgrade
```

## Daily usage

Normal build:

```bat
build-zg-inkscape.cmd
```

Force CMake reconfigure:

```bat
build-zg-inkscape.cmd --reconfigure
```

Only regenerate build files without compiling:

```bat
build-zg-inkscape.cmd --reconfigure --configure-only
```

Open a ready-to-build terminal:

```bat
start-zg-dev-shell.cmd
```

## Notes

- `build-zg-inkscape.cmd` now auto-runs CMake if `build.ninja` is missing.
- The build is pinned to the MSYS2 `UCRT64` toolchain.
- `compile_commands.json` is enabled during configure for editor tooling.

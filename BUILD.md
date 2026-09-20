# Building

[Back to TrueFPS](README.md)

For normal installation, download the plugin ZIP from the release page. These steps are for building the source.

## Requirements

- Windows with Visual Studio 2022 and its C++ tools.
- CMake 3.22 or newer.
- The Ashita v4 SDK folder containing `Ashita.h`.

## Build

From the project folder in Command Prompt:

```bat
set ASHITA4_SDK_PATH=C:\path\to\ashita-sdk
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

CMake builds for **32-bit x86** and writes `build\Release\truefps.dll`.

Fully close FFXI before replacing `/ashita/plugins/truefps.dll`, then relaunch and load the plugin.

## Release documentation

Edit the root `README.md` for GitHub. Both release workflows generate a plain Markdown `docs/truefps/README.md` inside the ZIP: badges become links, image headings become text, and feature dropdowns are expanded. The source README stays unchanged.

To preview the packaged README in PowerShell:

```powershell
./.github/scripts/export-readme.ps1 -Output build/README.release.md
```

The README is read from the revision being packaged. Documentation edits need to be included in that revision before preparing its release files; existing ZIPs do not update automatically.

## Development checks

In the development checkout, `tests\run.cmd` runs checks without the game. It also accepts FFXiMain dump files for signature checks. The public release checkout may omit the development tests; a successful compile does not verify gameplay behavior.

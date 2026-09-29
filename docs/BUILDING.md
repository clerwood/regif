# Building regif

For how the code fits together, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Requirements

- Windows 10 1809 or later, x64.
- Visual Studio 2026, or 2022 17.12 or later, with:
  - the **Desktop development with C++** workload, and
  - the **WinUI application development** workload, including **C++ WinUI app development
    tools** (an optional component; the C# tools alone aren't enough).
- [vcpkg](https://github.com/microsoft/vcpkg), which builds FFmpeg.

## One-time setup

```bat
git clone https://github.com/microsoft/vcpkg D:\dev\vcpkg
D:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
setx VCPKG_ROOT D:\dev\vcpkg
```

Restart Visual Studio (or your terminal) after `setx`. Without `VCPKG_ROOT`, the copy of vcpkg
bundled with Visual Studio is used when present.

The FFmpeg version is pinned by `builtin-baseline` in `vcpkg.json`. To move to newer ports, run
`%VCPKG_ROOT%\vcpkg x-update-baseline` and commit the change.

## Build and run

Open `regif.sln`, pick **Release | x64**, set **Regif** as the startup project and press F5. The
first build compiles FFmpeg (with FreeType and HarfBuzz for text) through vcpkg, which takes
10–30 minutes; after that it's cached.

From a Developer PowerShell:

```powershell
msbuild regif.sln -t:Restore -p:Configuration=Release -p:Platform=x64
msbuild regif.sln -p:Configuration=Release -p:Platform=x64 -m
.\build\bin\x64\Release\Regif.Core.Tests\Regif.Core.Tests.exe
.\build\bin\x64\Release\Regif.Media.Tests\Regif.Media.Tests.exe
.\build\bin\x64\Release\Regif\Regif.exe
```

Close regif before rebuilding: Windows locks a running `Regif.exe`, and linking fails with
LNK1104.

The app is unpackaged and self-contained: `build\bin\x64\Release\Regif` runs on any Windows 10/11
x64 PC without installing the Windows App SDK runtime.

The core library and its tests are portable and also build with GCC:

```sh
g++ -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Werror \
  -Isrc/Regif.Core/include src/Regif.Core/src/*.cpp tests/Regif.Core.Tests/*.cpp -o core-tests
./core-tests
```

## Continuous integration and releases

- **CI** (`.github/workflows/ci.yml`) runs on every push to `main` and on pull requests: the
  core tests on Linux with GCC, and a full Windows build with both test suites. The built app
  is attached to the run.
- **Release** (`.github/workflows/release.yml`) runs when a tag such as `v0.1.0` is pushed. It
  builds a portable zip and an installer (`regif-0.1.0-x64-setup.exe`, made with Inno Setup) and
  publishes both as a GitHub release. Tags with a hyphen, like `v0.2.0-beta.1`, become
  pre-releases.

To publish a release:

```bat
git tag v0.1.0
git push origin v0.1.0
```

When bumping the version, update `vcpkg.json` and `src/Regif/Regif.rc`. The installer and zip
take their version from the tag.

## Project layout

| Path | What it is |
| --- | --- |
| `src/Regif.Core` | Portable C++20: operations, validation, FFmpeg filter-graph planning, stages and sessions, text layer, editor geometry. No FFmpeg or Windows dependencies. |
| `src/Regif.Media` | FFmpeg backend: probing, rendering plans, preview frames, text rendering. |
| `src/Regif` | The WinUI 3 app. |
| `tests/` | `Regif.Core.Tests` (pure logic, runs anywhere) and `Regif.Media.Tests` (end-to-end FFmpeg checks on the clips in `tests/assets`). |
| `installer/` | Inno Setup script. |
| `docs/` | This file and the architecture notes. |

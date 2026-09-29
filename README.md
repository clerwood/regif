# regif

A Windows-native video-to-GIF converter and GIF editor, written in C++ with WinUI 3 and FFmpeg.

**Videos:** crop, trim, cut out a section, convert to GIF.
**GIFs:** crop, trim, cut out a section, change speed, optimize (colors, dithering, frame rate, size).

## Staged, non-destructive editing

regif never writes to the file you open. Every change you apply is rendered into a new
**stage**, and that stage becomes the starting point for the next change:

```
original.mp4 ─► stage 1: crop ─► stage 2: trim ─► stage 3: convert to GIF ─► Export…
 (read-only)
```

Undo, redo or click any entry in the History list to go back to an earlier stage; applying a
change from there replaces the stages after it. Only **Export** writes a file where you choose,
and it refuses to overwrite the original.

Stages live in `%LOCALAPPDATA%\regif\sessions\` and are deleted when you close the window
(leftovers from a crash are cleaned up after 7 days). Video stages are stored losslessly
(FFV1 in Matroska) so repeated edits don't lose quality; GIF stages are plain GIFs.

## Building

### Requirements

- Windows 10 1809 or later (Windows 11 recommended for the Mica backdrop).
- Visual Studio 2026 (or 2022 17.12+) with the **Desktop development with C++** workload and the
  **WinUI application development** workload (C++ WinUI app tools).
- [vcpkg](https://github.com/microsoft/vcpkg) for FFmpeg.

### One-time setup

```bat
git clone https://github.com/microsoft/vcpkg D:\dev\vcpkg
D:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
setx VCPKG_ROOT D:\dev\vcpkg
```

Restart Visual Studio after `setx` so it sees the variable. (If `VCPKG_ROOT` isn't set, the
copy of vcpkg bundled with Visual Studio is used when present.)

Recommended: pin the FFmpeg version so every build uses the same one, then commit `vcpkg.json`:

```bat
cd D:\regif
%VCPKG_ROOT%\vcpkg x-update-baseline --add-initial-baseline
```

### Build and run

Open `regif.sln`, pick **Release | x64** (or Debug), set **Regif** as the startup project, and
press F5. The first build compiles FFmpeg through vcpkg, which takes 15–30 minutes; after that
it's cached.

From a Developer PowerShell:

```powershell
msbuild regif.sln -t:Restore -p:Configuration=Release -p:Platform=x64
msbuild regif.sln -p:Configuration=Release -p:Platform=x64 -m
.\build\bin\x64\Release\Regif.Core.Tests\Regif.Core.Tests.exe
.\build\bin\x64\Release\Regif.Media.Tests\Regif.Media.Tests.exe
.\build\bin\x64\Release\Regif\Regif.exe
```

The app is unpackaged and self-contained: the folder `build\bin\x64\Release\Regif` runs on
any Windows 10/11 x64 PC without installing the Windows App SDK runtime.

## Continuous integration and releases

- **CI** (`.github/workflows/ci.yml`) runs on every push to `main` and every pull request: the
  core tests on Linux with GCC, and a full Windows build with both test suites. The built app
  is attached to the run as an artifact.
- **Release** (`.github/workflows/release.yml`) runs when you push a tag such as `v0.1.0`. It
  builds a portable zip and an installer (`regif-0.1.0-x64-setup.exe`, made with Inno Setup)
  and publishes both as a GitHub release. Tags with a hyphen (`v0.2.0-beta.1`) become
  pre-releases.

### Publishing to GitHub

Create an empty repository named `regif` on GitHub (no README or license, since this project
has them), then:

```bat
cd D:\regif
git init
git add .
git commit -m "Initial commit"
git branch -M main
git remote add origin https://github.com/<your-user>/regif.git
git push -u origin main
```

To publish a release:

```bat
git tag v0.1.0
git push origin v0.1.0
```

When bumping the version, update `vcpkg.json` and `src/Regif/Regif.rc` (the installer and zip
take their version from the tag).

## Project layout

| Path | What it is |
| --- | --- |
| `src/Regif.Core` | Portable C++20: operations, validation, FFmpeg filter-graph planning, stage/session management. No FFmpeg or Windows dependencies. |
| `src/Regif.Media` | FFmpeg backend: probing, rendering a plan, grabbing preview frames. |
| `src/Regif` | The WinUI 3 app. |
| `tests/` | `Regif.Core.Tests` (pure logic, runs anywhere) and `Regif.Media.Tests` (end-to-end FFmpeg checks on the clips in `tests/assets`). |
| `installer/` | Inno Setup script. |
| `docs/ARCHITECTURE.md` | How it fits together, and the roadmap. |

## Current limitations

- No audio: GIFs have none, and video stages drop the audio track.
- Exporting a video stage (for example a trimmed clip before converting) writes lossless FFV1
  Matroska (`.mkv`). It plays in VLC and mpv but not in every player, and it's large. MP4
  export is on the roadmap. Exporting the original (stage 1) writes an exact copy.
- Video preview is a still frame you scrub on the timeline rather than playback. GIFs animate.
- Cutting out a middle section is entered as numbers; the timeline handles only trim the ends.
- AV1 videos need FFmpeg's `dav1d` feature, which isn't enabled in `vcpkg.json` yet.
- Mirrored (flipped) phone videos are shown rotated but not un-mirrored.

## License

MIT (see `LICENSE`). Release builds include FFmpeg under the LGPL; see `THIRD_PARTY_NOTICES.md`.

# Third-party notices

regif itself is MIT licensed (see `LICENSE`). Release builds also contain the following components.

## FFmpeg

regif uses the FFmpeg libraries (libavcodec, libavformat, libavfilter, libavutil, libswscale)
under the GNU Lesser General Public License, version 2.1 or later. FFmpeg is built by vcpkg
without the `gpl` or `nonfree` features and is **dynamically linked**: the `av*.dll` and
`sw*.dll` files next to `Regif.exe` can be replaced with compatible builds.

- Project: https://ffmpeg.org
- License: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
- Source: the exact FFmpeg source used for a release is the version pinned by vcpkg for that
  release's commit (see `vcpkg.json` and its `builtin-baseline`), available from
  https://github.com/microsoft/vcpkg/tree/master/ports/ffmpeg and https://ffmpeg.org/download.html.

If you redistribute regif, keep this notice and make the corresponding FFmpeg source available.

## Text rendering libraries

FFmpeg's `drawtext` filter (used to draw text overlays) brings in these libraries, shipped as
DLLs next to `Regif.exe`. Versions are those pinned by the vcpkg baseline in `vcpkg.json`.

- **FreeType** (`freetype.dll`): used under the FreeType License (FTL), not the GPL option.
  Portions of this software are copyright © The FreeType Project (https://freetype.org).
  All rights reserved.
- **HarfBuzz** (`harfbuzz*.dll`): MIT license. https://github.com/harfbuzz/harfbuzz
- **zlib** (`z.dll`): zlib license. https://zlib.net
- **libpng** (`libpng16.dll`): PNG Reference Library License version 2. http://www.libpng.org
- **bzip2** (`bz2.dll`): bzip2 license (BSD-style). https://sourceware.org/bzip2/
- **Brotli** (`brotli*.dll`): MIT license. https://github.com/google/brotli

Each library's full license text is in its source distribution and in vcpkg's
`installed/<triplet>/share/<port>/copyright`.

## Windows App SDK and C++/WinRT

The Windows App SDK runtime files shipped with the self-contained build are distributed under
Microsoft's license terms for the Windows App SDK (https://aka.ms/windowsappsdk/license).
C++/WinRT is MIT licensed (https://github.com/microsoft/cppwinrt).

# regif

Turn videos into GIFs, edit GIFs, and add titles and captions, on Windows.

> **Pre-release.** regif is new and still rough in places. Please
> [report anything odd](https://github.com/clerwood/regif/issues).

## Features

- **Video to GIF**: pick the frame rate, size, number of colours and dithering, and whether it loops.
- **Crop** by dragging a box on the picture, with an optional aspect ratio (1:1, 4:3, 16:9, 9:16…).
- **Trim** with handles on the timeline, or **cut out** a section from the middle.
- **Edit GIFs**: change their speed, and make them smaller by reducing colours, frames or size.
- **Text**: add titles and subtitles on tracks in the timeline. Drag them into place on the
  picture, double-click to type, and pick any installed font, size, alignment, fill, outline,
  background box, shadow and opacity. The preview shows text exactly as it will be exported.
- **Quick captions**: type a phrase and regif spreads the words evenly across the clip.
- **Timeline** with thumbnails. Scrub, play from any point with Space, and snap text clips next
  to each other.
- **Your original file is never changed.** Every edit is kept as a step you can undo, redo or
  jump back to, and nothing is saved until you export.

## Install

regif needs **Windows 10 (version 1809 or later) or Windows 11, 64-bit**.

1. Go to the [latest release](https://github.com/clerwood/regif/releases) and download one of:
   - **`regif-…-x64-setup.exe`**, an installer. By default it installs just for you, without
     needing administrator rights.
   - **`regif-…-x64-portable.zip`**, if you'd rather not install anything. Unzip it anywhere and
     run `Regif.exe`.
2. Pre-releases aren't code-signed yet, so Windows may show **"Windows protected your PC"**.
   Choose **More info**, then **Run anyway**.

To uninstall, find regif in **Settings > Apps**. To remove the portable version, delete its folder.

## Getting started

1. **Open** a video or GIF (Ctrl+O), or drag it onto the window.
2. Make your changes in the panel on the right. Each one you apply appears in **History**.
3. For a video, choose **Convert to GIF** once the clip is the way you want it.
4. **Export** (Ctrl+S) to save the result wherever you like.

## Keyboard shortcuts

| Keys | Action |
| --- | --- |
| Ctrl+O | Open |
| Ctrl+S | Export |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Space | Play or pause from the playhead |
| ← / → | One frame back / forward (after clicking the timeline) |
| PageUp / PageDown | One second back / forward (after clicking the timeline) |
| Home / End | Jump to the start / end (after clicking the timeline) |
| I / O | Set the trim start / end at the playhead (after clicking the timeline) |
| Double-click text | Edit it in place, on the picture or on its track |
| Esc | Finish editing text |
| Alt while dragging a text clip | Don't snap to its neighbours |

## Good to know

- There's **no sound**. GIFs don't have any, and regif doesn't keep the audio of videos yet.
- Export saves videos (before converting to GIF) as `.mkv`. These play in VLC and most modern
  players, but they're large, so converting to GIF is usually the last step.
- Playback may skip frames on very large videos, such as 4K.
- Text isn't part of undo and redo. Edit or delete it directly.
- A few fonts aren't offered, and bold or italic only work when the font includes them.
- AV1 videos can't be opened yet.
- Everything happens on your PC. regif doesn't use the internet.

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md).

## License

regif is released under the MIT license (see [LICENSE](LICENSE)). It includes FFmpeg and other
open-source libraries under their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

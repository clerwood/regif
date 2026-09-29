# regif architecture

## Layers

```
Regif (WinUI 3 app, C++/WinRT)
   │  UI, pickers, preview, threading
   ▼
Regif.Core (portable C++20)            Regif.Media (FFmpeg)
   Operations, validation      ◄────   FfmpegProcessor : IMediaProcessor
   RenderPlan (filter graphs)          FrameGrabber (preview frames)
   Session (stages, undo/redo, export)
```

`Regif.Core` knows nothing about FFmpeg's C API or Windows. It turns an operation plus a
description of the current stage into a `RenderPlan`: an FFmpeg filter-graph string, an
encoder, a container and their options. `Session` drives an `IMediaProcessor` to render plans
into stage files. Keeping this layer pure means the interesting logic (validation, graph
construction, staging rules) is unit-tested in milliseconds on any OS, including the Linux CI job.

`Regif.Media` implements `IMediaProcessor` with libavformat, libavcodec and libavfilter:
decode, push frames through the plan's graph, encode, mux. It also provides `FrameGrabber` for
the scrubber.

## Staging model

- Stage 0 is the original file. It is only ever opened for reading, never copied or moved.
- Applying an operation renders `stage-NNN.partial.<ext>` in the session folder and renames it
  to `stage-NNN.<ext>` only after the render succeeds. A failed or cancelled render leaves the
  session exactly as it was.
- The new stage becomes current. If the user had undone some stages, those "redo" stages are
  deleted, but only after the new render succeeded.
- Video stages use FFV1 (level 3) in Matroska with 4:4:4 chroma, so chains of edits never
  degrade. GIF stages are GIFs, because re-quantizing a GIF from a lossless intermediate would
  change its palette.
- `exportCurrent` copies the current stage to a user-chosen path and refuses to write over the
  original (checked with `std::filesystem::equivalent`, falling back to comparing normalized
  absolute paths).
- Session folders are `%LOCALAPPDATA%\regif\sessions\session-<epoch>-<random>`. The destructor
  removes its folder; at start-up, folders older than 7 days (from crashes) are removed. Only
  folders with the `session-` prefix are ever deleted.

## Render plans

Each plan's graph has one unlabeled input and output, and the renderer connects them to a
`buffer` source and a `buffersink`. Typical graphs (all exercised against the real `ffmpeg`
CLI during development).

Video-stage graphs are `<rotation>, format=yuv444p, <operation>`; GIF-stage graphs are
`format=bgra, <operation>, <palette>`; converting a video is
`<rotation>, fps=N, scale=W:-1:flags=lanczos, <palette>`. The operation parts:

| Operation | Filters |
| --- | --- |
| Crop | `crop=w:h:x:y` |
| Trim | `trim=start=…:end=…, setpts=PTS-STARTPTS` |
| Cut | `split`, two `trim`s, `concat=n=2` (or a single `trim` when the cut touches an end) |
| Speed (GIF) | `setpts=PTS/F`, plus `fps=50` when the result would exceed 50 fps (browsers slow down faster GIFs) |
| Optimize (GIF) | optional `fps=…` and `scale=…`, with a smaller palette |

`<palette>` is `split[a][b]; [b]palettegen=max_colors=N:stats_mode=full|diff|single[p];
[a][p]paletteuse=dither=…:diff_mode=rectangle[:new=1]`. `diff_mode=rectangle` lets the GIF
encoder write only changed regions.

Rotation: phone videos store a display matrix instead of rotated pixels.
`av_display_rotation_get` returns the counter-clockwise angle; regif converts it to a
clockwise `rotationDegrees` and bakes it in with `transpose`/`hflip,vflip` on the first edit.
Stage files therefore never carry rotation metadata.

### Time bases (an important detail)

The encoder uses the **buffersink's time base**, not `1/fps`. Speeding up a GIF compresses
timestamps (for example to 1.5 centisecond steps); an encoder at `1/fps` would round
neighboring frames onto the same tick and drop them. With the sink time base every frame
survives (verified: 45 of 45 frames kept versus 21 with `1/fps`).

## Threading and UI

- All FFmpeg work (open, render, export, preview frames) runs on the thread pool via
  `co_await winrt::resume_background()`, and results come back with a small `ResumeOn`
  awaiter on the window's `DispatcherQueue`. If the window is gone, `ResumeOn` reports it and
  the coroutine stops without touching XAML.
- Only one render runs at a time (`m_busy`). Progress is posted to the UI thread at most once
  per percent. Cancel sets a `CancellationToken` that the renderer checks between packets and
  frames.
- Preview frames are coalesced: while one frame decodes, newer slider positions replace the
  pending request, so dragging never builds a backlog. A generation counter discards results
  from a stage that is no longer shown.
- GIF stages are shown as an animated `BitmapImage` loaded from memory (so the file stays
  unlocked). Scrubbing a GIF shows still frames from `FrameGrabber`; Play resumes the animation.
- The crop overlay and the timeline are drawn on `InteractiveSurface` controls (a UserControl
  that can set the mouse cursor). Their geometry (hit testing, aspect-locked dragging, rounding
  to a `CropOp`, thumbnail times) lives in `Regif.Core`'s `EditorGeometry` and is unit-tested.
- Timeline stills come from a second `FrameGrabber` on a background thread
  (`grabSequence`, one decoding pass for GIFs), cancelled when the stage changes, so they never
  hold up the preview frame.

## Testing

- `Regif.Core.Tests`: operations, validation messages, exact filter graphs, and session rules
  (staging, redo pruning, failure and cancellation, export protection, stale cleanup) using a
  fake processor. Runs on Windows (MSVC) and Linux (GCC) in CI.
- `Regif.Media.Tests`: runs real renders on tiny generated clips in `tests/assets` and checks
  sizes, durations, frame counts, rotation, cancellation and frame grabbing.

## Roadmap

- Timeline handles for cutting out a middle section.
- Video playback in the preview.
- MP4 (H.264 via Media Foundation, `h264_mf`) export for video stages.
- Audio passthrough for video stages.
- Two-pass palette generation to avoid buffering every frame for long clips; a lighter
  intermediate than FFV1 for long 1080p+ sources.
- Lossless GIF speed changes by rewriting frame delays instead of re-encoding.
- Lossy GIF optimization (gifsicle-style or libimagequant). Check licenses first: gifsicle is
  GPL and libimagequant is GPL or commercial, which would change how regif can be distributed.
- `dav1d` for AV1 input.
- ARM64 builds in CI, MSIX packaging and code signing.
- Trimming unused Windows App SDK components from the self-contained output.
- File associations and "Open with regif" in Explorer.

# Test assets

Tiny clips used by `Regif.Media.Tests`, generated from FFmpeg's built-in `testsrc2` pattern
(no third-party content):

```
ffmpeg -f lavfi -i "testsrc2=size=160x90:rate=30:duration=2" -c:v libx264 -pix_fmt yuv420p -crf 30 clip.mp4
ffmpeg -display_rotation 90 -i clip.mp4 -c copy rotated.mp4
ffmpeg -f lavfi -i "testsrc2=size=120x68:rate=15:duration=2" -vf "split[a][b];[b]palettegen=max_colors=64[p];[a][p]paletteuse" -loop 0 clip.gif
```

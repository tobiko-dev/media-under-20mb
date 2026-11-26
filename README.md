# <img src="assets/under20.png" width="42" height="42" alt=""> Under 20

Drop audio or video onto Under 20. It saves a copy **under 20 MB** beside each original, ready to attach or share.

**[Download for Windows x64 · 551 KB ZIP](https://github.com/tobiko-dev/media-under-20mb/releases/download/v1.0.0/Under20-Windows.zip)**

Extract the ZIP, open `Under20.exe`, and drop in your files. It uses your installed FFmpeg. Select a finished row and click **Open file location** to jump to the copy.

## A batch in action

[![The Windows app processes three files. A 127.34 MB video becomes a 19.47 MB MP4, a 24 MB WAV becomes a 1.69 MB lossless FLAC, and a small chime is skipped.](assets/windows-demo.gif)](assets/screenshots/finished-queue.png)

Two files saved, one skipped. Turn on **Keep WAV** before dropping audio if you need a WAV copy. Under 20 asks before shrinking the picture or removing tracks.

## Before and after

[![The same rotating ribbon plays side by side. The original MKV is 22.83 MB and the compressed MP4 is 19.74 MB.](assets/ribbon-comparison.webp)](assets/screenshots/ribbon-frame.png)

Both files keep all 120 frames at 800 × 450 and 30 fps. Click the comparison to inspect the same frame at full resolution.

[Technical details, setup and build instructions](TECHNICAL_DETAILS.md)

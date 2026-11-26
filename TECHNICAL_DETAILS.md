# Under 20 technical details

## Run the portable app

Extract the Windows x64 ZIP and open `Under20.exe`. The app requires a local FFmpeg build with `ffmpeg.exe`, `ffprobe.exe`, and the H.264 (`libx264`), AAC and FLAC encoders. It looks beside the executable, in its `bin` folder, on `PATH`, and in common installation directories. **Locate FFmpeg** lets you select a custom installation. If FFprobe is installed separately, the app can remember its path too. [FFmpeg downloads](https://ffmpeg.org/download.html)

The queue accepts multiple files from Explorer, including files dropped on the executable. Clicking the drop zone opens a multi-file picker. FFprobe identifies the streams inside each file. Supported inputs include MP4, MKV, MOV, WebM, AVI, WAV, FLAC, MP3, M4A, AAC, OGG and Opus, subject to the decoders installed on the machine. Probing and encoding use local files.

## How a copy is chosen

The size limit is **20,000,000 bytes**. A readable media file at 19,999,999 bytes skips. A file at exactly 20,000,000 bytes enters the compressor. The queue displays decimal MB and reports the exact output byte count for a finished copy.

| Source or option | What the compressor tries |
| --- | --- |
| One eligible integer PCM, FLAC or ALAC audio track, up to 24 bits | Try lossless FLAC. If the complete result exceeds the limit, try AAC in M4A. |
| Compatible H.264 video and AAC audio | When the measured stream payload could fit, try rewrapping the original streams first. |
| Video requiring re-encoding | Encode H.264 in two passes. Keep compatible AAC when it fits its share of the budget. Otherwise encode AAC. MP4 is the usual container. |
| Additional audio, subtitles or file attachments | Map the tracks explicitly. MP4 can carry text subtitles. Matroska retains styled or graphic subtitle streams and attachments such as fonts. |
| **Keep WAV** on an audio-only file | Write full-length PCM WAV. Choose a bit depth and sample rate that can fit with the retained channel count. The queue reports when a full-length WAV cannot fit. |

The encoder estimates the bitrate from the retained streams’ packet timelines. It allows room for container overhead, audio, subtitles and attachments. If an encoded candidate is too large, its actual byte count determines the next budget. Each plan gets up to four attempts. The app asks before using smaller video dimensions or removing extra audio, subtitles or attachments. Declining skips that file.

Video normally keeps its source dimensions, rotation, time base and frame timing, including variable frame rates. AAC encoding or **Keep WAV** may reduce the sample rate. A size-constrained H.264 or AAC encode can lose detail. Covers and general container metadata are outside the playback checks.

## Verification and output

Each job works in a temporary directory beside its input. Before publishing a candidate, the app checks its size and probes its track counts, dimensions, rotation, audio channels, relative start times and durations. It compares available video frame or sample counts and decodes all output audio and video through the end with FFmpeg. Codec delay and frame timing are allowed small tolerances. The app checks the byte count again before saving.

An input called `clip.mkv` might produce `clip-under-20MB.mp4`. If that name exists, the next copy becomes `clip-under-20MB (2).mp4`. On Windows, the final move uses no replace flag, including when two instances finish together. Canceling a running job stops its child process and cleans up that job’s temporary files.

## Test results

The README’s queue is a capture of the packaged executable running on Windows Server 2022. The demo launches the empty window, then opens a generated three-file batch through the same file-argument entry point Explorer uses for drops on the executable. [Windows demo run](https://github.com/tobiko-dev/media-under-20mb/actions/runs/45795876266) · [Capture script](tools/capture-windows.ps1)

| Windows demo input | Input bytes | Result |
| --- | ---: | --- |
| 12-second MKV | 127,337,016 | 19,465,493-byte MP4 |
| 125-second stereo WAV | 24,000,078 | 1,690,914-byte lossless FLAC |
| Short M4A | 9,429 | Skipped |

Additional tests exercised the same compression engine with FFmpeg 7.1.5 on Linux.

| Input | Input bytes | Output bytes | Verified property |
| --- | ---: | ---: | --- |
| Portrait MP4 | 34,273,678 | 19,419,157 | All 540 frames at 1080 × 1920 and 30 fps |
| MKV with alternate audio and subtitles | 34,582,552 | 19,423,509 | Two stereo tracks, subtitle text and cue timing |
| 140-second stereo WAV | 26,880,044 | 16,544,908 | FLAC decoded to identical PCM samples |
| Same WAV with **Keep WAV** | 26,880,044 | 17,920,078 | 140 seconds of 32 kHz stereo PCM |

Other checks exercised cancellation, concurrent filename collisions, retrying an exactly-at-cap output, a misleading file extension, variable frame timing, rotated video and declining a proposed downscale. The ribbon comparison uses a separate generated four-second animation. Its MKV measures 22,830,999 bytes and its MP4 measures 19,743,056 bytes. The animated preview samples matching frames from both files. The linked PNG contains the original-size decoded frames.

## Build from source

The app uses C++17, Win32 common controls and CMake. From the repository root, with Visual Studio C++ build tools, the Windows SDK and CMake installed, run the following in a developer shell.

```powershell
cmake -S src -B build -A x64
cmake --build build --config Release
```

The Visual Studio generator places the executable at `build/Release/Under20.exe`. The portable download contains the program, its source and runtime notices. The ZIP retains the original pre-publication readme. The Windows run above was performed later on this same package. Its SHA-256 is `d3adbb4d0a8752e434c4407ebe20d5d0a8c438079b0bcc84d99f4db0b71f5df4`.

On Linux, the same CMake project builds `media-test`, a command-line harness for the shared engine. It defaults to `/usr/bin/ffmpeg` and `/usr/bin/ffprobe`. Override them with `TEST_FFMPEG` and `TEST_FFPROBE` if needed.

[Window and queue](src/window.cpp) · [Compression and verification](src/media.cpp) · [FFmpeg process handling](src/process.cpp) · [Runtime notices](notices/)

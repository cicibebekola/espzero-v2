# SD MP3 Music

Upload `.mp3` files into `/music` using the launcher's web file manager. Install
the music application binary into `/apps` on the SD card, launch it, use Scan to
connect Bluetooth headphones/speakers, then select a song. Play/Pause, Stop,
Previous, and Next operate without loading the complete song into memory.
The bottom Back to Launcher button stops audio and reboots into the launcher.
New launcher builds save their return partition in NVS before launching an app;
legacy installs fall back to Launcher in `ota_0` and Music in `ota_1`. If the
return image is missing, invalid, or points to the running app, Back reports a
failure instead of rebooting. Upload this build through the launcher, not over
the launcher's flash partition.

This first version scans up to 32 MP3 files directly in `/music`, sorts them by
filename, and reads ID3v1 titles/artists where present. Subfolders are not scanned.
Reopen Music after uploading songs. Total duration is a first-frame bitrate
estimate (VBR duration can be inaccurate); elapsed time uses decoded samples.

The streaming pipeline uses a 4 KB compressed-input buffer, a 4.5 KB decoded
frame buffer, an 8 KB Bluetooth PCM ring, and 1 KB resampling chunks. The decoder
state is heap allocated. minimp3's measured frame-decoding stack usage is 17,344
bytes before callers, so the player reserves a 24 KB task stack, not the old
unsafe 8 KB stack. Playlist, Bluetooth, display, and RTOS memory are additional.
Allocation failures, missing/unreadable MP3s, and Bluetooth disconnect/stall
errors are reported on the now-playing card. Audio-rate conversion is linear,
not a high-quality anti-aliasing resampler.

Stereo/mono MP3 input is converted to the negotiated Bluetooth output rate.
PCM sends are bounded and retried with backpressure; transport commands can
interrupt those waits. Bluetooth remains awake while connected to prevent the
sleep manager interrupting playback. MP4/AAC/video support is not included.

Build independently of the launcher:

```text
idf.py -B build_music -DACTIVE_APP=bt_music build
```

Result: `build_music/esp_zero.bin`. The launcher output remains in `build`.

Music uses its own `sdkconfig.music`, generated from the shared defaults plus
`sdkconfig.music.defaults`. Unused LVGL widgets, extra themes, demos, and the
pull-down UI are excluded. The LVGL pool is 32 KB instead of 64 KB. Music uses
one 240 x 20 RGB565 display buffer (9,600 bytes), instead of two 240 x 40 buffers
(38,400 bytes); this trades some redraw throughput for Bluetooth headroom.
These two changes reclaim 61,568 bytes, before playlist/UI savings. Playlist
storage shrinks to the actual track count after scanning. The decoder's 24 KB
stack is retained. The launcher's UI features and display buffers are unchanged.
Startup reports free heap and the largest allocatable block after creating the
UI; check these and scan/connect/playback on hardware to confirm the budget.

`tests/music_audio_test.c` is a portable native C regression test for decoder
frame counts, stereo preservation, mono conversion, rate conversion across
chunks, and cancellation. Compile it with `audio/pcm_resampler.c` and
`components/minimp3/minimp3_impl.c`, with both directories in the include path.
A complete host C toolchain (including standard headers and runtime) is needed.

Hardware checks still required: listening tests with 44.1/48/22.05 kHz and mono
files, high-bitrate/VBR files, pauses, Stop with repeat enabled, selecting another
track mid-playback, headphone disconnection, SD read failure, and free-heap/stack
high-water measurements during sustained playback.

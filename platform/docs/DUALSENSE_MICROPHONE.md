# DualSense microphone capture

Status: reusable capture API and example implemented on `feature/dualsense-mic-stream`. A PS5 title-context test has confirmed live capture once; firmware/model details and numerical measurements were not recorded.

## What is known

The PS5 input path is a system audio service. A title opens an AudioIn port for a user, then reads signed PCM blocks from that port. It is not necessary to read raw DualSense USB/Bluetooth reports: the system routes the user's selected microphone device. That means the capture may come from a headset or USB mic if that is the selected input, rather than the controller's built-in mic. Sony describes selecting the active input device and muting from the PS5 Mic control center in its [microphone settings guide](https://www.playstation.com/en-us/support/hardware/ps5-change-audio-output/).

**Hardware validation (2026-11-27):** a homebrew title using this SDK API opened AudioIn at 48 kHz and was reported working by the PS5 owner. A raw ELF sent through `ps5-payload-elfldr` did not provide application identity and failed at `sceAudioInOpen` with `0x809b0081`; that result does not apply to a title launch. The title-context screen originally appeared static because its renderer drew only at startup; after making it refresh continuously, the owner confirmed that capture worked. The console model, firmware, selected input device, controller connection mode, and exact live meter values were not captured, so this is one positive hardware report rather than a compatibility claim.

The most concrete implementation reference in the related homebrew workspace is [PS5_RetroArch's `microphone_ps5.cpp`](https://github.com/mihawk-99/PS5_RetroArch/blob/main/src/microphone_ps5.cpp). Its source comments report native capture and mute-state validation on firmware 13.60. It initializes AudioIn, opens the initial user's port, reads fixed-size PCM blocks on a dedicated thread, and feeds a bounded ring buffer for the frontend to consume. It first tries 48 kHz with 256 samples per block, then 16 kHz; it tries port types 0 and 1. A separate [firmware 6.02 report](https://github.com/blackbearreloaded/ps5-audio-decoding-research/blob/main/examples/native-audio-poc/README.md) says type 5 also produced valid PCM in two runs, so the wrapper tries types 0, 1, and 5 in that order. The buffer format is signed 16-bit mono. A 256-sample block spans about 5.33 ms at 48 kHz or 16 ms at 16 kHz.

The capture functions used by that implementation have these observed signatures:

```c
int sceAudioInInit(void);
int sceAudioInOpen(int user_id, int type, int index,
                   unsigned length, unsigned rate, unsigned format);
int sceAudioInInput(int handle, void *samples);
int sceAudioInGetSilentState(int handle);
int sceAudioInClose(int handle);
```

Treat the signature as an observed homebrew ABI, not an official Sony header. Port types `0`, `1`, and `5`, format `0`, 256-sample grains, and rates 48 kHz/16 kHz are the values used by the sources and related probes; availability can vary with firmware and process context. `sceAudioInInit` may return an already-initialized error; the wrapper accepts the two observed initialized codes (`0x8026000e`, `0x8026000c`). The PS5 developer wiki identifies the internal AudioIn module as `libSceAudioIn`, module id `0x80000002` ([module list](https://www.psdevwiki.com/ps5/Libraries)). This fork's `crt/rtld_sprx.c` already knows that module id.

The RetroArch implementation treats `(silent_state & 5) != 0` as a muted or absent input state, emits zero samples in that state, and clears queued audio when the state changes. Those bit meanings are based on its target testing and should remain documented as firmware observations until independently checked. The PS5 UI's mute control must be considered too; a capture API should not preserve older speech in its queue after mute or pause.

## Real-time design for this SDK

The public abstraction is in `platform/include/ps5platform/microphone.h`; the empirical ABI declarations and import stub are `include/ps5/audioin.h` and `sce_stubs/libSceAudioIn.c`:

```text
AudioIn port
  -> dedicated capture thread (one 256-frame PCM block per read)
  -> bounded S16 mono ring buffer
  -> nonblocking or timed read by a homebrew worker
  -> voice encoder, VAD, recording, or other consumer
```

Example:

```c
ps5_microphone_options_t options = {
  .user_id = user_id,          /* obtained by the application via UserService */
  .sample_rate = 0,            /* 48 kHz, then 16 kHz fallback */
  .frames_per_read = 256,
  .queue_frames = 4096,
};
ps5_microphone_t *mic;
ps5_microphone_open(&mic, &options);
int16_t pcm[256];              /* mono S16; one read = 256 frames */
int frames = ps5_microphone_read(mic, pcm, 256, 100); /* timeout in ms */
ps5_microphone_close(mic);
```

Build consumers with `-lps5platform -lSceAudioIn`. The fork's SDK setup script builds and installs this AudioIn import stub alongside the platform archive. `samples/microphone_stream/` is a 30-second live stream example; it prints signal levels, silence state, and dropped-frame counts so that mute behavior can be observed without writing captured voice to a file. See its README for the build/deploy commands. The consumer API reports the negotiated rate, PCM frame reads, timeout/nonblocking behavior, and captured/delivered/dropped counters. The queue has a fixed maximum size; when full, dropping the oldest frames keeps a live stream current instead of adding unbounded latency. Applications should consume samples on a worker thread and must not call the blocking capture function from a render or controller-poll loop.

The capture starts only after an explicit open request. Close stops delivery, clears pending samples, joins the capture worker, and closes the port. Errors from open/input/state queries are returned to the caller. No explicit module load/unload operation is currently used by the wrapper. The selected user id must be supplied by the caller, so the core platform library does not silently choose a user.

The wrapper uses the source-observed `(silent_state & 5)` mute/absent-input interpretation: it clears stale queued frames when entering that state and delivers zero-filled blocks while muted. This is not an official ABI guarantee. Buffers are wiped when cleared and when the capture worker exits. `ps5_microphone_close()` waits for an in-flight AudioIn read to return; check stop latency on target hardware before using it in a latency-sensitive shutdown path. Do not call `read`, `get_stats`, and `close` concurrently on the same handle unless externally synchronized. Recording applications should make capture visible to the player and handle consent and privacy according to their use case; the system API's mute state should always be respected.

## Evidence and limits

- **Runtime implementation reference:** [PS5_RetroArch capture driver](https://github.com/mihawk-99/PS5_RetroArch/blob/main/src/microphone_ps5.cpp); its source records firmware 13.60 validation.
- **Independent API/capture probe source:** [Halo Android PS5 AudioIn probe](https://github.com/zay448345045/halo-android/blob/main/port/ps5/probe/audioin/main.c); documents user selection, port profiles, rates, block sizes, mute-state sampling, and module-load experiments.
- **SDK evidence:** `crt/rtld_sprx.c` maps `libSceAudioIn.sprx` to `0x80000002`; no import stub or header existed before this feature branch.
- **Official user behavior:** [Sony's PS5 mic settings](https://www.playstation.com/en-us/support/hardware/ps5-change-audio-output/); describes input-device selection and mute controls but does not publish the programming ABI.

No console is attached to this development environment, so code changes here cannot by themselves establish hardware capture, route choice, mute behavior, or firmware compatibility. Those claims require a target run with the console model, firmware, selected input device, controller connection mode, and mute state recorded.

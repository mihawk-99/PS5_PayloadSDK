# Microphone stream sample

This sample opens the current initial PS5 user’s system-selected microphone,
reads 30 seconds of signed 16-bit mono PCM, and prints peak/mean-square levels,
the AudioIn silence state, and dropped-frame counts once per second. It does not
save captured audio. Toggle the DualSense MUTE button during capture and check
that `silent` becomes nonzero and the signal level falls to zero.

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
make -C samples/microphone_stream
export PS5_HOST=ps5
make -C samples/microphone_stream test
```

Before deploying, make sure the PS5 homebrew environment is active and an ELF
loader is already listening on the target port. `prospero-deploy` sends the ELF;
it does not start the loader. The standard `ps5-payload-elfldr` listener uses
TCP port 9021. Set `PS5_HOST` and `PS5_PORT` for your console and loader. For a
different capture duration, pass for example
`make -C samples/microphone_stream CAPTURE_SECONDS=10`.

**Runtime limitation:** sending this sample as a standalone ELF through
`ps5-payload-elfldr` does not provide a PS5 application identity. In this
payload context, `sceAudioInOpen` can fail with `0x809b0081` even when
UserService successfully finds the foreground user. This was observed on a
PS5 at `192.168.1.177`: user routing succeeded, but AudioIn rejected the open.
It is not evidence that microphone capture fails in a homebrew title. To verify
actual PCM capture, package this sample as an app and launch it through the
console's homebrew title loader; the title must have an app identity. Port 9021
only accepts ELF payloads and does not install or launch a title. The sample's
`test` target is therefore a payload-context smoke test, not a microphone
hardware acceptance test.

A separate title-context probe using this SDK API was reported working on a
PS5 on 2026-11-27. Its model, firmware, selected input device, and numeric
capture measurements were not recorded; see
[`platform/docs/DUALSENSE_MICROPHONE.md`](../../platform/docs/DUALSENSE_MICROPHONE.md)
for the validation scope.

The printed silence bitmask is firmware-dependent diagnostic information, not
a public Sony API contract. Record the PS5 model, system firmware, selected
input device, controller connection mode, and mute transitions when reporting
results. The AudioIn route follows the input selected by the system, so this
sample does not guarantee that the built-in DualSense microphone is the active
source.

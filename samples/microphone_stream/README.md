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

Set `PS5_HOST` and `PS5_PORT` for the target console and its payload deploy
service. For a different capture duration, pass for example
`make -C samples/microphone_stream CAPTURE_SECONDS=10`.

The printed silence bitmask is firmware-dependent diagnostic information, not
a public Sony API contract. Record the PS5 model, system firmware, selected
input device, controller connection mode, and mute transitions when reporting
results. The AudioIn route follows the input selected by the system, so this
sample does not guarantee that the built-in DualSense microphone is the active
source.

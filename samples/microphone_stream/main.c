#include <ps5platform/microphone.h>

#include <stdint.h>
#include <stdio.h>

#ifndef CAPTURE_SECONDS
#define CAPTURE_SECONDS 30u
#endif

int sceUserServiceInitialize(void *param);
int sceUserServiceTerminate(void);
int sceUserServiceGetInitialUser(int *user_id);

int
main(void) {
  int user_id = -1;
  ps5_microphone_options_t options;
  ps5_microphone_stats_t stats;
  ps5_microphone_t *mic = NULL;
  int16_t samples[256];
  uint64_t frames = 0;
  uint64_t window_frames = 0;
  uint64_t energy = 0;
  unsigned loops = 0;
  unsigned elapsed = 0;
  int peak = 0;
  int rc;

  rc = sceUserServiceInitialize(NULL);
  if(rc < 0) {
    fprintf(stderr, "Could not initialize UserService: %x\n", rc);
    return 1;
  }
  rc = sceUserServiceGetInitialUser(&user_id);
  if(rc < 0) {
    fprintf(stderr, "Could not get initial user: %x\n", rc);
    sceUserServiceTerminate();
    return 1;
  }
  options.user_id = user_id;
  options.sample_rate = 0; /* Prefer 48 kHz; fall back to 16 kHz. */
  options.frames_per_read = 256;
  options.queue_frames = 4096;
  rc = ps5_microphone_open(&mic, &options);
  if(rc < 0) {
    fprintf(stderr, "Could not open selected microphone: %x\n", rc);
    sceUserServiceTerminate();
    return 1;
  }

  ps5_microphone_get_stats(mic, &stats);
  printf("Input opened: %u Hz, S16 mono, user %d\n", stats.sample_rate, user_id);
  printf("Reading stream for %u seconds; toggle the DualSense MUTE button to observe silence.\n",
         CAPTURE_SECONDS);
  while(elapsed < CAPTURE_SECONDS) {
    rc = ps5_microphone_read(mic, samples, 256, 1000);
    if(rc < 0) {
      fprintf(stderr, "Microphone read failed: %x\n", rc);
      break;
    }
    if(rc == 0) continue;
    frames += (unsigned)rc;
    window_frames += (unsigned)rc;
    loops++;
    {
      int i;
      for(i = 0; i < rc; i++) {
        int value = samples[i];
        int magnitude = value < 0 ? -value : value;
        energy += (uint64_t)value * value;
        if(magnitude > peak) peak = magnitude;
      }
    }
    if(loops >= stats.sample_rate / 256) {
      ps5_microphone_get_stats(mic, &stats);
      printf("second=%u frames=%llu peak=%d mean-square=%llu silent=0x%x dropped=%llu\n",
             elapsed + 1, (unsigned long long)frames, peak,
             (unsigned long long)(energy / window_frames), stats.silent_state,
             (unsigned long long)stats.dropped_frames);
      loops = 0;
      energy = 0;
      window_frames = 0;
      peak = 0;
      elapsed++;
    }
  }
  ps5_microphone_get_stats(mic, &stats);
  printf("Capture complete: captured=%llu delivered=%llu dropped=%llu\n",
         (unsigned long long)stats.captured_frames,
         (unsigned long long)stats.delivered_frames,
         (unsigned long long)stats.dropped_frames);
  ps5_microphone_close(mic);
  sceUserServiceTerminate();
  return rc < 0 ? 1 : 0;
}

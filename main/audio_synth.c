#include "audio_synth.h"
#include "led_effects.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

static const char *TAG = "AUDIO_SYNTH";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define MAX_POLY_VOICES 8
#define CHUNK_SAMPLES 256

// Pre-computed Sine Wavetable for Ultra-Fast DSP (512 points)
#define SINE_TABLE_SIZE 512
#define SINE_TABLE_MASK (SINE_TABLE_SIZE - 1)
static float s_sine_table[SINE_TABLE_SIZE];

static void init_sine_table(void) {
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        s_sine_table[i] = sinf(2.0f * M_PI * (float)i / (float)SINE_TABLE_SIZE);
    }
}

static inline float fast_sin(float phase) {
    uint32_t idx = ((uint32_t)phase) & SINE_TABLE_MASK;
    return s_sine_table[idx];
}

typedef enum {
    VOICE_IDLE = 0,
    VOICE_TONE_ADSR,     // Piano, Music box, 8-bit
    VOICE_PITCH_SWEEP,   // Kick, Laser, Boing, Water drop, Whistle
    VOICE_NOISE_BURST,   // Snare, Hi-hat, Cymbal, Typewriter click
    VOICE_MORSE,         // Morse dit/dah sequence
    VOICE_R2D2,          // Robot chirps
    VOICE_ENGINE_RUMBLE, // Cockpit engine
    VOICE_ANIMAL         // Animal sounds
} voice_mode_t;

typedef struct {
    bool active;
    voice_mode_t mode;
    float phase;
    float phase_inc;
    float start_freq;
    float end_freq;
    float amp;
    uint32_t pos;
    uint32_t length;
    uint8_t sub_type;
} voice_t;

static i2s_chan_handle_t s_tx_handle = NULL;
static bool s_audio_ready = false;
static float s_master_volume = AUDIO_DEFAULT_VOLUME;
static SemaphoreHandle_t s_voice_mutex = NULL;
static voice_t s_voices[MAX_POLY_VOICES];

// Cockpit siren state
static bool s_siren_on = false;
static float s_siren_phase = 0.0f;
static uint32_t s_siren_step = 0;

// Speed runner timing
static int64_t s_last_speed_key_time = 0;
static uint32_t s_speed_melody_idx = 0;

// Mozart song data
typedef struct {
    float freq;
    uint32_t dur_ms;
} note_t;

static const note_t s_song_baby_shark[] = {
    {293.66f, 250}, {329.63f, 250}, {392.00f, 200}, {392.00f, 200}, {392.00f, 200},
    {392.00f, 150}, {392.00f, 150}, {392.00f, 200}, {392.00f, 350},
    {293.66f, 250}, {329.63f, 250}, {392.00f, 200}, {392.00f, 200}, {392.00f, 200},
    {392.00f, 150}, {392.00f, 150}, {392.00f, 200}, {392.00f, 350},
    {293.66f, 250}, {329.63f, 250}, {392.00f, 200}, {392.00f, 200}, {392.00f, 200},
    {392.00f, 150}, {392.00f, 150}, {392.00f, 200}, {392.00f, 350},
    {392.00f, 250}, {392.00f, 250}, {369.99f, 600}
};

static const note_t s_song_twinkle[] = {
    {261.63f, 300}, {261.63f, 300}, {392.00f, 300}, {392.00f, 300},
    {440.00f, 300}, {440.00f, 300}, {392.00f, 600},
    {349.23f, 300}, {349.23f, 300}, {329.63f, 300}, {329.63f, 300},
    {293.66f, 300}, {293.66f, 300}, {261.63f, 600}
};

static const note_t s_song_butterfly[] = {
    // Kìa con bướm vàng (Frère Jacques)
    {261.63f, 300}, {293.66f, 300}, {329.63f, 300}, {261.63f, 300},
    {261.63f, 300}, {293.66f, 300}, {329.63f, 300}, {261.63f, 300},
    {329.63f, 300}, {349.23f, 300}, {392.00f, 600},
    {329.63f, 300}, {349.23f, 300}, {392.00f, 600}
};

static const note_t s_song_happy_birthday[] = {
    {261.63f, 200}, {261.63f, 200}, {293.66f, 400}, {261.63f, 400}, {349.23f, 400}, {329.63f, 800},
    {261.63f, 200}, {261.63f, 200}, {293.66f, 400}, {261.63f, 400}, {392.00f, 400}, {349.23f, 800}
};

static const struct {
    const note_t *notes;
    size_t count;
    const char *name;
} s_mozart_playlist[] = {
    {s_song_baby_shark, sizeof(s_song_baby_shark)/sizeof(note_t), "Baby Shark"},
    {s_song_butterfly, sizeof(s_song_butterfly)/sizeof(note_t), "Kia Con Buom Vang"},
    {s_song_twinkle, sizeof(s_song_twinkle)/sizeof(note_t), "Twinkle Little Star"},
    {s_song_happy_birthday, sizeof(s_song_happy_birthday)/sizeof(note_t), "Happy Birthday"}
};

static uint8_t s_current_song = 0;
static uint32_t s_current_note_idx = 0;

static const float s_speed_notes[] = {
    261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f, 523.25f,
    587.33f, 659.25f, 698.46f, 783.99f, 880.00f, 783.99f, 659.25f, 523.25f
};

// Thread-safe voice triggering
static void trigger_voice_internal(voice_mode_t mode, float freq, float start_f, float end_f,
                                   float amp, uint32_t dur_ms, uint8_t sub_type) {
    if (!s_voice_mutex) return;
    xSemaphoreTake(s_voice_mutex, portMAX_DELAY);

    voice_t *chosen = NULL;
    for (int i = 0; i < MAX_POLY_VOICES; i++) {
        if (!s_voices[i].active) {
            chosen = &s_voices[i];
            break;
        }
    }
    if (!chosen) {
        uint32_t max_pos = 0;
        int steal_idx = 0;
        for (int i = 0; i < MAX_POLY_VOICES; i++) {
            if (s_voices[i].pos > max_pos) {
                max_pos = s_voices[i].pos;
                steal_idx = i;
            }
        }
        chosen = &s_voices[steal_idx];
    }

    chosen->mode = mode;
    chosen->phase = 0.0f;
    chosen->phase_inc = (float)(SINE_TABLE_SIZE * freq) / (float)AUDIO_SAMPLE_RATE;
    chosen->start_freq = start_f;
    chosen->end_freq = end_f;
    chosen->amp = amp;
    chosen->pos = 0;
    chosen->length = (AUDIO_SAMPLE_RATE * dur_ms) / 1000;
    if (chosen->length == 0) chosen->length = 1;
    chosen->sub_type = sub_type;
    chosen->active = true;

    xSemaphoreGive(s_voice_mutex);
}

// Low-level audio synthesis task running on Core 1
static void audio_synth_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio synthesis task started on CPU core %d", xPortGetCoreID());

    int32_t *mix_buffer = (int32_t *)heap_caps_malloc(CHUNK_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!mix_buffer) {
        mix_buffer = (int32_t *)malloc(CHUNK_SAMPLES * 2 * sizeof(int32_t));
    }

    float float_mix[CHUNK_SAMPLES];

    while (1) {
        // Clear mix buffer
        memset(float_mix, 0, sizeof(float_mix));

        xSemaphoreTake(s_voice_mutex, portMAX_DELAY);

        // Mix active voices using ultra-fast wavetable
        for (int v = 0; v < MAX_POLY_VOICES; v++) {
            voice_t *vc = &s_voices[v];
            if (!vc->active) continue;

            for (int i = 0; i < CHUNK_SAMPLES; i++) {
                if (vc->pos >= vc->length) {
                    vc->active = false;
                    break;
                }

                float sample = 0.0f;
                float progress = (float)vc->pos / (float)vc->length;

                switch (vc->mode) {
                    case VOICE_TONE_ADSR: {
                        float env = 1.0f;
                        if (progress < 0.04f) {
                            env = progress / 0.04f;
                        } else {
                            float decay = 1.0f - progress;
                            env = decay * decay;
                        }

                        vc->phase += vc->phase_inc;
                        float s = fast_sin(vc->phase);

                        if (vc->sub_type == TIMBRE_PIANO) {
                            // Piano: fundamental + 2nd harmonic
                            float s2 = fast_sin(vc->phase * 2.0f);
                            sample = (s + 0.35f * s2) * env * vc->amp;
                        } else if (vc->sub_type == TIMBRE_8BIT_MARIO) {
                            // Square wave
                            sample = ((s >= 0.0f) ? 0.7f : -0.7f) * env * vc->amp;
                        } else {
                            // Music box: Pure bell with decay
                            float bell_env = (1.0f - progress) * (1.0f - progress);
                            float s3 = fast_sin(vc->phase * 2.75f);
                            sample = (s + 0.25f * s3) * bell_env * vc->amp;
                        }
                        break;
                    }

                    case VOICE_PITCH_SWEEP: {
                        float cur_f = vc->start_freq + (vc->end_freq - vc->start_freq) * progress;
                        vc->phase += (float)(SINE_TABLE_SIZE * cur_f) / (float)AUDIO_SAMPLE_RATE;
                        float env = 1.0f - progress;
                        sample = fast_sin(vc->phase) * env * vc->amp;
                        break;
                    }

                    case VOICE_NOISE_BURST: {
                        float env = (1.0f - progress) * (1.0f - progress);
                        float white = ((float)(rand() % 2000) - 1000.0f) / 1000.0f;
                        sample = white * env * vc->amp;
                        break;
                    }

                    case VOICE_MORSE: {
                        float env = 1.0f;
                        if (progress < 0.05f) env = progress / 0.05f;
                        else if (progress > 0.95f) env = (1.0f - progress) / 0.05f;
                        vc->phase += (float)(SINE_TABLE_SIZE * 750.0f) / (float)AUDIO_SAMPLE_RATE;
                        sample = fast_sin(vc->phase) * env * vc->amp;
                        break;
                    }

                    case VOICE_R2D2: {
                        vc->phase += (float)(SINE_TABLE_SIZE * vc->start_freq) / (float)AUDIO_SAMPLE_RATE;
                        sample = fast_sin(vc->phase) * vc->amp;
                        break;
                    }

                    case VOICE_ENGINE_RUMBLE: {
                        vc->phase += (float)(SINE_TABLE_SIZE * 55.0f) / (float)AUDIO_SAMPLE_RATE;
                        float pulse = fast_sin(vc->phase);
                        float n = ((float)(rand() % 1000) - 500.0f) / 1000.0f;
                        sample = (pulse * 0.7f + n * 0.3f) * vc->amp;
                        break;
                    }

                    case VOICE_ANIMAL: {
                        float f = vc->start_freq + (vc->end_freq - vc->start_freq) * progress;
                        vc->phase += (float)(SINE_TABLE_SIZE * f) / (float)AUDIO_SAMPLE_RATE;
                        float env = fast_sin(progress * (float)(SINE_TABLE_SIZE / 2));
                        sample = fast_sin(vc->phase) * env * vc->amp;
                        break;
                    }

                    default:
                        break;
                }

                float_mix[i] += sample;
                vc->pos++;
            }
        }

        // Handle cockpit siren if active
        if (s_siren_on) {
            s_siren_step += CHUNK_SAMPLES;
            float siren_t = (float)s_siren_step / (float)AUDIO_SAMPLE_RATE;
            float siren_f = ((fmodf(siren_t, 0.7f) < 0.35f) ? 440.0f : 554.0f);
            float inc = (float)(SINE_TABLE_SIZE * siren_f) / (float)AUDIO_SAMPLE_RATE;
            for (int i = 0; i < CHUNK_SAMPLES; i++) {
                s_siren_phase += inc;
                float_mix[i] += fast_sin(s_siren_phase) * 0.45f;
            }
        }

        xSemaphoreGive(s_voice_mutex);

        // Convert float mix to 32-bit stereo PCM with soft limiter
        const float max_int32 = 2147480000.0f * s_master_volume;
        for (int i = 0; i < CHUNK_SAMPLES; i++) {
            float val = float_mix[i];
            if (val > 1.0f) val = 1.0f;
            else if (val < -1.0f) val = -1.0f;

            int32_t pcm = (int32_t)(val * max_int32);
            mix_buffer[i * 2]     = pcm; // Left channel
            mix_buffer[i * 2 + 1] = pcm; // Right channel
        }

        // Write directly to I2S DMA
        size_t written = 0;
        i2s_channel_write(s_tx_handle, mix_buffer, CHUNK_SAMPLES * 2 * sizeof(int32_t), &written, pdMS_TO_TICKS(50));
    }
}

esp_err_t audio_synth_init(void) {
    ESP_LOGI(TAG, "Initializing I2S MAX98357A on GPIO WS=%d, DIN=%d, BCLK=%d",
             PIN_I2S_LCK, PIN_I2S_DIN, PIN_I2S_BCK);

    init_sine_table();
    s_voice_mutex = xSemaphoreCreateMutex();
    memset(s_voices, 0, sizeof(s_voices));

    // I2S Standard Master Channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    slot_cfg.ws_width = 32;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCK,
            .ws   = (gpio_num_t)PIN_I2S_LCK,
            .dout = (gpio_num_t)PIN_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    s_audio_ready = true;

    // Launch synthesis task pinned to CPU Core 1 with normal priority (4) so IDLE1 can always run
    xTaskCreatePinnedToCore(audio_synth_task, "audio_synth", 6144, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "I2S Audio Engine ready at %d Hz!", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

void audio_synth_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_master_volume = vol;
    ESP_LOGI(TAG, "Master Volume: %d%%", (int)(s_master_volume * 100));
}

float audio_synth_get_volume(void) {
    return s_master_volume;
}

void audio_synth_volume_up(void) {
    audio_synth_set_volume(s_master_volume + 0.05f);
}

void audio_synth_volume_down(void) {
    audio_synth_set_volume(s_master_volume - 0.05f);
}

// Mode 1: Piano Note
void audio_play_piano_note(float freq_hz, piano_timbre_t timbre) {
    trigger_voice_internal(VOICE_TONE_ADSR, freq_hz, 0, 0, 0.85f, 750, timbre);
}

// Mode change melodic chime
void audio_play_chime_mode(uint8_t mode_index) {
    float root_f = 440.0f + (mode_index * 55.0f);
    trigger_voice_internal(VOICE_TONE_ADSR, root_f, 0, 0, 0.7f, 250, TIMBRE_MUSIC_BOX);
    trigger_voice_internal(VOICE_TONE_ADSR, root_f * 1.5f, 0, 0, 0.8f, 400, TIMBRE_MUSIC_BOX);
}

// Mode 2: Baby Mozart
void audio_play_mozart_next_note(void) {
    const note_t *curr_notes = s_mozart_playlist[s_current_song].notes;
    size_t count = s_mozart_playlist[s_current_song].count;

    note_t n = curr_notes[s_current_note_idx];
    s_current_note_idx = (s_current_note_idx + 1) % count;

    trigger_voice_internal(VOICE_TONE_ADSR, n.freq, 0, 0, 0.9f, n.dur_ms * 2, TIMBRE_PIANO);
}

void audio_mozart_next_song(void) {
    s_current_song = (s_current_song + 1) % (sizeof(s_mozart_playlist) / sizeof(s_mozart_playlist[0]));
    s_current_note_idx = 0;
    ESP_LOGI(TAG, "Switched song to: %s", s_mozart_playlist[s_current_song].name);
    audio_play_cartoon_sfx(SFX_MARIO_COIN);
}

// Mode 3: Speed Runner
void audio_speed_runner_keystroke(void) {
    s_last_speed_key_time = esp_timer_get_time();
    float f = s_speed_notes[s_speed_melody_idx];
    s_speed_melody_idx = (s_speed_melody_idx + 1) % (sizeof(s_speed_notes)/sizeof(float));
    trigger_voice_internal(VOICE_TONE_ADSR, f, 0, 0, 0.8f, 180, TIMBRE_8BIT_MARIO);
}

void audio_speed_runner_idle_check(void) {
    if (s_last_speed_key_time > 0) {
        int64_t diff = esp_timer_get_time() - s_last_speed_key_time;
        if (diff > 500000 && diff < 600000) {
            audio_play_cartoon_sfx(SFX_SKID_STOP);
            s_last_speed_key_time = 0;
        }
    }
}

// Mode 4: Drum Kit
void audio_play_drum(drum_type_t drum) {
    switch (drum) {
        case DRUM_KICK:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 160.0f, 42.0f, 1.0f, 140, 0);
            break;
        case DRUM_SNARE:
            trigger_voice_internal(VOICE_NOISE_BURST, 0, 0, 0, 0.95f, 160, 0);
            trigger_voice_internal(VOICE_TONE_ADSR, 190.0f, 0, 0, 0.65f, 80, TIMBRE_PIANO);
            break;
        case DRUM_HIHAT:
            trigger_voice_internal(VOICE_NOISE_BURST, 0, 0, 0, 0.55f, 45, 0);
            break;
        case DRUM_CYMBAL:
            trigger_voice_internal(VOICE_NOISE_BURST, 0, 0, 0, 0.85f, 450, 0);
            break;
        case DRUM_TOM:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 220.0f, 90.0f, 0.9f, 180, 0);
            break;
        case DRUM_CLAP:
            trigger_voice_internal(VOICE_NOISE_BURST, 0, 0, 0, 0.9f, 120, 0);
            break;
    }
}

// Mode 5: Vintage Typewriter
void audio_play_typewriter(typewriter_sound_t type) {
    switch (type) {
        case TYPEWRITER_KEY_CLICK:
            trigger_voice_internal(VOICE_NOISE_BURST, 0, 0, 0, 0.75f, 25, 0);
            break;
        case TYPEWRITER_SPACE_SLIDE:
            trigger_voice_internal(VOICE_NOISE_BURST, 0, 0, 0, 0.5f, 140, 0);
            break;
        case TYPEWRITER_ENTER_BELL:
            trigger_voice_internal(VOICE_TONE_ADSR, 2489.0f, 0, 0, 0.95f, 650, TIMBRE_MUSIC_BOX);
            break;
        case TYPEWRITER_BACKSPACE:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 300.0f, 120.0f, 0.6f, 90, 0);
            break;
    }
}

// Mode 6: Cartoon SFX
void audio_play_cartoon_sfx(cartoon_sfx_t sfx) {
    switch (sfx) {
        case SFX_MARIO_COIN:
            trigger_voice_internal(VOICE_TONE_ADSR, 987.77f, 0, 0, 0.85f, 80, TIMBRE_8BIT_MARIO);
            trigger_voice_internal(VOICE_TONE_ADSR, 1318.51f, 0, 0, 0.95f, 320, TIMBRE_8BIT_MARIO);
            break;
        case SFX_SPRING_BOING:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 220.0f, 680.0f, 0.9f, 280, 1);
            break;
        case SFX_LASER:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 2600.0f, 180.0f, 0.85f, 130, 0);
            break;
        case SFX_CAR_HORN:
            trigger_voice_internal(VOICE_TONE_ADSR, 440.0f, 0, 0, 0.7f, 220, TIMBRE_8BIT_MARIO);
            trigger_voice_internal(VOICE_TONE_ADSR, 554.37f, 0, 0, 0.7f, 220, TIMBRE_8BIT_MARIO);
            break;
        case SFX_VICTORY_FANFARE:
            trigger_voice_internal(VOICE_TONE_ADSR, 523.25f, 0, 0, 0.85f, 150, TIMBRE_8BIT_MARIO);
            trigger_voice_internal(VOICE_TONE_ADSR, 659.25f, 0, 0, 0.85f, 150, TIMBRE_8BIT_MARIO);
            trigger_voice_internal(VOICE_TONE_ADSR, 783.99f, 0, 0, 0.85f, 150, TIMBRE_8BIT_MARIO);
            trigger_voice_internal(VOICE_TONE_ADSR, 1046.50f, 0, 0, 0.95f, 400, TIMBRE_8BIT_MARIO);
            break;
        case SFX_WHISTLE_SLIDE:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 1200.0f, 280.0f, 0.85f, 350, 0);
            break;
        case SFX_WATER_DROP:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 600.0f, 1900.0f, 0.85f, 90, 0);
            break;
        case SFX_DOORBELL:
            trigger_voice_internal(VOICE_TONE_ADSR, 783.99f, 0, 0, 0.9f, 300, TIMBRE_MUSIC_BOX);
            trigger_voice_internal(VOICE_TONE_ADSR, 659.25f, 0, 0, 0.9f, 500, TIMBRE_MUSIC_BOX);
            break;
        case SFX_FIRE_SIREN:
            audio_cockpit_siren_toggle();
            break;
        case SFX_SKID_STOP:
            trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 1800.0f, 400.0f, 0.8f, 200, 0);
            break;
    }
}

// Mode 7: Morse Code & Robot
static const char *s_morse_table[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",
    ".---", "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",
    "...",  "-",    "..-",  "...-", ".--",  "-..-", "-.--", "--.."
};

void audio_play_morse_char(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 'A' || c > 'Z') return;

    const char *morse = s_morse_table[c - 'A'];
    while (*morse) {
        uint32_t dur = (*morse == '.') ? 60 : 180;
        trigger_voice_internal(VOICE_MORSE, 750.0f, 0, 0, 0.85f, dur, 0);
        vTaskDelay(pdMS_TO_TICKS(dur + 60));
        morse++;
    }
}

void audio_play_robot_chirp(void) {
    int num_chirps = 3 + (rand() % 3);
    for (int i = 0; i < num_chirps; i++) {
        float f = 1800.0f + (rand() % 2000);
        uint32_t dur = 30 + rand() % 40;
        trigger_voice_internal(VOICE_R2D2, f, f, 0, 0.8f, dur, 0);
        vTaskDelay(pdMS_TO_TICKS(dur + 20));
    }
}

// Mode 8: Cockpit & Animals
void audio_cockpit_engine_start(void) {
    trigger_voice_internal(VOICE_ENGINE_RUMBLE, 55.0f, 0, 0, 0.95f, 400, 0);
}

void audio_cockpit_engine_stop(void) {}

void audio_cockpit_siren_toggle(void) {
    s_siren_on = !s_siren_on;
    ESP_LOGI(TAG, "Cockpit Siren: %s", s_siren_on ? "ON" : "OFF");
}

bool audio_cockpit_is_siren_on(void) {
    return s_siren_on;
}

void audio_cockpit_rocket_launch(void) {
    trigger_voice_internal(VOICE_TONE_ADSR, 880.0f, 0, 0, 0.9f, 250, TIMBRE_8BIT_MARIO);
    trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 80.0f, 500.0f, 1.0f, 800, 0);
}

void audio_play_animal_sound(char key) {
    if (key >= 'a' && key <= 'z') key -= 32;

    switch (key) {
        case 'D': // Dog
            trigger_voice_internal(VOICE_ANIMAL, 0, 280.0f, 150.0f, 0.9f, 160, 0);
            break;
        case 'C': // Cat
            trigger_voice_internal(VOICE_ANIMAL, 0, 450.0f, 900.0f, 0.9f, 350, 0);
            break;
        case 'B': // Bird
            trigger_voice_internal(VOICE_ANIMAL, 0, 2800.0f, 3800.0f, 0.9f, 100, 0);
            break;
        case 'M': // Cow
            trigger_voice_internal(VOICE_ANIMAL, 0, 150.0f, 120.0f, 0.9f, 500, 0);
            break;
        case 'Q': // Duck
            trigger_voice_internal(VOICE_ANIMAL, 0, 340.0f, 260.0f, 0.9f, 200, 0);
            break;
        case 'F': // Frog
            trigger_voice_internal(VOICE_ANIMAL, 0, 140.0f, 90.0f, 0.9f, 180, 0);
            break;
        case 'L': // Lion
            trigger_voice_internal(VOICE_ANIMAL, 0, 180.0f, 60.0f, 0.9f, 600, 0);
            break;
        case 'P': // Pig
            trigger_voice_internal(VOICE_ANIMAL, 0, 400.0f, 250.0f, 0.9f, 150, 0);
            break;
        default:
            trigger_voice_internal(VOICE_ANIMAL, 0, 400.0f + (key % 10) * 80.0f, 800.0f, 0.9f, 200, 0);
            break;
    }
}

// Mode 9: Phonics & Counting Learning
void audio_play_counting_number(uint8_t num) {
    if (num == 0) num = 10;
    if (num > 10) num = 10;

    // Harmonized bell note announcing the number
    trigger_voice_internal(VOICE_TONE_ADSR, 261.63f + num * 32.0f, 0, 0, 0.9f, 220, TIMBRE_PIANO);
    trigger_voice_internal(VOICE_TONE_ADSR, (261.63f + num * 32.0f) * 1.5f, 0, 0, 0.5f, 220, TIMBRE_MUSIC_BOX);

    // Punchy rhythmic drum beat pulses for counting with synchronized LED count
    for (uint8_t i = 0; i < num; i++) {
        led_effects_count(i + 1, 255, 180, 0);
        trigger_voice_internal(VOICE_PITCH_SWEEP, 0, 180.0f, 50.0f, 0.95f, 120, 0);
        if (i < num - 1) {
            vTaskDelay(pdMS_TO_TICKS(160));
        }
    }
}

void audio_play_phonics_letter(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;

    float f1 = 600.0f;
    float f2 = 1200.0f;

    switch (c) {
        case 'A': f1 = 800.0f; f2 = 1250.0f; break; // "A"
        case 'E': f1 = 530.0f; f2 = 1840.0f; break; // "E"
        case 'I': f1 = 270.0f; f2 = 2290.0f; break; // "I"
        case 'O': f1 = 510.0f; f2 = 840.0f;  break; // "O"
        case 'U': f1 = 300.0f; f2 = 870.0f;  break; // "U"
        case 'B': f1 = 300.0f; f2 = 1100.0f; break; // "Bờ"
        case 'C': f1 = 400.0f; f2 = 1400.0f; break; // "Cờ"
        case 'D': f1 = 350.0f; f2 = 1600.0f; break; // "Dờ"
        case 'G': f1 = 380.0f; f2 = 1300.0f; break; // "Gờ"
        case 'M': f1 = 280.0f; f2 = 1000.0f; break; // "Mờ"
        case 'N': f1 = 320.0f; f2 = 1450.0f; break; // "Nờ"
        case 'P': f1 = 340.0f; f2 = 1200.0f; break; // "Pờ"
        case 'T': f1 = 400.0f; f2 = 1700.0f; break; // "Tờ"
        case 'V': f1 = 320.0f; f2 = 1500.0f; break; // "Vờ"
        default:  f1 = 400.0f + (c % 7) * 60.0f; f2 = 1100.0f + (c % 9) * 80.0f; break;
    }

    // Double formant vocal tract filter
    trigger_voice_internal(VOICE_TONE_ADSR, f1, 0, 0, 0.75f, 220, TIMBRE_PIANO);
    trigger_voice_internal(VOICE_TONE_ADSR, f2, 0, 0, 0.45f, 220, TIMBRE_PIANO);
    // Plosive attack
    trigger_voice_internal(VOICE_NOISE_BURST, 0, 0, 0, 0.4f, 30, 0);
}


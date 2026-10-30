#include "audio_synth.h"
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

static const char *TAG = "AUDIO_SYNTH";

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define MAX_POLY_VOICES 12
#define CHUNK_SAMPLES 256

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
    float freq;
    float start_freq;
    float end_freq;
    float amp;
    uint32_t pos;
    uint32_t length;
    float param1;
    float param2;
    uint8_t sub_type;
} voice_t;

static i2s_chan_handle_t s_tx_handle = NULL;
static bool s_audio_ready = false;
static float s_master_volume = AUDIO_DEFAULT_VOLUME;
static SemaphoreHandle_t s_voice_mutex = NULL;
static voice_t s_voices[MAX_POLY_VOICES];

// Cockpit siren state
static bool s_siren_on = false;
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

// Speed runner melody (Can-Can arpeggio snippets)
static const float s_speed_notes[] = {
    261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f, 523.25f,
    587.33f, 659.25f, 698.46f, 783.99f, 880.00f, 783.99f, 659.25f, 523.25f
};

// Allocate a free voice
static voice_t* alloc_voice(void) {
    if (!s_voice_mutex) return NULL;
    xSemaphoreTake(s_voice_mutex, portMAX_DELAY);
    voice_t *chosen = NULL;
    for (int i = 0; i < MAX_POLY_VOICES; i++) {
        if (!s_voices[i].active) {
            chosen = &s_voices[i];
            break;
        }
    }
    // If all busy, steal voice with highest progress
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
    chosen->active = true;
    chosen->pos = 0;
    xSemaphoreGive(s_voice_mutex);
    return chosen;
}

// Low-level audio synthesis task running on Core 1
static void audio_synth_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio synthesis task started on CPU core %d", xPortGetCoreID());

    // Allocate 32-bit stereo buffer (Chunk * 2 channels)
    int32_t *mix_buffer = (int32_t *)heap_caps_malloc(CHUNK_SAMPLES * 2 * sizeof(int32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!mix_buffer) {
        mix_buffer = (int32_t *)malloc(CHUNK_SAMPLES * 2 * sizeof(int32_t));
    }

    float float_mix[CHUNK_SAMPLES];

    while (1) {
        // Clear mix buffer
        memset(float_mix, 0, sizeof(float_mix));

        xSemaphoreTake(s_voice_mutex, portMAX_DELAY);

        // Mix all active voices
        bool any_active = false;
        for (int v = 0; v < MAX_POLY_VOICES; v++) {
            voice_t *vc = &s_voices[v];
            if (!vc->active) continue;
            any_active = true;

            for (int i = 0; i < CHUNK_SAMPLES; i++) {
                if (vc->pos >= vc->length) {
                    vc->active = false;
                    break;
                }

                float t = (float)vc->pos / (float)AUDIO_SAMPLE_RATE;
                float sample = 0.0f;
                float progress = (float)vc->pos / (float)vc->length;

                switch (vc->mode) {
                    case VOICE_TONE_ADSR: {
                        float env = 1.0f;
                        if (progress < 0.05f) {
                            env = progress / 0.05f; // Attack
                        } else {
                            env = 1.0f - (progress - 0.05f) / 0.95f; // Decay
                            env = env * env; // Exponential curve
                        }

                        if (vc->sub_type == TIMBRE_PIANO) {
                            // Warm piano: fundamental + 2nd + 3rd harmonic
                            sample = (sinf(2.0f * M_PI * vc->freq * t) +
                                      0.35f * sinf(4.0f * M_PI * vc->freq * t) +
                                      0.15f * sinf(6.0f * M_PI * vc->freq * t)) * env * vc->amp;
                        } else if (vc->sub_type == TIMBRE_8BIT_MARIO) {
                            // Square wave 50% duty
                            float s = sinf(2.0f * M_PI * vc->freq * t);
                            sample = ((s >= 0.0f) ? 0.7f : -0.7f) * env * vc->amp;
                        } else {
                            // Music box: Pure bell with high shimmer
                            float bell_env = powf(1.0f - progress, 3.0f);
                            sample = (sinf(2.0f * M_PI * vc->freq * t) +
                                      0.25f * sinf(2.0f * M_PI * vc->freq * 2.75f * t)) * bell_env * vc->amp;
                        }
                        break;
                    }

                    case VOICE_PITCH_SWEEP: {
                        // Linear/Exponential pitch sweep
                        float current_f = vc->start_freq + (vc->end_freq - vc->start_freq) * progress;
                        float env = 1.0f - progress;
                        if (vc->sub_type == 1) { // Spring boing: add 20Hz vibrato
                            current_f += 40.0f * sinf(2.0f * M_PI * 25.0f * t);
                        }
                        sample = sinf(2.0f * M_PI * current_f * t) * env * vc->amp;
                        break;
                    }

                    case VOICE_NOISE_BURST: {
                        float env = powf(1.0f - progress, 2.5f);
                        float white = ((float)(rand() % 2000) - 1000.0f) / 1000.0f;
                        sample = white * env * vc->amp;
                        break;
                    }

                    case VOICE_MORSE: {
                        // Clean 750Hz sine with 5ms ramp
                        float env = 1.0f;
                        float time_ms = (float)vc->pos * 1000.0f / (float)AUDIO_SAMPLE_RATE;
                        float rem_ms = (float)(vc->length - vc->pos) * 1000.0f / (float)AUDIO_SAMPLE_RATE;
                        if (time_ms < 5.0f) env = time_ms / 5.0f;
                        else if (rem_ms < 5.0f) env = rem_ms / 5.0f;
                        sample = sinf(2.0f * M_PI * 750.0f * t) * env * vc->amp;
                        break;
                    }

                    case VOICE_R2D2: {
                        // FM modulated chirps
                        float f = vc->start_freq + 300.0f * sinf(2.0f * M_PI * 30.0f * t);
                        sample = sinf(2.0f * M_PI * f * t) * vc->amp;
                        break;
                    }

                    case VOICE_ENGINE_RUMBLE: {
                        // Low frequency pulse + noise
                        float pulse = sinf(2.0f * M_PI * vc->freq * t);
                        float n = ((float)(rand() % 1000) - 500.0f) / 1000.0f;
                        sample = (pulse * 0.7f + n * 0.3f) * vc->amp;
                        break;
                    }

                    case VOICE_ANIMAL: {
                        // Procedural animal formant modulation
                        float f = vc->start_freq + (vc->end_freq - vc->start_freq) * progress;
                        float env = sinf(progress * M_PI);
                        sample = sinf(2.0f * M_PI * f * t) * env * vc->amp;
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
            // 2-tone siren: 440Hz / 554Hz alternating every 0.35s
            float siren_f = ((fmodf(siren_t, 0.7f) < 0.35f) ? 440.0f : 554.0f);
            for (int i = 0; i < CHUNK_SAMPLES; i++) {
                float_mix[i] += sinf(2.0f * M_PI * siren_f * ((float)(s_siren_step + i) / AUDIO_SAMPLE_RATE)) * 0.45f;
            }
        }

        xSemaphoreGive(s_voice_mutex);

        // Convert float mix to 32-bit stereo PCM with soft limiter
        const float max_int32 = 2147480000.0f * s_master_volume;
        for (int i = 0; i < CHUNK_SAMPLES; i++) {
            float val = float_mix[i];
            // Soft clipping (tanh approximation)
            if (val > 1.0f) val = 1.0f;
            else if (val < -1.0f) val = -1.0f;

            int32_t pcm = (int32_t)(val * max_int32);
            mix_buffer[i * 2]     = pcm; // Left channel
            mix_buffer[i * 2 + 1] = pcm; // Right channel
        }

        // Write directly to I2S DMA
        size_t written = 0;
        i2s_channel_write(s_tx_handle, mix_buffer, CHUNK_SAMPLES * 2 * sizeof(int32_t), &written, portMAX_DELAY);
    }
}

esp_err_t audio_synth_init(void) {
    ESP_LOGI(TAG, "Initializing I2S MAX98357A on GPIO WS=%d, DIN=%d, BCLK=%d",
             PIN_I2S_LCK, PIN_I2S_DIN, PIN_I2S_BCK);

    s_voice_mutex = xSemaphoreCreateMutex();
    memset(s_voices, 0, sizeof(s_voices));

    // Ground SCK for PCM5102A compatibility
    gpio_config_t sck_conf = {
        .pin_bit_mask = (1ULL << PIN_I2S_SCK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&sck_conf);
    gpio_set_level((gpio_num_t)PIN_I2S_SCK, 0);

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

    // Standard Philips 32-bit slot width (64 BCLK ticks per stereo frame)
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

    // Launch synthesis task pinned to CPU Core 1 with high priority
    xTaskCreatePinnedToCore(audio_synth_task, "audio_synth", 8192, NULL, 6, NULL, 1);

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
    voice_t *v = alloc_voice();
    if (!v) return;
    v->mode = VOICE_TONE_ADSR;
    v->freq = freq_hz;
    v->amp = 0.85f;
    v->sub_type = timbre;
    v->length = (AUDIO_SAMPLE_RATE * 800) / 1000; // 800ms decay
}

// Mode change melodic chime
void audio_play_chime_mode(uint8_t mode_index) {
    float root_f = 440.0f + (mode_index * 55.0f);
    voice_t *v1 = alloc_voice();
    if (v1) {
        v1->mode = VOICE_TONE_ADSR;
        v1->freq = root_f;
        v1->amp = 0.7f;
        v1->sub_type = TIMBRE_MUSIC_BOX;
        v1->length = (AUDIO_SAMPLE_RATE * 250) / 1000;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    voice_t *v2 = alloc_voice();
    if (v2) {
        v2->mode = VOICE_TONE_ADSR;
        v2->freq = root_f * 1.5f;
        v2->amp = 0.8f;
        v2->sub_type = TIMBRE_MUSIC_BOX;
        v2->length = (AUDIO_SAMPLE_RATE * 400) / 1000;
    }
}

// Mode 2: Baby Mozart
void audio_play_mozart_next_note(void) {
    const note_t *curr_notes = s_mozart_playlist[s_current_song].notes;
    size_t count = s_mozart_playlist[s_current_song].count;

    note_t n = curr_notes[s_current_note_idx];
    s_current_note_idx = (s_current_note_idx + 1) % count;

    voice_t *v = alloc_voice();
    if (!v) return;
    v->mode = VOICE_TONE_ADSR;
    v->freq = n.freq;
    v->amp = 0.9f;
    v->sub_type = TIMBRE_PIANO;
    v->length = (AUDIO_SAMPLE_RATE * n.dur_ms * 2) / 1000;
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

    voice_t *v = alloc_voice();
    if (!v) return;
    v->mode = VOICE_TONE_ADSR;
    v->freq = f;
    v->amp = 0.8f;
    v->sub_type = TIMBRE_8BIT_MARIO;
    v->length = (AUDIO_SAMPLE_RATE * 180) / 1000;
}

void audio_speed_runner_idle_check(void) {
    if (s_last_speed_key_time > 0) {
        int64_t diff = esp_timer_get_time() - s_last_speed_key_time;
        if (diff > 500000 && diff < 600000) { // 500ms hesitation -> Skid sound!
            audio_play_cartoon_sfx(SFX_SKID_STOP);
            s_last_speed_key_time = 0; // prevent retrigger
        }
    }
}

// Mode 4: Drum Kit
void audio_play_drum(drum_type_t drum) {
    voice_t *v = alloc_voice();
    if (!v) return;

    switch (drum) {
        case DRUM_KICK: // Deep 150Hz -> 45Hz punch
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 160.0f;
            v->end_freq = 42.0f;
            v->amp = 1.0f;
            v->sub_type = 0;
            v->length = (AUDIO_SAMPLE_RATE * 140) / 1000;
            break;

        case DRUM_SNARE: // White noise + 180Hz pop
            v->mode = VOICE_NOISE_BURST;
            v->amp = 0.95f;
            v->length = (AUDIO_SAMPLE_RATE * 160) / 1000;
            // Pop underlay
            voice_t *vpop = alloc_voice();
            if (vpop) {
                vpop->mode = VOICE_TONE_ADSR;
                vpop->freq = 190.0f;
                vpop->amp = 0.7f;
                vpop->sub_type = TIMBRE_PIANO;
                vpop->length = (AUDIO_SAMPLE_RATE * 80) / 1000;
            }
            break;

        case DRUM_HIHAT: // Short metallic tick
            v->mode = VOICE_NOISE_BURST;
            v->amp = 0.55f;
            v->length = (AUDIO_SAMPLE_RATE * 45) / 1000;
            break;

        case DRUM_CYMBAL: // Long shimmer
            v->mode = VOICE_NOISE_BURST;
            v->amp = 0.85f;
            v->length = (AUDIO_SAMPLE_RATE * 450) / 1000;
            break;

        case DRUM_TOM: // 220Hz -> 90Hz
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 220.0f;
            v->end_freq = 90.0f;
            v->amp = 0.9f;
            v->length = (AUDIO_SAMPLE_RATE * 180) / 1000;
            break;

        case DRUM_CLAP: // Crisp handclap
            v->mode = VOICE_NOISE_BURST;
            v->amp = 0.9f;
            v->length = (AUDIO_SAMPLE_RATE * 120) / 1000;
            break;
    }
}

// Mode 5: Vintage Typewriter
void audio_play_typewriter(typewriter_sound_t type) {
    voice_t *v = alloc_voice();
    if (!v) return;

    switch (type) {
        case TYPEWRITER_KEY_CLICK:
            v->mode = VOICE_NOISE_BURST;
            v->amp = 0.75f;
            v->length = (AUDIO_SAMPLE_RATE * 25) / 1000; // Sharp 25ms mechanical snap
            break;

        case TYPEWRITER_SPACE_SLIDE:
            v->mode = VOICE_NOISE_BURST;
            v->amp = 0.5f;
            v->length = (AUDIO_SAMPLE_RATE * 140) / 1000;
            break;

        case TYPEWRITER_ENTER_BELL: // Crisp bell "KENGGG"
            v->mode = VOICE_TONE_ADSR;
            v->freq = 2489.0f; // High D#7 bell
            v->amp = 0.95f;
            v->sub_type = TIMBRE_MUSIC_BOX;
            v->length = (AUDIO_SAMPLE_RATE * 650) / 1000;
            break;

        case TYPEWRITER_BACKSPACE:
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 300.0f;
            v->end_freq = 120.0f;
            v->amp = 0.6f;
            v->length = (AUDIO_SAMPLE_RATE * 90) / 1000;
            break;
    }
}

// Mode 6: Cartoon SFX
void audio_play_cartoon_sfx(cartoon_sfx_t sfx) {
    voice_t *v = alloc_voice();
    if (!v) return;

    switch (sfx) {
        case SFX_MARIO_COIN: // B5 (987Hz) then E6 (1318Hz)
            v->mode = VOICE_TONE_ADSR;
            v->freq = 987.77f;
            v->amp = 0.85f;
            v->sub_type = TIMBRE_8BIT_MARIO;
            v->length = (AUDIO_SAMPLE_RATE * 80) / 1000;
            // Second high note
            voice_t *v2 = alloc_voice();
            if (v2) {
                v2->mode = VOICE_TONE_ADSR;
                v2->freq = 1318.51f;
                v2->amp = 0.95f;
                v2->sub_type = TIMBRE_8BIT_MARIO;
                v2->length = (AUDIO_SAMPLE_RATE * 320) / 1000;
            }
            break;

        case SFX_SPRING_BOING: // 250Hz -> 650Hz with 25Hz vibrato
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 220.0f;
            v->end_freq = 680.0f;
            v->amp = 0.9f;
            v->sub_type = 1; // Vibrato flag
            v->length = (AUDIO_SAMPLE_RATE * 280) / 1000;
            break;

        case SFX_LASER: // 2500Hz -> 200Hz in 130ms
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 2600.0f;
            v->end_freq = 180.0f;
            v->amp = 0.85f;
            v->sub_type = 0;
            v->length = (AUDIO_SAMPLE_RATE * 130) / 1000;
            break;

        case SFX_CAR_HORN: // Dual chord 440Hz + 554Hz
            v->mode = VOICE_TONE_ADSR;
            v->freq = 440.0f;
            v->amp = 0.7f;
            v->sub_type = TIMBRE_8BIT_MARIO;
            v->length = (AUDIO_SAMPLE_RATE * 220) / 1000;
            voice_t *vh2 = alloc_voice();
            if (vh2) {
                vh2->mode = VOICE_TONE_ADSR;
                vh2->freq = 554.37f;
                vh2->amp = 0.7f;
                vh2->sub_type = TIMBRE_8BIT_MARIO;
                vh2->length = (AUDIO_SAMPLE_RATE * 220) / 1000;
            }
            break;

        case SFX_VICTORY_FANFARE: // Rapid C5-E5-G5-C6
            v->mode = VOICE_TONE_ADSR;
            v->freq = 523.25f;
            v->amp = 0.85f;
            v->sub_type = TIMBRE_8BIT_MARIO;
            v->length = (AUDIO_SAMPLE_RATE * 150) / 1000;
            break;

        case SFX_WHISTLE_SLIDE: // Smooth slide down 1200Hz -> 300Hz
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 1200.0f;
            v->end_freq = 280.0f;
            v->amp = 0.85f;
            v->sub_type = 0;
            v->length = (AUDIO_SAMPLE_RATE * 350) / 1000;
            break;

        case SFX_WATER_DROP: // Upward bubble pop 600Hz -> 1800Hz
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 600.0f;
            v->end_freq = 1900.0f;
            v->amp = 0.85f;
            v->sub_type = 0;
            v->length = (AUDIO_SAMPLE_RATE * 90) / 1000;
            break;

        case SFX_DOORBELL: // Ding-Dong (G5 -> E5)
            v->mode = VOICE_TONE_ADSR;
            v->freq = 783.99f;
            v->amp = 0.9f;
            v->sub_type = TIMBRE_MUSIC_BOX;
            v->length = (AUDIO_SAMPLE_RATE * 300) / 1000;
            voice_t *vd2 = alloc_voice();
            if (vd2) {
                vd2->mode = VOICE_TONE_ADSR;
                vd2->freq = 659.25f;
                vd2->amp = 0.9f;
                vd2->sub_type = TIMBRE_MUSIC_BOX;
                vd2->length = (AUDIO_SAMPLE_RATE * 500) / 1000;
            }
            break;

        case SFX_FIRE_SIREN:
            audio_cockpit_siren_toggle();
            break;

        case SFX_SKID_STOP:
            v->mode = VOICE_PITCH_SWEEP;
            v->start_freq = 1800.0f;
            v->end_freq = 400.0f;
            v->amp = 0.8f;
            v->sub_type = 0;
            v->length = (AUDIO_SAMPLE_RATE * 200) / 1000;
            break;
    }
}

// Mode 7: Morse Code & Robot
static const char *s_morse_table[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",   // A-I
    ".---", "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",  // J-R
    "...",  "-",    "..-",  "...-", ".--",  "-..-", "-.--", "--.."          // S-Z
};

void audio_play_morse_char(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < 'A' || c > 'Z') return;

    const char *morse = s_morse_table[c - 'A'];
    while (*morse) {
        voice_t *v = alloc_voice();
        if (v) {
            v->mode = VOICE_MORSE;
            v->amp = 0.85f;
            v->length = (*morse == '.') ? (AUDIO_SAMPLE_RATE * 60) / 1000 : (AUDIO_SAMPLE_RATE * 180) / 1000;
        }
        vTaskDelay(pdMS_TO_TICKS((*morse == '.') ? 100 : 220));
        morse++;
    }
}

void audio_play_robot_chirp(void) {
    // Generate 4-6 randomized FM chirps
    int num_chirps = 3 + (rand() % 4);
    for (int i = 0; i < num_chirps; i++) {
        voice_t *v = alloc_voice();
        if (v) {
            v->mode = VOICE_R2D2;
            v->start_freq = 1800.0f + (rand() % 2400);
            v->amp = 0.8f;
            v->length = (AUDIO_SAMPLE_RATE * (30 + rand() % 50)) / 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(50 + rand() % 40));
    }
}

// Mode 8: Cockpit & Animals
void audio_cockpit_engine_start(void) {
    voice_t *v = alloc_voice();
    if (!v) return;
    v->mode = VOICE_ENGINE_RUMBLE;
    v->freq = 55.0f; // Low Vroom
    v->amp = 0.95f;
    v->length = (AUDIO_SAMPLE_RATE * 400) / 1000;
}

void audio_cockpit_engine_stop(void) {
    // optional idle rumble
}

void audio_cockpit_siren_toggle(void) {
    s_siren_on = !s_siren_on;
    ESP_LOGI(TAG, "Cockpit Siren: %s", s_siren_on ? "ON" : "OFF");
}

bool audio_cockpit_is_siren_on(void) {
    return s_siren_on;
}

void audio_cockpit_rocket_launch(void) {
    // 3... 2... 1... Blast Off!
    for (int i = 3; i >= 1; i--) {
        voice_t *v = alloc_voice();
        if (v) {
            v->mode = VOICE_TONE_ADSR;
            v->freq = 880.0f;
            v->amp = 0.9f;
            v->sub_type = TIMBRE_8BIT_MARIO;
            v->length = (AUDIO_SAMPLE_RATE * 150) / 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    // Thruster roar
    voice_t *vroar = alloc_voice();
    if (vroar) {
        vroar->mode = VOICE_PITCH_SWEEP;
        vroar->start_freq = 80.0f;
        vroar->end_freq = 400.0f;
        vroar->amp = 1.0f;
        vroar->length = (AUDIO_SAMPLE_RATE * 800) / 1000;
    }
}

void audio_play_animal_sound(char key) {
    if (key >= 'a' && key <= 'z') key -= 32;
    voice_t *v = alloc_voice();
    if (!v) return;

    v->mode = VOICE_ANIMAL;
    v->amp = 0.9f;

    switch (key) {
        case 'D': // Dog: Woof! (250Hz -> 180Hz)
            v->start_freq = 280.0f;
            v->end_freq = 150.0f;
            v->length = (AUDIO_SAMPLE_RATE * 160) / 1000;
            break;
        case 'C': // Cat: Meoowww (500Hz -> 850Hz)
            v->start_freq = 450.0f;
            v->end_freq = 900.0f;
            v->length = (AUDIO_SAMPLE_RATE * 350) / 1000;
            break;
        case 'B': // Bird: Chirp chirp (2800Hz -> 3600Hz)
            v->start_freq = 2800.0f;
            v->end_freq = 3800.0f;
            v->length = (AUDIO_SAMPLE_RATE * 100) / 1000;
            break;
        case 'M': // Cow: Moooo (140Hz)
            v->start_freq = 150.0f;
            v->end_freq = 120.0f;
            v->length = (AUDIO_SAMPLE_RATE * 500) / 1000;
            break;
        case 'Q': // Duck: Quack! (320Hz formants)
            v->start_freq = 340.0f;
            v->end_freq = 260.0f;
            v->length = (AUDIO_SAMPLE_RATE * 200) / 1000;
            break;
        case 'F': // Frog: Croak! (120Hz)
            v->start_freq = 140.0f;
            v->end_freq = 90.0f;
            v->length = (AUDIO_SAMPLE_RATE * 180) / 1000;
            break;
        case 'L': // Lion: Roar!
            v->start_freq = 180.0f;
            v->end_freq = 60.0f;
            v->length = (AUDIO_SAMPLE_RATE * 600) / 1000;
            break;
        case 'P': // Pig: Oink!
            v->start_freq = 400.0f;
            v->end_freq = 250.0f;
            v->length = (AUDIO_SAMPLE_RATE * 150) / 1000;
            break;
        default: // Generic cute chirping
            v->start_freq = 400.0f + (key % 10) * 100.0f;
            v->end_freq = 800.0f + (key % 10) * 50.0f;
            v->length = (AUDIO_SAMPLE_RATE * 200) / 1000;
            break;
    }
}

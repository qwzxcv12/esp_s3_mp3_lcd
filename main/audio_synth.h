#ifndef AUDIO_SYNTH_H
#define AUDIO_SYNTH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Hardware pinout for MAX98357A on ESP32-S3
#define PIN_I2S_LCK    1   // Word Select / LRCK / WS
#define PIN_I2S_DIN    2   // Serial Data / DOUT / DIN
#define PIN_I2S_BCK    3   // Bit Clock / BCLK

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_DEFAULT_VOLUME 0.40f

typedef enum {
    TIMBRE_PIANO = 0,
    TIMBRE_8BIT_MARIO,
    TIMBRE_MUSIC_BOX
} piano_timbre_t;

typedef enum {
    DRUM_KICK = 0,
    DRUM_SNARE,
    DRUM_HIHAT,
    DRUM_CYMBAL,
    DRUM_TOM,
    DRUM_CLAP
} drum_type_t;

typedef enum {
    SFX_MARIO_COIN = 0,
    SFX_SPRING_BOING,
    SFX_LASER,
    SFX_CAR_HORN,
    SFX_VICTORY_FANFARE,
    SFX_WHISTLE_SLIDE,
    SFX_WATER_DROP,
    SFX_DOORBELL,
    SFX_FIRE_SIREN,
    SFX_SKID_STOP
} cartoon_sfx_t;

typedef enum {
    TYPEWRITER_KEY_CLICK = 0,
    TYPEWRITER_SPACE_SLIDE,
    TYPEWRITER_ENTER_BELL,
    TYPEWRITER_BACKSPACE
} typewriter_sound_t;

// Init & Controls
esp_err_t audio_synth_init(void);
void audio_synth_set_volume(float vol);
float audio_synth_get_volume(void);
void audio_synth_volume_up(void);
void audio_synth_volume_down(void);

// Mode 1: Piano & Pentatonic
void audio_play_piano_note(float freq_hz, piano_timbre_t timbre);
void audio_play_chime_mode(uint8_t mode_index);

// Mode 2: Baby Mozart
void audio_play_mozart_next_note(void);
void audio_mozart_next_song(void);

// Mode 3: Speed Runner
void audio_speed_runner_keystroke(void);
void audio_speed_runner_idle_check(void);

// Mode 4: Drum Kit
void audio_play_drum(drum_type_t drum);

// Mode 5: Vintage Typewriter
void audio_play_typewriter(typewriter_sound_t type);

// Mode 6: Cartoon SFX
void audio_play_cartoon_sfx(cartoon_sfx_t sfx);

// Mode 7: Morse Code & Robot
void audio_play_morse_char(char c);
void audio_play_robot_chirp(void);

// Mode 8: Cockpit & Animals
void audio_cockpit_engine_start(void);
void audio_cockpit_engine_stop(void);
void audio_cockpit_siren_toggle(void);
bool audio_cockpit_is_siren_on(void);
void audio_cockpit_rocket_launch(void);
void audio_play_animal_sound(char key);

// Mode 9: Phonics & Counting Learning
void audio_play_phonics_letter(char c);
void audio_play_counting_number(uint8_t num);

#endif // AUDIO_SYNTH_H

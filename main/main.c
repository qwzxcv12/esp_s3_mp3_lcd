/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "errno.h"
#include "driver/gpio.h"

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "audio_synth.h"
#include "led_effects.h"

static const char *TAG = "usb_toy";

QueueHandle_t app_event_queue = NULL;

typedef enum {
    APP_EVENT_HID_HOST = 0,
    APP_EVENT_UPDATE_LEDS
} app_event_group_t;

typedef struct {
    app_event_group_t event_group;
    struct {
        hid_host_device_handle_t handle;
        hid_host_driver_event_t event;
        void *arg;
    } hid_host_device;
} app_event_queue_t;

static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"
};

typedef struct {
    enum key_state {
        KEY_STATE_PRESSED = 0x00,
        KEY_STATE_RELEASED = 0x01
    } state;
    uint8_t modifier;
    uint8_t key_code;
} key_event_t;

#define KEYBOARD_ENTER_MAIN_CHAR    '\r'
#define KEYBOARD_ENTER_LF_EXTEND    1

const uint8_t keycode2ascii [57][2] = {
    {0, 0}, /* HID_KEY_NO_PRESS        */
    {0, 0}, /* HID_KEY_ROLLOVER        */
    {0, 0}, /* HID_KEY_POST_FAIL       */
    {0, 0}, /* HID_KEY_ERROR_UNDEFINED */
    {'a', 'A'}, /* HID_KEY_A               */
    {'b', 'B'}, /* HID_KEY_B               */
    {'c', 'C'}, /* HID_KEY_C               */
    {'d', 'D'}, /* HID_KEY_D               */
    {'e', 'E'}, /* HID_KEY_E               */
    {'f', 'F'}, /* HID_KEY_F               */
    {'g', 'G'}, /* HID_KEY_G               */
    {'h', 'H'}, /* HID_KEY_H               */
    {'i', 'I'}, /* HID_KEY_I               */
    {'j', 'J'}, /* HID_KEY_J               */
    {'k', 'K'}, /* HID_KEY_K               */
    {'l', 'L'}, /* HID_KEY_L               */
    {'m', 'M'}, /* HID_KEY_M               */
    {'n', 'N'}, /* HID_KEY_N               */
    {'o', 'O'}, /* HID_KEY_O               */
    {'p', 'P'}, /* HID_KEY_P               */
    {'q', 'Q'}, /* HID_KEY_Q               */
    {'r', 'R'}, /* HID_KEY_R               */
    {'s', 'S'}, /* HID_KEY_S               */
    {'t', 'T'}, /* HID_KEY_T               */
    {'u', 'U'}, /* HID_KEY_U               */
    {'v', 'V'}, /* HID_KEY_V               */
    {'w', 'W'}, /* HID_KEY_W               */
    {'x', 'X'}, /* HID_KEY_X               */
    {'y', 'Y'}, /* HID_KEY_Y               */
    {'z', 'Z'}, /* HID_KEY_Z               */
    {'1', '!'}, /* HID_KEY_1               */
    {'2', '@'}, /* HID_KEY_2               */
    {'3', '#'}, /* HID_KEY_3               */
    {'4', '$'}, /* HID_KEY_4               */
    {'5', '%'}, /* HID_KEY_5               */
    {'6', '^'}, /* HID_KEY_6               */
    {'7', '&'}, /* HID_KEY_7               */
    {'8', '*'}, /* HID_KEY_8               */
    {'9', '('}, /* HID_KEY_9               */
    {'0', ')'}, /* HID_KEY_0               */
    {KEYBOARD_ENTER_MAIN_CHAR, KEYBOARD_ENTER_MAIN_CHAR}, /* HID_KEY_ENTER           */
    {0, 0}, /* HID_KEY_ESC             */
    {'\b', '\b'}, /* HID_KEY_DEL (Backspace) */
    {'\t', '\t'}, /* HID_KEY_TAB             */
    {' ', ' '}, /* HID_KEY_SPACE           */
    {'-', '_'}, /* HID_KEY_MINUS           */
    {'=', '+'}, /* HID_KEY_EQUAL           */
    {'[', '{'}, /* HID_KEY_OPEN_BRACKET    */
    {']', '}'}, /* HID_KEY_CLOSE_BRACKET   */
    {'\\', '|'}, /* HID_KEY_BACK_SLASH      */
    {'\\', '|'}, /* HID_KEY_SHARP           */
    {';', ':'}, /* HID_KEY_COLON           */
    {'\'', '"'}, /* HID_KEY_QUOTE           */
    {'`', '~'}, /* HID_KEY_TILDE           */
    {',', '<'}, /* HID_KEY_LESS            */
    {'.', '>'}, /* HID_KEY_GREATER         */
    {'/', '?'} /* HID_KEY_SLASH           */
};

// -------------------------------------------------------------
// 8-MODE STATE MACHINE DEFINITIONS
// -------------------------------------------------------------
typedef enum {
    MODE_1_PIANO = 0,
    MODE_2_BABY_MOZART,
    MODE_3_SPEED_RUNNER,
    MODE_4_KIDS_DRUMMER,
    MODE_5_TYPEWRITER,
    MODE_6_CARTOON_SFX,
    MODE_7_SECRET_AGENT_ROBOT,
    MODE_8_COCKPIT_ANIMALS,
    MODE_MAX_COUNT
} toy_mode_t;

static const char *s_mode_names[MODE_MAX_COUNT] = {
    "1. ĐÀN PIANO DIỆU KỲ & NGŨ CUNG",
    "2. NGHỆ SĨ THIÊN TÀI (BABY MOZART)",
    "3. NHẠC CHẠY THEO TỐC ĐỘ TAY",
    "4. DÀN TRỐNG BEAT-MAKER & DJ",
    "5. MÁY GÕ CHỮ CỔ ĐIỂN",
    "6. HỘP ÂM THANH GAME & HOẠT HÌNH",
    "7. ĐIỆP VIÊN NHÍ & ROBOT R2-D2",
    "8. BUỒNG LÁI XE CỨU HỎA & ĐẠI NHẠC HỘI CON VẬT"
};

static toy_mode_t s_current_mode = MODE_1_PIANO;
static piano_timbre_t s_piano_timbre = TIMBRE_PIANO;
static bool s_pentatonic_mode = false;

// Hardware LEDs on keyboard
static hid_host_device_handle_t s_kb_handle = NULL;
static bool s_caps_lock = false;
static bool s_num_lock = true;
static bool s_scroll_lock = false;

static inline void trigger_led_update(void) {
    if (app_event_queue) {
        app_event_queue_t evt = { .event_group = APP_EVENT_UPDATE_LEDS };
        xQueueSend(app_event_queue, &evt, 0);
    }
}

static void update_keyboard_leds(void) {
    if (s_kb_handle == NULL) return;
    uint8_t led_report = 0;
    if (s_num_lock)    led_report |= 0x01;
    if (s_caps_lock)   led_report |= 0x02;
    if (s_scroll_lock) led_report |= 0x04;

    hid_class_request_set_report(s_kb_handle,
                                 HID_REPORT_TYPE_OUTPUT,
                                 0,
                                 &led_report,
                                 1);
}

static inline bool hid_keyboard_is_modifier_shift(uint8_t modifier) {
    return (((modifier & 0x02) == 0x02) || ((modifier & 0x20) == 0x20));
}

static void switch_toy_mode(toy_mode_t new_mode) {
    s_current_mode = new_mode % MODE_MAX_COUNT;
    printf("\r\n\r\n========================================================\r\n");
    printf(">>> [CHẾ ĐỘ %s] <<<\r\n", s_mode_names[s_current_mode]);
    printf("========================================================\r\n");
    fflush(stdout);

    // Audio chime & LED animation
    audio_play_chime_mode(s_current_mode);
    led_effects_mode_switch(s_current_mode);
}

// -------------------------------------------------------------
// TOY KEY EVENT HANDLER (ROUTES ACROSS 8 MODES)
// -------------------------------------------------------------
static void handle_toy_mode_key(uint8_t kc, uint8_t mod, char c) {
    // 1. GLOBAL SYSTEM SHORTCUTS (ACTIVE IN ALL MODES)
    if (kc == 0x29) { // Phím ESC -> Đổi chế độ tiếp theo!
        switch_toy_mode((toy_mode_t)((s_current_mode + 1) % MODE_MAX_COUNT));
        return;
    }
    if (kc >= 0x3A && kc <= 0x41) { // F1 - F8: Nhảy thẳng vào Chế độ 1 - 8
        switch_toy_mode((toy_mode_t)(kc - 0x3A));
        return;
    }
    if (kc == 0x44) { // F11: Giảm âm lượng
        audio_synth_volume_down();
        printf("[VOL: %d%%] ", (int)(audio_synth_get_volume() * 100));
        fflush(stdout);
        return;
    }
    if (kc == 0x45) { // F12: Tăng âm lượng
        audio_synth_volume_up();
        printf("[VOL: %d%%] ", (int)(audio_synth_get_volume() * 100));
        fflush(stdout);
        return;
    }

    // 2. DISPATCH EVENT BASED ON CURRENT MODE
    switch (s_current_mode) {

        // --- MODE 1: ĐÀN PIANO DIỆU KỲ & NGŨ CUNG ---
        case MODE_1_PIANO: {
            if (kc == 0x2B) { // Phím TAB: Bật / Tắt Thang âm Ngũ Cung chống phô
                s_pentatonic_mode = !s_pentatonic_mode;
                printf("[NGŨ CUNG PENTATONIC: %s] ", s_pentatonic_mode ? "BẬT" : "TẮT");
                fflush(stdout);
                audio_play_cartoon_sfx(SFX_MARIO_COIN);
                led_effects_color_burst(255, 215, 0);
                return;
            }
            if (c == '1') { s_piano_timbre = TIMBRE_PIANO; printf("[TIMBRE: PIANO] "); fflush(stdout); audio_play_piano_note(523.25f, s_piano_timbre); return; }
            if (c == '2') { s_piano_timbre = TIMBRE_8BIT_MARIO; printf("[TIMBRE: 8-BIT] "); fflush(stdout); audio_play_piano_note(523.25f, s_piano_timbre); return; }
            if (c == '3') { s_piano_timbre = TIMBRE_MUSIC_BOX; printf("[TIMBRE: MUSIC BOX] "); fflush(stdout); audio_play_piano_note(523.25f, s_piano_timbre); return; }

            // Thang âm trắng chuẩn: C4 -> E5
            static const float white_freqs[10] = {
                261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f, 523.25f, 587.33f, 659.25f
            };
            // Thang âm Ngũ Cung (Pentatonic C-D-E-G-A): không bao giờ chói gắt
            static const float penta_freqs[10] = {
                261.63f, 293.66f, 329.63f, 392.00f, 440.00f, 523.25f, 587.33f, 659.25f, 783.99f, 880.00f
            };

            int note_idx = -1;
            float freq = 0.0f;

            // Hàng A - S - D - F - G - H - J - K - L - ;
            switch (c) {
                case 'a': case 'A': note_idx = 0; break;
                case 's': case 'S': note_idx = 1; break;
                case 'd': case 'D': note_idx = 2; break;
                case 'f': case 'F': note_idx = 3; break;
                case 'g': case 'G': note_idx = 4; break;
                case 'h': case 'H': note_idx = 5; break;
                case 'j': case 'J': note_idx = 6; break;
                case 'k': case 'K': note_idx = 7; break;
                case 'l': case 'L': note_idx = 8; break;
                case ';':           note_idx = 9; break;
                // Nốt thăng giáng (Hàng W - E - T - Y - U - O - P)
                case 'w': case 'W': freq = 277.18f; note_idx = 0; break; // C#4
                case 'e': case 'E': freq = 311.13f; note_idx = 1; break; // D#4
                case 't': case 'T': freq = 369.99f; note_idx = 3; break; // F#4
                case 'y': case 'Y': freq = 415.30f; note_idx = 4; break; // G#4
                case 'u': case 'U': freq = 466.16f; note_idx = 5; break; // A#4
                case 'o': case 'O': freq = 554.37f; note_idx = 7; break; // C#5
                case 'p': case 'P': freq = 622.25f; note_idx = 8; break; // D#5
                default: break;
            }

            if (note_idx >= 0) {
                if (freq == 0.0f) {
                    freq = s_pentatonic_mode ? penta_freqs[note_idx] : white_freqs[note_idx];
                }
                audio_play_piano_note(freq, s_piano_timbre);
                // 7 sắc cầu vồng theo nốt nhạc
                static const uint8_t colors[10][3] = {
                    {255, 0, 0}, {255, 128, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255},
                    {0, 100, 255}, {150, 0, 255}, {255, 0, 150}, {255, 150, 150}, {255, 255, 255}
                };
                led_effects_note(note_idx, 10, colors[note_idx][0], colors[note_idx][1], colors[note_idx][2]);
            }
            break;
        }

        // --- MODE 2: NGHỆ SĨ THIÊN TÀI (BABY MOZART - 1 KEY 1 NOTE) ---
        case MODE_2_BABY_MOZART: {
            if (c == ' ') {
                audio_mozart_next_song();
                led_effects_mode_switch(1);
            } else {
                audio_play_mozart_next_note();
                led_effects_color_burst(rand() % 256, rand() % 256, rand() % 256);
            }
            break;
        }

        // --- MODE 3: NHẠC CHẠY THEO TỐC ĐỘ TAY ---
        case MODE_3_SPEED_RUNNER: {
            audio_speed_runner_keystroke();
            led_effects_beat();
            break;
        }

        // --- MODE 4: DÀN TRỐNG BEAT-MAKER & DJ ---
        case MODE_4_KIDS_DRUMMER: {
            if (c == ' ' || c == 'b' || c == 'B') {
                audio_play_drum(DRUM_KICK);
                led_effects_beat();
            } else if (c == 's' || c == 'S') {
                audio_play_drum(DRUM_SNARE);
                led_effects_color_burst(255, 255, 255);
            } else if (c == 'h' || c == 'H') {
                audio_play_drum(DRUM_HIHAT);
                led_effects_color_burst(255, 255, 0);
            } else if (c == 'c' || c == 'C') {
                audio_play_drum(DRUM_CYMBAL);
                led_effects_color_burst(0, 255, 255);
            } else if (c == 't' || c == 'T') {
                audio_play_drum(DRUM_TOM);
                led_effects_color_burst(255, 128, 0);
            } else if (c == '\r' || c == '\n') {
                audio_play_drum(DRUM_CLAP);
                led_effects_color_burst(255, 0, 128);
            } else {
                audio_play_drum((drum_type_t)(kc % 6));
                led_effects_spark();
            }
            break;
        }

        // --- MODE 5: MÁY GÕ CHỮ CỔ ĐIỂN ---
        case MODE_5_TYPEWRITER: {
            if (c == ' ') {
                audio_play_typewriter(TYPEWRITER_SPACE_SLIDE);
                led_effects_spark();
            } else if (c == '\r' || c == '\n') {
                audio_play_typewriter(TYPEWRITER_ENTER_BELL);
                led_effects_color_burst(255, 215, 0);
            } else if (c == '\b') {
                audio_play_typewriter(TYPEWRITER_BACKSPACE);
            } else {
                audio_play_typewriter(TYPEWRITER_KEY_CLICK);
                led_effects_spark();
            }
            break;
        }

        // --- MODE 6: HỘP ÂM THANH GAME & HOẠT HÌNH ---
        case MODE_6_CARTOON_SFX: {
            if (c == '1' || c == '\r') { audio_play_cartoon_sfx(SFX_MARIO_COIN); led_effects_color_burst(255, 215, 0); }
            else if (c == '2' || c == ' ') { audio_play_cartoon_sfx(SFX_SPRING_BOING); led_effects_color_burst(50, 255, 50); }
            else if (c == '3') { audio_play_cartoon_sfx(SFX_LASER); led_effects_color_burst(255, 30, 30); }
            else if (c == '4') { audio_play_cartoon_sfx(SFX_CAR_HORN); led_effects_color_burst(255, 160, 0); }
            else if (c == '5') { audio_play_cartoon_sfx(SFX_VICTORY_FANFARE); led_effects_mode_switch(5); }
            else if (c == '6') { audio_play_cartoon_sfx(SFX_WHISTLE_SLIDE); led_effects_color_burst(0, 200, 255); }
            else if (c == '7') { audio_play_cartoon_sfx(SFX_WATER_DROP); led_effects_color_burst(30, 100, 255); }
            else if (c == '8') { audio_play_cartoon_sfx(SFX_DOORBELL); led_effects_color_burst(200, 50, 255); }
            else if (c == '9') { audio_play_cartoon_sfx(SFX_FIRE_SIREN); }
            else { audio_play_cartoon_sfx((cartoon_sfx_t)(kc % 9)); led_effects_spark(); }
            break;
        }

        // --- MODE 7: ĐIỆP VIÊN NHÍ & ROBOT R2-D2 ---
        case MODE_7_SECRET_AGENT_ROBOT: {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                audio_play_morse_char(c);
                led_effects_matrix();
            } else {
                audio_play_robot_chirp();
                led_effects_matrix();
            }
            break;
        }

        // --- MODE 8: BUỒNG LÁI XE CỨU HỎA & ĐẠI NHẠC HỘI CON VẬT ---
        case MODE_8_COCKPIT_ANIMALS: {
            if (c == ' ') {
                audio_cockpit_engine_start();
                led_effects_color_burst(255, 100, 0);
            } else if (c == 's' || c == 'S') {
                audio_cockpit_siren_toggle();
            } else if (c == '\r' || c == '\n') {
                audio_cockpit_rocket_launch();
                led_effects_color_burst(255, 0, 0);
            } else {
                audio_play_animal_sound(c);
                led_effects_color_burst(0, 255, 128);
            }
            break;
        }

        default:
            break;
    }
}

// -------------------------------------------------------------
// HID KEY EVENT CALLBACK
// -------------------------------------------------------------
static void print_key_combo(uint8_t mod, uint8_t kc);

static void key_event_callback(key_event_t *key_event)
{
    if (KEY_STATE_PRESSED != key_event->state) {
        return;
    }

    uint8_t kc = key_event->key_code;
    uint8_t mod = key_event->modifier;
    bool is_shift = hid_keyboard_is_modifier_shift(mod);

    // Xử lý tổ hợp Ctrl / Alt / Win
    bool has_ctrl = (mod & 0x11) != 0;
    bool has_alt  = (mod & 0x44) != 0;
    bool has_gui  = (mod & 0x88) != 0;

    if (has_ctrl || has_alt || has_gui) {
        print_key_combo(mod, kc);
        return;
    }

    char c = 0;

    // 1. Letters: 'a' - 'z' (0x04 - 0x1D)
    if (kc >= 0x04 && kc <= 0x1D) {
        bool upper = is_shift ^ s_caps_lock;
        c = upper ? ('A' + (kc - 0x04)) : ('a' + (kc - 0x04));
        putchar(c);
        fflush(stdout);
        handle_toy_mode_key(kc, mod, c);
        return;
    }

    // 2. Numbers & symbols
    if (kc >= 0x1E && kc <= 0x38) {
        uint8_t shift_idx = is_shift ? 1 : 0;
        c = keycode2ascii[kc][shift_idx];
        if (c == KEYBOARD_ENTER_MAIN_CHAR) {
            printf("\r\n");
        } else if (c == '\b') {
            printf("\b \b");
        } else if (c) {
            putchar(c);
        }
        fflush(stdout);
        handle_toy_mode_key(kc, mod, c);
        return;
    }

    // 3. Special keys
    if (kc == 0x29) { // Escape
        printf("[ESC]");
        fflush(stdout);
        handle_toy_mode_key(kc, mod, 0);
        return;
    }
    if (kc == 0x2B) { // Tab
        putchar('\t');
        fflush(stdout);
        handle_toy_mode_key(kc, mod, '\t');
        return;
    }
    if (kc == 0x39) { // CapsLock
        s_caps_lock = !s_caps_lock;
        trigger_led_update();
        return;
    }

    // 4. Function keys (F1 - F12)
    if (kc >= 0x3A && kc <= 0x45) {
        printf("[F%d]", (kc - 0x3A + 1));
        fflush(stdout);
        handle_toy_mode_key(kc, mod, 0);
        return;
    }

    // 5. Navigation & Editing keys
    switch (kc) {
        case 0x46: printf("[PRTSC]"); fflush(stdout); return;
        case 0x47: 
            s_scroll_lock = !s_scroll_lock;
            trigger_led_update();
            printf("[SCRLK]"); 
            fflush(stdout); 
            return;
        case 0x48: printf("[PAUSE]"); fflush(stdout); return;
        case 0x49: printf("[INS]"); fflush(stdout); return;
        case 0x4A: printf("[HOME]"); fflush(stdout); return;
        case 0x4B: printf("[PGUP]"); fflush(stdout); return;
        case 0x4C: printf("[DEL]"); fflush(stdout); return;
        case 0x4D: printf("[END]"); fflush(stdout); return;
        case 0x4E: printf("[PGDN]"); fflush(stdout); return;
        case 0x4F: printf("[RIGHT]"); fflush(stdout); handle_toy_mode_key(kc, mod, 0); return;
        case 0x50: printf("[LEFT]"); fflush(stdout); handle_toy_mode_key(kc, mod, 0); return;
        case 0x51: printf("[DOWN]"); fflush(stdout); handle_toy_mode_key(kc, mod, 0); return;
        case 0x52: printf("[UP]"); fflush(stdout); handle_toy_mode_key(kc, mod, 0); return;
        case 0x53: // NumLock
            s_num_lock = !s_num_lock;
            trigger_led_update();
            return;
        default:
            break;
    }

    // 6. Numpad keys (0x54 - 0x63)
    if (kc >= 0x54 && kc <= 0x63) {
        switch (kc) {
            case 0x54: c = '/'; break;
            case 0x55: c = '*'; break;
            case 0x56: c = '-'; break;
            case 0x57: c = '+'; break;
            case 0x58: c = '\r'; break;
            case 0x59: c = s_num_lock ? '1' : 0; break;
            case 0x5A: c = s_num_lock ? '2' : 0; break;
            case 0x5B: c = s_num_lock ? '3' : 0; break;
            case 0x5C: c = s_num_lock ? '4' : 0; break;
            case 0x5D: c = s_num_lock ? '5' : 0; break;
            case 0x5E: c = s_num_lock ? '6' : 0; break;
            case 0x5F: c = s_num_lock ? '7' : 0; break;
            case 0x60: c = s_num_lock ? '8' : 0; break;
            case 0x61: c = s_num_lock ? '9' : 0; break;
            case 0x62: c = s_num_lock ? '0' : 0; break;
            case 0x63: c = s_num_lock ? '.' : 0; break;
        }
        if (c == '\r') {
            printf("\r\n");
        } else if (c) {
            putchar(c);
        }
        fflush(stdout);
        handle_toy_mode_key(kc, mod, c);
        return;
    }

    handle_toy_mode_key(kc, mod, 0);
}

static void print_key_combo(uint8_t mod, uint8_t kc)
{
    printf("[");
    bool first = true;
    if (mod & 0x11) { printf("CTRL"); first = false; }
    if (mod & 0x44) { if (!first) printf("+"); printf("ALT"); first = false; }
    if (mod & 0x88) { if (!first) printf("+"); printf("WIN"); first = false; }
    if (mod & 0x22) { if (!first) printf("+"); printf("SHIFT"); first = false; }

    if (kc >= 0x04 && kc <= 0x1D) {
        if (!first) printf("+");
        printf("%c", 'A' + (kc - 0x04));
    } else if (kc >= 0x1E && kc <= 0x27) {
        if (!first) printf("+");
        printf("%c", (kc == 0x27) ? '0' : ('1' + (kc - 0x1E)));
    } else if (kc >= 0x3A && kc <= 0x45) {
        if (!first) printf("+");
        printf("F%d", (kc - 0x3A + 1));
    } else if (kc == 0x28) {
        if (!first) printf("+");
        printf("ENTER");
    } else if (kc == 0x29) {
        if (!first) printf("+");
        printf("ESC");
    } else if (kc == 0x2C) {
        if (!first) printf("+");
        printf("SPACE");
    }
    printf("]");
    fflush(stdout);
}

static inline bool key_found(const uint8_t *const src, uint8_t key, unsigned int length)
{
    for (unsigned int i = 0; i < length; i++) {
        if (src[i] == key) return true;
    }
    return false;
}

static void hid_host_keyboard_report_callback(const uint8_t *const data, const int length)
{
    hid_keyboard_input_report_boot_t *kb_report = (hid_keyboard_input_report_boot_t *)data;
    if (length < sizeof(hid_keyboard_input_report_boot_t)) return;

    static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = { 0 };
    static uint8_t prev_modifier = 0;
    key_event_t key_event;

    uint8_t curr_modifier = kb_report->modifier.val;
    uint8_t pressed_mods = curr_modifier & (~prev_modifier);
    bool any_key_pressed = false;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        if (kb_report->key[i] > HID_KEY_ERROR_UNDEFINED) {
            any_key_pressed = true;
            break;
        }
    }

    if (!any_key_pressed && pressed_mods != 0) {
        if (pressed_mods & 0x01) { printf("[LCTRL]"); fflush(stdout); }
        if (pressed_mods & 0x04) { printf("[LALT]"); fflush(stdout); }
        if (pressed_mods & 0x08) { printf("[LWIN]"); fflush(stdout); }
        if (pressed_mods & 0x10) { printf("[RCTRL]"); fflush(stdout); }
        if (pressed_mods & 0x40) { printf("[RALT]"); fflush(stdout); }
        if (pressed_mods & 0x80) { printf("[RWIN]"); fflush(stdout); }
    }
    prev_modifier = curr_modifier;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        if (prev_keys[i] > HID_KEY_ERROR_UNDEFINED &&
            !key_found(kb_report->key, prev_keys[i], HID_KEYBOARD_KEY_MAX)) {
            key_event.key_code = prev_keys[i];
            key_event.modifier = 0;
            key_event.state = KEY_STATE_RELEASED;
            key_event_callback(&key_event);
        }

        if (kb_report->key[i] > HID_KEY_ERROR_UNDEFINED &&
            !key_found(prev_keys, kb_report->key[i], HID_KEYBOARD_KEY_MAX)) {
            key_event.key_code = kb_report->key[i];
            key_event.modifier = kb_report->modifier.val;
            key_event.state = KEY_STATE_PRESSED;
            key_event_callback(&key_event);
        }
    }

    memcpy(prev_keys, &kb_report->key, HID_KEYBOARD_KEY_MAX);
}

static void hid_host_mouse_report_callback(const uint8_t *const data, const int length) {
    // Mouse support stub
}

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                        const hid_host_interface_event_t event,
                                        void *arg)
{
    uint8_t data[64] = { 0 };
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                                  data,
                                                                  64,
                                                                  &data_length));
        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                hid_host_keyboard_report_callback(data, data_length);
            } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                hid_host_mouse_report_callback(data, data_length);
            }
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID Device disconnected");
        if (s_kb_handle == hid_device_handle) {
            s_kb_handle = NULL;
        }
        ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID Device transfer error");
        break;
    default:
        break;
    }
}

static void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                                  const hid_host_driver_event_t event,
                                  void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "HID Device connected: Proto: %s", hid_proto_name_str[dev_params.proto]);

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL
        };

        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));

        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
            ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
                s_kb_handle = hid_device_handle;
                trigger_led_update(); // Sync NumLock LED on connect
            }
        }
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
        break;
    default:
        break;
    }
}

static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                                     const hid_host_driver_event_t event,
                                     void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        app_event_queue_t evt_queue = {
            .event_group = APP_EVENT_HID_HOST,
            .hid_host_device.handle = hid_device_handle,
            .hid_host_device.event = event,
            .hid_host_device.arg = arg
        };
        xQueueSend(app_event_queue, &evt_queue, 0);
    }
}

static void usb_lib_task(void *pvParameters)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(pvParameters);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

// Background task: Speed runner idle monitor & Cockpit police siren
static void toy_background_task(void *pvParameters) {
    bool police_toggle = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (s_current_mode == MODE_3_SPEED_RUNNER) {
            audio_speed_runner_idle_check();
        }

        if (s_current_mode == MODE_8_COCKPIT_ANIMALS && audio_cockpit_is_siren_on()) {
            police_toggle = !police_toggle;
            led_effects_police(police_toggle);
        }
    }
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "   ESP32-S3 N16R8: SMART SENSORY KEYBOARD TOY STARTING    ");
    ESP_LOGI(TAG, "==========================================================");

    // 1. Report PSRAM (8MB Octal) and Flash (16MB)
    size_t psram_size = esp_psram_get_size();
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_free = esp_get_free_heap_size();
    ESP_LOGI(TAG, ">>> [N16R8 HARDWARE READY]");
    ESP_LOGI(TAG, ">>> PSRAM Total: %u MB (8MB Octal OPI)", (unsigned int)(psram_size / (1024 * 1024)));
    ESP_LOGI(TAG, ">>> PSRAM Free : %u KB", (unsigned int)(psram_free / 1024));
    ESP_LOGI(TAG, ">>> Total Free Heap: %u KB", (unsigned int)(total_free / 1024));

    // 2. Initialize Audio Synthesizer Engine (Core 1)
    ESP_ERROR_CHECK(audio_synth_init());

    // 3. Initialize WS2812B RGB LED Strip (GPIO 4 via RMT)
    ESP_ERROR_CHECK(led_effects_init());

    // 4. Start toy background monitor task
    xTaskCreate(toy_background_task, "toy_bg", 3072, NULL, 4, NULL);

    // 5. Announce Mode 1
    switch_toy_mode(MODE_1_PIANO);

    // 6. Start USB Host Library & HID Driver (Core 0)
    app_event_queue = xQueueCreate(32, sizeof(app_event_queue_t));

    BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task,
                                                      "usb_events",
                                                      4096,
                                                      xTaskGetCurrentTaskHandle(),
                                                      10, NULL, 0);
    assert(task_created == pdTRUE);
    ulTaskNotifyTake(false, 1000);

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 10,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "USB HOST READY! Hay cam ban phim USB vao cong Type-C OTG");
    ESP_LOGI(TAG, "==========================================================");

    app_event_queue_t evt_queue;
    while (1) {
        if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
            if (APP_EVENT_HID_HOST == evt_queue.event_group) {
                hid_host_device_event(evt_queue.hid_host_device.handle,
                                      evt_queue.hid_host_device.event,
                                      evt_queue.hid_host_device.arg);
            } else if (APP_EVENT_UPDATE_LEDS == evt_queue.event_group) {
                update_keyboard_leds();
            }
        }
    }
}

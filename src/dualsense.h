#pragma once
#include <windows.h>

typedef struct {
    unsigned char lx, ly, rx, ry;
    unsigned char l2, r2;
    unsigned char b0, b1, b2;
    float gyro[3];
    short accel[3];
    int touch_on[2];
    int touch_id[2];
    int touch_x[2];
    int touch_y[2];
    int battery;
    int charging;
} PadInput;

typedef struct {
    unsigned char motor_left;
    unsigned char motor_right;
    unsigned char r, g, b;
    unsigned char player_leds;
    unsigned char mute_led;
    unsigned char trig_r[11];
    unsigned char trig_l[11];
} PadOutput;

typedef struct {
    int rumble_hd;
    int rumble_strength;
    int triggers;
    int trigger_strength;
    int haptics_volume;
    int lightbar_brightness;
    int player_leds;
    int gyro_mode;
    float gyro_sens_x;
    float gyro_sens_y;
    int gyro_invert_x;
    int gyro_invert_y;
    int gyro_anti_deadzone;
    float gyro_deadzone;
    int mute_toggles_gyro;
    int low_battery_warning;
    int audio_haptics;
    int audio_gain;
    int touch_swipes;
    int battery_on_connect;
    int verbose;
} Config;

extern Config cfg;
extern CRITICAL_SECTION lock;
extern PadInput pad_in;
extern PadOutput pad_out;
extern volatile int connected;
extern volatile int is_bt;
extern int connect_count;
extern volatile ULONGLONG connect_time;
extern volatile int gyro_enabled;
extern volatile int gyro_script;
extern volatile int light_override;
extern unsigned char light_override_rgb[3];

void log_msg(const char *fmt, ...);
void device_start(void);
void device_stop(void);
void device_reset_now(void);
void wake_writer(void);
void script_watchdog(void);

void haptics_fill(signed char *out, int frames);
int haptics_busy(void);
void haptics_play(int kind, float strength);
void haptics_tone(float freq, float amp, float ms, float pan);
void haptics_engine(float rpm, float load);
void haptics_rumble(int large, int little);
void haptics_audio_gate(float level);

void audio_start(void);
void audio_stop(void);
int audio_take(float *l, float *r);
float audio_loudness(void);

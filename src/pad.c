#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "dualsense.h"

typedef struct {
    unsigned short x, y;
    unsigned char id;
    unsigned char reserve[3];
} PadTouch;

typedef struct {
    unsigned int buttons;
    unsigned char lx, ly, rx, ry;
    unsigned char l2, r2;
    unsigned char pad1[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    unsigned char touch_num;
    unsigned char reserve1[3];
    unsigned int reserve2;
    PadTouch touch[2];
    unsigned char connected;
    unsigned char pad2[3];
    unsigned long long timestamp;
    unsigned int ext_id;
    unsigned char ext_reserve;
    unsigned char ext_len;
    unsigned char ext_data[10];
    unsigned char connected_count;
    unsigned char reserve3[2];
    unsigned char unique_len;
    unsigned char unique[12];
} ScePadData;

static_assert(sizeof(ScePadData) == 0x78, "pad data size");
static_assert(offsetof(ScePadData, touch_num) == 0x34, "touch offset");
static_assert(offsetof(ScePadData, touch) == 0x3c, "touch array offset");
static_assert(offsetof(ScePadData, connected) == 0x4c, "connected offset");
static_assert(offsetof(ScePadData, timestamp) == 0x50, "timestamp offset");
static_assert(offsetof(ScePadData, connected_count) == 0x68, "count offset");

Config cfg;
volatile int gyro_enabled = 1;
volatile int gyro_script = 0;
volatile int l2_brake = 0;
static int swipe_down = 0, swipe_done = 0, swipe_x = 0, swipe_y = 0, swipe_btn = 0;
static ULONGLONG swipe_start = 0, swipe_until = 0;
volatile int light_override = 0;
unsigned char light_override_rgb[3];

static int inited = 0;
static int opened[5];
static wchar_t base_dir[MAX_PATH];
static CRITICAL_SECTION log_lock;
static FILE *log_file = NULL;
static unsigned char default_rgb[3] = { 0, 30, 160 };
static int game_set_light = 0;
static int read_calls = 0, vib_calls = 0, light_calls = 0;
static int last_vib_big = -1, last_vib_small = -1;
static LARGE_INTEGER qpc_freq;
static volatile LONGLONG last_beat = 0;
static volatile int script_seen = 0;
static volatile int script_lost = 0;

void log_msg(const char *fmt, ...)
{
    if (!log_file)
        return;
    EnterCriticalSection(&log_lock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(log_file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fprintf(log_file, "\n");
    fflush(log_file);
    LeaveCriticalSection(&log_lock);
}

static const char *default_ini =
    "[dualsense]\r\n"
    "rumble=hd\r\n"
    "rumble_strength=100\r\n"
    "haptics_volume=100\r\n"
    "triggers=1\r\n"
    "trigger_strength=100\r\n"
    "lightbar_brightness=100\r\n"
    "lightbar_color=0,30,160\r\n"
    "player_leds=1\r\n"
    "gyro=l2\r\n"
    "gyro_sens_x=1.3\r\n"
    "gyro_sens_y=1.0\r\n"
    "gyro_invert_x=0\r\n"
    "gyro_invert_y=0\r\n"
    "gyro_anti_deadzone=22\r\n"
    "gyro_deadzone=1.0\r\n"
    "mute_toggles_gyro=1\r\n"
    "low_battery_warning=1\r\n"
    "touch_swipes=1\r\n"
    "battery_on_connect=1\r\n"
    "verbose_log=0\r\n";

static int ini_int(const wchar_t *path, const wchar_t *key, int def)
{
    return GetPrivateProfileIntW(L"dualsense", key, def, path);
}

static float ini_float(const wchar_t *path, const wchar_t *key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"dualsense", key, L"", buf, 64, path);
    if (buf[0] == 0)
        return def;
    return (float)_wtof(buf);
}

static void load_config(void)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\ak_dualsense.ini", base_dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = _wfopen(path, L"wb");
        if (f) {
            fwrite(default_ini, 1, strlen(default_ini), f);
            fclose(f);
        }
    }
    wchar_t buf[64];
    GetPrivateProfileStringW(L"dualsense", L"rumble", L"hd", buf, 64, path);
    cfg.rumble_hd = _wcsicmp(buf, L"classic") != 0;
    cfg.rumble_strength = ini_int(path, L"rumble_strength", 100);
    cfg.haptics_volume = ini_int(path, L"haptics_volume", 100);
    cfg.triggers = ini_int(path, L"triggers", 1);
    cfg.trigger_strength = ini_int(path, L"trigger_strength", 100);
    cfg.lightbar_brightness = ini_int(path, L"lightbar_brightness", 100);
    cfg.player_leds = ini_int(path, L"player_leds", 1);
    GetPrivateProfileStringW(L"dualsense", L"gyro", L"l2", buf, 64, path);
    if (_wcsicmp(buf, L"off") == 0)
        cfg.gyro_mode = 0;
    else if (_wcsicmp(buf, L"always") == 0)
        cfg.gyro_mode = 2;
    else
        cfg.gyro_mode = 1;
    cfg.gyro_sens_x = ini_float(path, L"gyro_sens_x", 1.3f);
    cfg.gyro_sens_y = ini_float(path, L"gyro_sens_y", 1.0f);
    cfg.gyro_invert_x = ini_int(path, L"gyro_invert_x", 0);
    cfg.gyro_invert_y = ini_int(path, L"gyro_invert_y", 0);
    cfg.gyro_anti_deadzone = ini_int(path, L"gyro_anti_deadzone", 22);
    cfg.gyro_deadzone = ini_float(path, L"gyro_deadzone", 1.0f);
    cfg.mute_toggles_gyro = ini_int(path, L"mute_toggles_gyro", 1);
    cfg.low_battery_warning = ini_int(path, L"low_battery_warning", 1);
    cfg.verbose = ini_int(path, L"verbose_log", 0);
    cfg.audio_haptics = ini_int(path, L"audio_haptics", 0);
    cfg.touch_swipes = ini_int(path, L"touch_swipes", 1);
    cfg.battery_on_connect = ini_int(path, L"battery_on_connect", 1);
    cfg.audio_gain = ini_int(path, L"audio_haptics_gain", 400);
    if (cfg.audio_gain < 0) cfg.audio_gain = 0;
    if (cfg.audio_gain > 2000) cfg.audio_gain = 2000;
    GetPrivateProfileStringW(L"dualsense", L"lightbar_color", L"0,30,160", buf, 64, path);
    int r, g, b;
    if (swscanf_s(buf, L"%d,%d,%d", &r, &g, &b) == 3) {
        default_rgb[0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
        default_rgb[1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
        default_rgb[2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
    }
    if (cfg.rumble_strength < 0) cfg.rumble_strength = 0;
    if (cfg.rumble_strength > 200) cfg.rumble_strength = 200;
    if (cfg.haptics_volume < 0) cfg.haptics_volume = 0;
    if (cfg.haptics_volume > 200) cfg.haptics_volume = 200;
    if (cfg.trigger_strength < 0) cfg.trigger_strength = 0;
    if (cfg.trigger_strength > 100) cfg.trigger_strength = 100;
    if (cfg.lightbar_brightness < 0) cfg.lightbar_brightness = 0;
    if (cfg.lightbar_brightness > 100) cfg.lightbar_brightness = 100;
    if (cfg.gyro_anti_deadzone < 0) cfg.gyro_anti_deadzone = 0;
    if (cfg.gyro_anti_deadzone > 90) cfg.gyro_anti_deadzone = 90;
}

static void setup(void)
{
    if (inited)
        return;
    inited = 1;
    QueryPerformanceFrequency(&qpc_freq);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\ak_dualsense.log", base_dir);
    log_file = _wfopen(path, L"w");
    load_config();
    log_msg("ak dualsense starting, rumble %s, gyro mode %d", cfg.rumble_hd ? "hd" : "classic", cfg.gyro_mode);
    EnterCriticalSection(&lock);
    memset(&pad_out, 0, sizeof(pad_out));
    pad_out.trig_l[0] = 0x05;
    pad_out.trig_r[0] = 0x05;
    pad_out.r = default_rgb[0];
    pad_out.g = default_rgb[1];
    pad_out.b = default_rgb[2];
    pad_out.player_leds = 0x04;
    LeaveCriticalSection(&lock);
    device_start();
    if (cfg.audio_haptics && cfg.rumble_hd)
        audio_start();
}

static int clamp_stick(float v)
{
    int i = (int)(v + 128.0f + 0.5f);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return i;
}

static void apply_gyro(ScePadData *d, const PadInput *in)
{
    if (!gyro_enabled || cfg.gyro_mode == 0)
        return;
    int active = cfg.gyro_mode == 2 || gyro_script || (cfg.gyro_mode == 1 && in->l2 > 40);
    if (!active)
        return;
    float yaw = in->gyro[1];
    float pitch = in->gyro[0];
    if (fabsf(yaw) < cfg.gyro_deadzone) yaw = 0;
    if (fabsf(pitch) < cfg.gyro_deadzone) pitch = 0;
    float dx = -yaw * cfg.gyro_sens_x;
    float dy = pitch * cfg.gyro_sens_y;
    if (cfg.gyro_invert_x) dx = -dx;
    if (cfg.gyro_invert_y) dy = -dy;
    float mag = sqrtf(dx * dx + dy * dy);
    if (mag < 0.5f)
        return;
    float adz = (float)cfg.gyro_anti_deadzone * 127.0f / 100.0f;
    float want = adz + mag * (127.0f - adz) / 127.0f;
    if (want > 127) want = 127;
    dx = dx / mag * want;
    dy = dy / mag * want;
    float sx = d->rx - 128.0f + dx;
    float sy = d->ry - 128.0f - dy;
    d->rx = (unsigned char)clamp_stick(sx);
    d->ry = (unsigned char)clamp_stick(sy);
}

static unsigned int map_buttons(const PadInput *in)
{
    unsigned int b = 0;
    switch (in->b0 & 0x0F) {
    case 0: b |= 0x10; break;
    case 1: b |= 0x10 | 0x20; break;
    case 2: b |= 0x20; break;
    case 3: b |= 0x20 | 0x40; break;
    case 4: b |= 0x40; break;
    case 5: b |= 0x40 | 0x80; break;
    case 6: b |= 0x80; break;
    case 7: b |= 0x80 | 0x10; break;
    }
    if (in->b0 & 0x10) b |= 0x8000;
    if (in->b0 & 0x20) b |= 0x4000;
    if (in->b0 & 0x40) b |= 0x2000;
    if (in->b0 & 0x80) b |= 0x1000;
    if (in->b1 & 0x01) b |= 0x0400;
    if (in->b1 & 0x02) b |= 0x0800;
    if (in->b1 & 0x04) b |= 0x0100;
    if (in->b1 & 0x08) b |= 0x0200;
    if (in->b1 & 0x20) b |= 0x0008;
    if (in->b1 & 0x40) b |= 0x0002;
    if (in->b1 & 0x80) b |= 0x0004;
    if (in->b2 & 0x02) b |= 0x100000;
    return b;
}

__declspec(dllexport) int scePadInit(void)
{
    setup();
    return 0;
}

__declspec(dllexport) int scePadOpen(int user, int type, int index, void *param)
{
    (void)param;
    setup();
    if (type != 0 || index != 0)
        return 0x80920001;
    if (user < 1 || user > 4)
        return 0x80920001;
    opened[user] = 1;
    log_msg("scePadOpen user %d", user);
    return user;
}

__declspec(dllexport) int scePadClose(int handle)
{
    if (handle < 1 || handle > 4 || !opened[handle])
        return 0x80920003;
    opened[handle] = 0;
    return 0;
}

__declspec(dllexport) int scePadReadState(int handle, ScePadData *data)
{
    if (!inited)
        return 0x80920005;
    if (!data)
        return 0x80920001;
    if (handle < 1 || handle > 4 || !opened[handle])
        return 0x80920003;
    memset(data, 0, sizeof(*data));
    data->lx = data->ly = data->rx = data->ry = 0x80;
    data->orientation[3] = 1.0f;
    data->acceleration[1] = 1.0f;
    read_calls++;
    if (handle != 1 || !connected)
        return 0;

    PadInput in;
    EnterCriticalSection(&lock);
    in = pad_in;
    int count = connect_count;
    LeaveCriticalSection(&lock);

    data->buttons = map_buttons(&in);
    data->lx = in.lx;
    data->ly = in.ly;
    data->rx = in.rx;
    data->ry = in.ry;
    data->l2 = in.l2;
    data->r2 = in.r2;
    apply_gyro(data, &in);
    if (l2_brake) {
        if (in.l2 > 50)
            data->buttons |= 0x8000;
        data->buttons &= ~0x100u;
        data->l2 = 0;
    }
    int n = 0;
    for (int i = 0; i < 2; i++) {
        if (!in.touch_on[i])
            continue;
        data->touch[n].x = (unsigned short)in.touch_x[i];
        data->touch[n].y = (unsigned short)(in.touch_y[i] * 943 / 1080);
        data->touch[n].id = (unsigned char)in.touch_id[i];
        n++;
    }
    data->touch_num = (unsigned char)n;
    if (cfg.touch_swipes) {
        ULONGLONG ms = GetTickCount64();
        if (in.touch_on[0] && !(in.b2 & 0x02)) {
            if (!swipe_down) {
                swipe_down = 1;
                swipe_done = 0;
                swipe_x = in.touch_x[0];
                swipe_y = in.touch_y[0];
                swipe_start = ms;
            } else if (!swipe_done && ms - swipe_start < 700) {
                int dx = in.touch_x[0] - swipe_x;
                int dy = in.touch_y[0] - swipe_y;
                int btn = 0;
                if (abs(dx) > 550 && abs(dx) > abs(dy) * 2)
                    btn = dx > 0 ? 0x20 : 0x80;
                else if (abs(dy) > 380 && abs(dy) > abs(dx) * 2)
                    btn = dy > 0 ? 0x40 : 0x10;
                if (btn) {
                    swipe_done = 1;
                    swipe_btn = btn;
                    swipe_until = ms + 110;
                    if (cfg.verbose)
                        log_msg("swipe %x", btn);
                }
            }
        } else {
            swipe_down = 0;
        }
        if (ms < swipe_until)
            data->buttons |= (unsigned int)swipe_btn;
    }
    data->connected = 1;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    data->timestamp = (unsigned long long)(now.QuadPart / (double)qpc_freq.QuadPart * 1000000.0);
    data->connected_count = (unsigned char)count;
    return 0;
}

__declspec(dllexport) int scePadRead(int handle, ScePadData *data, int num)
{
    if (num < 1)
        return 0x80920001;
    int r = scePadReadState(handle, data);
    return r < 0 ? r : 1;
}

__declspec(dllexport) int scePadSetVibration(int handle, const unsigned char *param)
{
    if (!inited)
        return 0x80920005;
    if (!param)
        return 0x80920001;
    if (handle < 1 || handle > 4 || !opened[handle])
        return 0x80920003;
    if (handle != 1)
        return 0;
    vib_calls++;
    int big = param[0], little = param[1];
    if (big != last_vib_big || little != last_vib_small) {
        if (cfg.verbose)
            log_msg("vibration %d %d", big, little);
        last_vib_big = big;
        last_vib_small = little;
        EnterCriticalSection(&lock);
        pad_out.motor_left = (unsigned char)big;
        pad_out.motor_right = (unsigned char)little;
        LeaveCriticalSection(&lock);
        haptics_rumble(big, little);
        wake_writer();
    }
    return 0;
}

__declspec(dllexport) int scePadSetLightBar(int handle, const unsigned char *param)
{
    if (!inited)
        return 0x80920005;
    if (!param)
        return 0x80920001;
    if (param[0] < 13 && param[1] < 13 && param[2] < 13)
        return 0x80920006;
    if (handle < 1 || handle > 4 || !opened[handle])
        return 0x80920003;
    if (handle != 1)
        return 0;
    light_calls++;
    EnterCriticalSection(&lock);
    int changed = pad_out.r != param[0] || pad_out.g != param[1] || pad_out.b != param[2];
    pad_out.r = param[0];
    pad_out.g = param[1];
    pad_out.b = param[2];
    LeaveCriticalSection(&lock);
    if (changed) {
        if (!game_set_light || cfg.verbose)
            log_msg("game light bar %d %d %d", param[0], param[1], param[2]);
        game_set_light = 1;
        wake_writer();
    }
    return 0;
}

__declspec(dllexport) int scePadResetLightBar(int handle)
{
    if (handle < 1 || handle > 4 || !opened[handle])
        return 0x80920003;
    EnterCriticalSection(&lock);
    pad_out.r = default_rgb[0];
    pad_out.g = default_rgb[1];
    pad_out.b = default_rgb[2];
    LeaveCriticalSection(&lock);
    wake_writer();
    return 0;
}

__declspec(dllexport) int scePadGetControllerInformation(int handle, void *info)
{
    if (!info)
        return 0x80920001;
    if (handle < 1 || handle > 4 || !opened[handle])
        return 0x80920003;
    memset(info, 0, 16);
    return 0;
}

__declspec(dllexport) int scePadResetOrientation(int handle) { (void)handle; return 0; }
__declspec(dllexport) int scePadSetAngularVelocityDeadbandState(int handle, int on) { (void)handle; (void)on; return 0; }
__declspec(dllexport) int scePadSetMotionSensorState(int handle, int on) { (void)handle; (void)on; return 0; }
__declspec(dllexport) int scePadSetTiltCorrectionState(int handle, int on) { (void)handle; (void)on; return 0; }

static void put_trigger(int side, const unsigned char *eff)
{
    EnterCriticalSection(&lock);
    if (side == 0 || side == 2)
        memcpy(pad_out.trig_l, eff, 11);
    if (side == 1 || side == 2)
        memcpy(pad_out.trig_r, eff, 11);
    LeaveCriticalSection(&lock);
    wake_writer();
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void zones_effect(int side, int mode, const int *strength, int freq)
{
    unsigned char e[11];
    unsigned int packed = 0;
    unsigned int active = 0;
    memset(e, 0, sizeof(e));
    for (int i = 0; i < 10; i++) {
        int s = clampi(strength[i], 0, 8);
        if (s > 0) {
            packed |= (unsigned int)((s - 1) & 7) << (3 * i);
            active |= 1u << i;
        }
    }
    if (!active) {
        e[0] = 0x05;
        put_trigger(side, e);
        return;
    }
    e[0] = (unsigned char)mode;
    e[1] = active & 0xFF;
    e[2] = (active >> 8) & 0xFF;
    e[3] = packed & 0xFF;
    e[4] = (packed >> 8) & 0xFF;
    e[5] = (packed >> 16) & 0xFF;
    e[6] = (packed >> 24) & 0xFF;
    e[9] = (unsigned char)clampi(freq, 0, 255);
    put_trigger(side, e);
}

__declspec(dllexport) void ds_TriggerOff(int side)
{
    unsigned char e[11] = { 0x05 };
    put_trigger(side, e);
}

__declspec(dllexport) void ds_TriggerFeedback(int side, int pos, int strength)
{
    int s[10] = { 0 };
    pos = clampi(pos, 0, 9);
    for (int i = pos; i < 10; i++)
        s[i] = strength;
    zones_effect(side, 0x21, s, 0);
}

__declspec(dllexport) void ds_TriggerSlope(int side, int start, int end, int from, int to)
{
    int s[10] = { 0 };
    start = clampi(start, 0, 8);
    end = clampi(end, start + 1, 9);
    for (int i = start; i < 10; i++) {
        if (i >= end)
            s[i] = to;
        else
            s[i] = from + (to - from) * (i - start) / (end - start);
    }
    zones_effect(side, 0x21, s, 0);
}

__declspec(dllexport) void ds_TriggerZones(int side, const int *strength)
{
    if (strength)
        zones_effect(side, 0x21, strength, 0);
}

__declspec(dllexport) void ds_TriggerVibration(int side, int pos, int amp, int freq)
{
    int s[10] = { 0 };
    pos = clampi(pos, 0, 9);
    for (int i = pos; i < 10; i++)
        s[i] = amp;
    if (freq <= 0) {
        ds_TriggerOff(side);
        return;
    }
    zones_effect(side, 0x26, s, freq);
}

__declspec(dllexport) void ds_TriggerWeapon(int side, int start, int end, int strength)
{
    unsigned char e[11];
    memset(e, 0, sizeof(e));
    start = clampi(start, 2, 7);
    end = clampi(end, start + 1, 8);
    strength = clampi(strength, 1, 8);
    unsigned int zones = (1u << start) | (1u << end);
    e[0] = 0x25;
    e[1] = zones & 0xFF;
    e[2] = (zones >> 8) & 0xFF;
    e[3] = (unsigned char)(strength - 1);
    put_trigger(side, e);
}

__declspec(dllexport) void ds_TriggerBow(int side, int start, int end, int strength, int snap)
{
    unsigned char e[11];
    memset(e, 0, sizeof(e));
    start = clampi(start, 1, 8);
    end = clampi(end, start + 1, 8);
    strength = clampi(strength, 1, 8);
    snap = clampi(snap, 1, 8);
    unsigned int zones = (1u << start) | (1u << end);
    e[0] = 0x22;
    e[1] = zones & 0xFF;
    e[2] = (zones >> 8) & 0xFF;
    e[3] = (unsigned char)(((strength - 1) & 7) | (((snap - 1) & 7) << 3));
    put_trigger(side, e);
}

__declspec(dllexport) void ds_TriggerGalloping(int side, int start, int end, int foot1, int foot2, int freq)
{
    unsigned char e[11];
    memset(e, 0, sizeof(e));
    start = clampi(start, 0, 8);
    end = clampi(end, start + 1, 9);
    foot1 = clampi(foot1, 0, 6);
    foot2 = clampi(foot2, foot1 + 1, 7);
    unsigned int zones = (1u << start) | (1u << end);
    e[0] = 0x23;
    e[1] = zones & 0xFF;
    e[2] = (zones >> 8) & 0xFF;
    e[3] = (unsigned char)((foot2 & 7) | ((foot1 & 7) << 3));
    e[4] = (unsigned char)clampi(freq, 1, 255);
    put_trigger(side, e);
}

__declspec(dllexport) void ds_TriggerMachine(int side, int start, int end, int a, int b, int freq, int period)
{
    unsigned char e[11];
    memset(e, 0, sizeof(e));
    start = clampi(start, 1, 8);
    end = clampi(end, start + 1, 9);
    unsigned int zones = (1u << start) | (1u << end);
    e[0] = 0x27;
    e[1] = zones & 0xFF;
    e[2] = (zones >> 8) & 0xFF;
    e[3] = (unsigned char)((clampi(a, 0, 7) & 7) | ((clampi(b, 0, 7) & 7) << 3));
    e[4] = (unsigned char)clampi(freq, 1, 255);
    e[5] = (unsigned char)clampi(period, 0, 255);
    put_trigger(side, e);
}

__declspec(dllexport) void ds_TriggerRaw(int side, const unsigned char *eff)
{
    if (eff)
        put_trigger(side, eff);
}

__declspec(dllexport) void ds_Haptic(int kind, float strength)
{
    if (cfg.rumble_hd && is_bt) {
        haptics_play(kind, strength);
        wake_writer();
    }
}

__declspec(dllexport) void ds_HapticTone(float freq, float amp, float ms, float pan)
{
    if (cfg.rumble_hd && is_bt) {
        haptics_tone(freq, amp, ms, pan);
        wake_writer();
    }
}

__declspec(dllexport) void ds_Engine(float rpm, float load)
{
    haptics_engine(rpm, load);
}

__declspec(dllexport) void ds_SetRumbleMode(int hd)
{
    cfg.rumble_hd = hd ? 1 : 0;
    wake_writer();
}

__declspec(dllexport) void ds_AudioHaptics(float level)
{
    haptics_audio_gate(level);
}

__declspec(dllexport) void ds_BrakeOnL2(int on)
{
    l2_brake = on ? 1 : 0;
}

__declspec(dllexport) void ds_GyroAim(int on)
{
    gyro_script = on ? 1 : 0;
}

__declspec(dllexport) void ds_Lightbar(int r, int g, int b)
{
    EnterCriticalSection(&lock);
    light_override_rgb[0] = (unsigned char)clampi(r, 0, 255);
    light_override_rgb[1] = (unsigned char)clampi(g, 0, 255);
    light_override_rgb[2] = (unsigned char)clampi(b, 0, 255);
    light_override = 1;
    LeaveCriticalSection(&lock);
    wake_writer();
}

__declspec(dllexport) void ds_LightbarRelease(void)
{
    light_override = 0;
    wake_writer();
}

__declspec(dllexport) void ds_PlayerLedBar(float fill)
{
    int n = (int)(fill * 5 + 0.5f);
    int mask = 0;
    if (fill > 0 && n == 0)
        n = 1;
    for (int i = 0; i < n && i < 5; i++)
        mask |= 1 << i;
    EnterCriticalSection(&lock);
    pad_out.player_leds = (unsigned char)mask;
    LeaveCriticalSection(&lock);
    wake_writer();
}

__declspec(dllexport) void ds_PlayerLeds(int mask)
{
    EnterCriticalSection(&lock);
    pad_out.player_leds = (unsigned char)(mask & 0x1F);
    LeaveCriticalSection(&lock);
    wake_writer();
}

__declspec(dllexport) int ds_Connected(void) { return connected; }
__declspec(dllexport) int ds_Bluetooth(void) { return is_bt; }
__declspec(dllexport) int ds_HdHaptics(void) { return cfg.rumble_hd && is_bt; }
__declspec(dllexport) int ds_GyroOn(void) { return gyro_enabled; }

__declspec(dllexport) int ds_Battery(void)
{
    int b = pad_in.battery * 10;
    return b > 100 ? 100 : b;
}

__declspec(dllexport) int ds_Charging(void) { return pad_in.charging; }

__declspec(dllexport) int ds_Input(int which)
{
    PadInput in;
    EnterCriticalSection(&lock);
    in = pad_in;
    LeaveCriticalSection(&lock);
    switch (which) {
    case 0: return in.l2;
    case 1: return in.r2;
    case 2: return in.lx;
    case 3: return in.ly;
    case 4: return in.rx;
    case 5: return in.ry;
    case 6: return (int)map_buttons(&in);
    }
    return 0;
}

__declspec(dllexport) float ds_Gyro(int axis)
{
    float v = 0;
    if (axis < 0 || axis > 2)
        return 0;
    EnterCriticalSection(&lock);
    v = pad_in.gyro[axis];
    LeaveCriticalSection(&lock);
    return v;
}

__declspec(dllexport) void ds_Heartbeat(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    last_beat = now.QuadPart;
    if (!script_seen)
        log_msg("script attached");
    if (script_lost)
        script_lost = 0;
    script_seen = 1;
}

void script_watchdog(void)
{
    if (!script_seen || script_lost)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if ((now.QuadPart - last_beat) / (double)qpc_freq.QuadPart < 0.3)
        return;
    script_lost = 1;
    EnterCriticalSection(&lock);
    memset(pad_out.trig_l, 0, 11);
    memset(pad_out.trig_r, 0, 11);
    pad_out.trig_l[0] = 0x05;
    pad_out.trig_r[0] = 0x05;
    LeaveCriticalSection(&lock);
    haptics_engine(0, 0);
    gyro_script = 0;
    l2_brake = 0;
    if (cfg.verbose)
        log_msg("script went quiet, triggers released");
}

__declspec(dllexport) void ds_Log(const char *text)
{
    if (text)
        log_msg("script: %s", text);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&lock);
        InitializeCriticalSection(&log_lock);
        GetModuleFileNameW(inst, base_dir, MAX_PATH);
        wchar_t *slash = wcsrchr(base_dir, L'\\');
        if (slash)
            *slash = 0;
    } else if (reason == DLL_PROCESS_DETACH) {
        if (inited) {
            log_msg("shutting down (reads %d, vibration %d, light bar %d)", read_calls, vib_calls, light_calls);
            device_reset_now();
        }
        (void)reserved;
    }
    return TRUE;
}

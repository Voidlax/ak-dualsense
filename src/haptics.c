#include <windows.h>
#include <math.h>
#include <string.h>
#include "dualsense.h"

#define RATE 3000.0f
#define MAX_VOICES 32
#define PI2 6.28318530718f

typedef struct {
    int on;
    float freq;
    float amp;
    float left;
    float right;
    float delay;
    float t;
    float len;
    float decay;
    float noise;
    float trem;
    float phase;
} Voice;

static Voice voices[MAX_VOICES];
static float rumble_big = 0, rumble_small = 0;
static float target_big = 0, target_small = 0;
static float engine_rpm = 0, engine_load = 0;
static float engine_level = 0;
static float ph_big = 0, ph_small = 0, ph_engine = 0, ph_engine2 = 0;
static unsigned int seed = 12345;
static float noise_lp = 0;
static float audio_gate = 1.0f;
static float audio_gate_now = 0;

static float rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return ((seed >> 9) & 0xFFFF) / 32768.0f - 1.0f;
}

static void add_voice(float freq, float amp, float ms, float pan, float delay_ms, float decay, float noise, float trem)
{
    if (amp <= 0)
        return;
    EnterCriticalSection(&lock);
    int pick = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].on) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        float oldest = -1;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].t > oldest) {
                oldest = voices[i].t;
                pick = i;
            }
        }
    }
    Voice *v = &voices[pick];
    v->on = 1;
    v->freq = freq;
    v->amp = amp > 1.5f ? 1.5f : amp;
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    v->left = pan <= 0 ? 1.0f : 1.0f - pan * 0.8f;
    v->right = pan >= 0 ? 1.0f : 1.0f + pan * 0.8f;
    v->delay = delay_ms / 1000.0f;
    v->t = 0;
    v->len = ms / 1000.0f;
    v->decay = decay;
    v->noise = noise;
    v->trem = trem;
    v->phase = 0;
    LeaveCriticalSection(&lock);
}

void haptics_tone(float freq, float amp, float ms, float pan)
{
    add_voice(freq, amp, ms, pan, 0, 3.0f, 0, 0);
}

void haptics_play(int kind, float s)
{
    switch (kind) {
    case 0:
        add_voice(230, 0.55f * s, 22, 0, 0, 2.0f, 0, 0);
        break;
    case 1:
        add_voice(65, 0.9f * s, 140, 0, 0, 4.0f, 0.25f, 0);
        add_voice(140, 0.4f * s, 40, 0, 0, 3.0f, 0.5f, 0);
        break;
    case 2:
        add_voice(42, 1.1f * s, 380, 0, 0, 5.0f, 0.35f, 0);
        add_voice(95, 0.7f * s, 90, 0, 0, 4.0f, 0.6f, 0);
        break;
    case 3:
        add_voice(55, 0.8f * s, 90, -0.3f, 0, 3.0f, 0, 0);
        add_voice(55, 0.6f * s, 90, -0.3f, 190, 3.0f, 0, 0);
        break;
    case 4:
        add_voice(125, 0.6f * s, 260, 0, 0, 1.0f, 0.15f, 18);
        break;
    case 5:
        add_voice(95, 0.7f * s, 40, 0.2f, 0, 3.0f, 0.4f, 0);
        break;
    case 6:
        add_voice(75, 0.9f * s, 110, -1, 0, 4.0f, 0.3f, 0);
        break;
    case 7:
        add_voice(75, 0.9f * s, 110, 1, 0, 4.0f, 0.3f, 0);
        break;
    case 8:
        add_voice(180, 0.5f * s, 60, 1, 0, 2.5f, 0, 0);
        add_voice(110, 0.6f * s, 120, 1, 50, 3.0f, 0.2f, 0);
        break;
    case 9:
        add_voice(160, 0.45f * s, 30, -1, 0, 2.0f, 0, 0);
        break;
    case 10:
        add_voice(160, 0.45f * s, 30, 1, 0, 2.0f, 0, 0);
        break;
    case 11:
        add_voice(30, 1.2f * s, 700, 0, 0, 3.5f, 0.45f, 0);
        add_voice(70, 0.8f * s, 250, 0, 0, 4.0f, 0.7f, 0);
        break;
    case 12:
        add_voice(250, 0.35f * s, 1200, 0, 0, 0.3f, 0.05f, 6);
        break;
    default:
        add_voice(100, 0.6f * s, 60, 0, 0, 3.0f, 0, 0);
        break;
    }
}

void haptics_engine(float rpm, float load)
{
    if (rpm < 0) rpm = 0;
    if (rpm > 1) rpm = 1;
    if (load < 0) load = 0;
    if (load > 1) load = 1;
    EnterCriticalSection(&lock);
    engine_rpm = rpm;
    engine_load = load;
    LeaveCriticalSection(&lock);
}

void haptics_rumble(int large, int little)
{
    EnterCriticalSection(&lock);
    target_big = large / 255.0f;
    target_small = little / 255.0f;
    LeaveCriticalSection(&lock);
}

void haptics_audio_gate(float level)
{
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    audio_gate = level;
}

int haptics_busy(void)
{
    int busy = 0;
    if (cfg.audio_haptics && audio_gate > 0 && audio_loudness() * cfg.audio_gain / 100.0f > 0.01f)
        busy = 1;
    EnterCriticalSection(&lock);
    if (target_big > 0 || target_small > 0 || rumble_big > 0.01f || rumble_small > 0.01f)
        busy = 1;
    if (engine_load > 0 || engine_rpm > 0 || engine_level > 0.01f)
        busy = 1;
    for (int i = 0; i < MAX_VOICES && !busy; i++)
        if (voices[i].on)
            busy = 1;
    LeaveCriticalSection(&lock);
    return busy;
}

void haptics_fill(signed char *out, int frames)
{
    float vol = cfg.haptics_volume / 100.0f;
    float rs = cfg.rumble_strength / 100.0f;
    float dt = 1.0f / RATE;
    EnterCriticalSection(&lock);
    float eng_target = (engine_rpm > 0 || engine_load > 0) ? 1.0f : 0.0f;
    for (int n = 0; n < frames; n++) {
        rumble_big += (target_big - rumble_big) * 0.02f;
        rumble_small += (target_small - rumble_small) * 0.03f;
        engine_level += (eng_target - engine_level) * 0.004f;

        float l = 0, r = 0;

        // float fb = 45;
        float fb = 38 + rumble_big * 22;
        ph_big += PI2 * fb * dt;
        if (ph_big > PI2) ph_big -= PI2;
        float big = (sinf(ph_big) * 0.75f + sinf(ph_big * 2) * 0.25f) * rumble_big * rs;

        ph_small += PI2 * 165 * dt;
        if (ph_small > PI2) ph_small -= PI2;
        float little = sinf(ph_small) * rumble_small * 0.8f * rs;

        l += big + little * 0.35f;
        r += little + big * 0.35f;

        if (engine_level > 0.001f) {
            float f = 26 + engine_rpm * 75;
            ph_engine += PI2 * f * dt;
            if (ph_engine > PI2) ph_engine -= PI2;
            ph_engine2 += PI2 * f * 0.5f * dt;
            if (ph_engine2 > PI2) ph_engine2 -= PI2;
            float a = (0.10f + engine_load * 0.45f + engine_rpm * 0.12f) * engine_level;
            float e = (sinf(ph_engine) * 0.7f + sinf(ph_engine2) * 0.3f) * a;
            l += e;
            r += e * 0.9f;
        }

        noise_lp += (rnd() - noise_lp) * 0.25f;

        if (cfg.audio_haptics) {
            float al = 0, ar = 0;
            audio_gate_now += (audio_gate - audio_gate_now) * 0.002f;
            if (audio_take(&al, &ar) && audio_gate_now > 0.001f) {
                float g = cfg.audio_gain / 100.0f * audio_gate_now;
                al *= g;
                ar *= g;
                if (fabsf(al) < 0.015f) al = 0;
                if (fabsf(ar) < 0.015f) ar = 0;
                l += al;
                r += ar;
            }
        }

        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = &voices[i];
            if (!v->on)
                continue;
            if (v->delay > 0) {
                v->delay -= dt;
                continue;
            }
            float env;
            float att = 0.003f;
            if (v->t < att)
                env = v->t / att;
            else
                env = expf(-v->decay * (v->t - att) / v->len);
            if (v->t > v->len * 0.8f)
                env *= (v->len - v->t) / (v->len * 0.2f);
            if (env < 0) env = 0;
            if (v->trem > 0)
                env *= 0.6f + 0.4f * sinf(PI2 * v->trem * v->t);
            v->phase += PI2 * v->freq * dt;
            if (v->phase > PI2) v->phase -= PI2;
            float x = sinf(v->phase) * (1.0f - v->noise) + noise_lp * 2.0f * v->noise;
            x *= v->amp * env;
            l += x * v->left;
            r += x * v->right;
            v->t += dt;
            if (v->t >= v->len)
                v->on = 0;
        }

        l = tanhf(l * vol * 1.6f);
        r = tanhf(r * vol * 1.6f);
        int a = (int)(l * 127.0f);
        int b = (int)(r * 127.0f);
        if (a > 127) a = 127;
        if (a < -127) a = -127;
        if (b > 127) b = 127;
        if (b < -127) b = -127;
        out[n * 2] = (signed char)a;
        out[n * 2 + 1] = (signed char)b;
    }
    LeaveCriticalSection(&lock);
}

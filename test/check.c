#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <conio.h>

typedef int (*init_fn)(void);
typedef int (*open_fn)(int, int, int, void *);
typedef int (*read_fn)(int, unsigned char *);
typedef int (*set_fn)(int, const unsigned char *);
typedef void (*trig3_fn)(int, int, int);
typedef void (*trig4_fn)(int, int, int, int);
typedef void (*trig5_fn)(int, int, int, int, int);
typedef void (*trig1_fn)(int);
typedef void (*hap_fn)(int, float);
typedef void (*eng_fn)(float, float);
typedef int (*get_fn)(void);
typedef int (*geti_fn)(int);
typedef float (*gyro_fn)(int);
typedef void (*mode_fn)(int);

FILE *out;
int handle;
set_fn vib, light;
trig1_fn off;
trig3_fn feedback;
trig4_fn weapon, vibr;
trig5_fn slope;
hap_fn hap;
eng_fn eng;
geti_fn input;
gyro_fn gyro;
mode_fn rumble_mode;
get_fn conn, batt, gyro_on;

char ask(const char *q, const char *keys)
{
    printf("%s ", q);
    fflush(stdout);
    while (_kbhit())
        _getch();
    for (;;) {
        int c = _getch();
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (strchr(keys, c)) {
            printf("%c\n", c);
            fprintf(out, "%s -> %c\n", q, c);
            fflush(out);
            return (char)c;
        }
    }
}

void wait_key(const char *msg)
{
    printf("%s (press any key on the keyboard)\n", msg);
    while (_kbhit())
        _getch();
    _getch();
}

float measure(int axis, const char *what)
{
    printf("\nget ready to %s. starting in 2 seconds...\n", what);
    Sleep(2000);
    printf("GO, %s now!\n", what);
    float sum = 0;
    for (int i = 0; i < 150; i++) {
        sum += gyro(axis) * 0.01f;
        Sleep(10);
    }
    printf("ok, measured %.0f degrees\n", sum);
    fprintf(out, "%s: axis %d total %.1f deg\n", what, axis, sum);
    fflush(out);
    return sum;
}

int main(void)
{
    out = fopen("check_results.txt", "w");
    HMODULE lib = LoadLibraryA("libScePad_x64.dll");
    if (!lib || !out) {
        printf("setup failed\n");
        Sleep(5000);
        return 1;
    }
    init_fn init = (init_fn)GetProcAddress(lib, "scePadInit");
    open_fn opn = (open_fn)GetProcAddress(lib, "scePadOpen");
    vib = (set_fn)GetProcAddress(lib, "scePadSetVibration");
    light = (set_fn)GetProcAddress(lib, "scePadSetLightBar");
    off = (trig1_fn)GetProcAddress(lib, "ds_TriggerOff");
    feedback = (trig3_fn)GetProcAddress(lib, "ds_TriggerFeedback");
    weapon = (trig4_fn)GetProcAddress(lib, "ds_TriggerWeapon");
    vibr = (trig4_fn)GetProcAddress(lib, "ds_TriggerVibration");
    slope = (trig5_fn)GetProcAddress(lib, "ds_TriggerSlope");
    hap = (hap_fn)GetProcAddress(lib, "ds_Haptic");
    eng = (eng_fn)GetProcAddress(lib, "ds_Engine");
    input = (geti_fn)GetProcAddress(lib, "ds_Input");
    gyro = (gyro_fn)GetProcAddress(lib, "ds_Gyro");
    rumble_mode = (mode_fn)GetProcAddress(lib, "ds_SetRumbleMode");
    conn = (get_fn)GetProcAddress(lib, "ds_Connected");
    batt = (get_fn)GetProcAddress(lib, "ds_Battery");
    gyro_on = (get_fn)GetProcAddress(lib, "ds_GyroOn");

    init();
    handle = opn(1, 0, 0, NULL);
    printf("waiting for the controller...\n");
    while (!conn())
        Sleep(200);
    printf("dualsense connected, battery %d%%\n\n", batt());
    printf("answer with y or n on the KEYBOARD. hold the controller normally.\n");
    printf("nothing happens until you press a key, take your time.\n\n");
    wait_key("look at the light bar, then start");

    unsigned char red[3] = { 255, 0, 0 }, green[3] = { 0, 255, 0 }, blue[3] = { 0, 0, 255 }, base[3] = { 0, 30, 160 };
    printf("watching the light bar: red, green, blue...\n");
    light(handle, red);
    Sleep(1000);
    light(handle, green);
    Sleep(1000);
    light(handle, blue);
    Sleep(1000);
    light(handle, base);
    ask("1. did the light bar go red, then green, then blue? (y/n)", "yn");
    ask("2. is just the middle player light on under the touchpad? (y/n)", "yn");

    wait_key("\nnext is classic rumble. hold the controller, then");
    rumble_mode(0);
    unsigned char v[2] = { 220, 0 };
    printf("\nclassic rumble, left side then right side...\n");
    vib(handle, v);
    Sleep(1000);
    v[0] = 0;
    v[1] = 220;
    vib(handle, v);
    Sleep(1000);
    v[1] = 0;
    vib(handle, v);
    ask("3. felt a strong rumble then a lighter buzzy one? (y/n)", "yn");

    wait_key("\nnext is the same rumble through the new bluetooth haptics, then");
    rumble_mode(1);
    printf("\nnow the new bluetooth haptics. a game rumble first, same as before...\n");
    Sleep(300);
    v[0] = 220;
    vib(handle, v);
    Sleep(1000);
    v[0] = 0;
    v[1] = 220;
    vib(handle, v);
    Sleep(1000);
    v[1] = 0;
    vib(handle, v);
    Sleep(500);
    char hd = ask("4. felt that one too? (y/n)", "yn");
    if (hd == 'y')
        ask("5. compared to the classic one it felt: b = better, s = same, w = worse", "bsw");

    const char *names[] = { "tick", "thud", "heavy hit", "heartbeat", "electric buzz", "gun shot",
                            "hit on the left", "hit on the right", "grapple", "tap left", "tap right",
                            "explosion", "long hum" };
    wait_key("\nnext is a list of 13 effects plus an engine, about 17 seconds, then");
    printf("\na list of effects, one by one:\n");
    for (int k = 0; k <= 12; k++) {
        printf("  %s\n", names[k]);
        hap(k, 1.0f);
        Sleep(k == 12 ? 1600 : 1100);
    }
    printf("  engine revving up\n");
    for (int i = 0; i <= 40; i++) {
        eng(i / 40.0f, i / 40.0f);
        Sleep(70);
    }
    eng(0, 0);
    Sleep(800);
    ask("6. could you tell the effects apart, and did left/right hits come from the right side? (y/n)", "yn");
    ask("7. how strong overall: w = too weak, g = good, s = too strong", "wgs");

    wait_key("\nnext are the triggers, then");
    printf("\nthe right trigger now has a gun click halfway.\n");
    weapon(1, 3, 6, 8);
    wait_key("pull R2 all the way a few times, then");
    ask("8. was there resistance that snapped/clicked through? (y/n)", "yn");
    off(1);
    feedback(0, 2, 7);
    printf("\nthe left trigger is now stiff from about 20%%.\n");
    wait_key("press L2 a few times, then");
    ask("9. was L2 stiff? (y/n)", "yn");
    vibr(2, 1, 7, 35);
    printf("\nboth triggers now vibrate while pressed.\n");
    wait_key("press both triggers, then");
    ask("10. did they vibrate? (y/n)", "yn");
    slope(1, 1, 8, 1, 8);
    off(0);
    printf("\nthe right trigger now gets harder the further you pull.\n");
    wait_key("pull R2 slowly, then");
    ask("11. did it ramp up? (y/n)", "yn");
    off(2);

    wait_key("\nnext is gyro, two quick moves. hold the controller like playing, then");
    measure(1, "rotate the controller flat to point it to your LEFT");
    wait_key("\nnext move, then");
    measure(0, "tilt the FRONT of the controller UP (point it at the ceiling)");

    int before = gyro_on();
    wait_key("\nlast one: press the MUTE button (under the PS button) once, then");
    Sleep(300);
    fprintf(out, "mute toggled gyro: %d -> %d\n", before, gyro_on());
    ask("12. did the orange mute light come on? (y/n)", "yn");
    wait_key("press MUTE again to turn it back off, then");

    fclose(out);
    printf("\nall done, thanks. you can close this window.\n");
    Sleep(4000);
    return 0;
}

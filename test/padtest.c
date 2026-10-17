#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int (*init_fn)(void);
typedef int (*open_fn)(int, int, int, void *);
typedef int (*read_fn)(int, unsigned char *);
typedef int (*set_fn)(int, const unsigned char *);
typedef void (*trig3_fn)(int, int, int);
typedef void (*trig4_fn)(int, int, int, int);
typedef void (*trig1_fn)(int);
typedef void (*hap_fn)(int, float);
typedef void (*eng_fn)(float, float);
typedef int (*get_fn)(void);

HMODULE lib;
int handle;
read_fn rd;

void show_state(int seconds)
{
    unsigned char d[0x78];
    for (int i = 0; i < seconds * 10; i++) {
        memset(d, 0, sizeof(d));
        int r = rd(handle, d);
        unsigned int b = *(unsigned int *)d;
        float *f = (float *)(d + 0x0c);
        printf("\rret %d conn %d btn %06x L(%3d,%3d) R(%3d,%3d) L2 %3d R2 %3d touch %d x %4d y %4d   ",
               r, d[0x4c], b, d[4], d[5], d[6], d[7], d[8], d[9], d[0x34], *(unsigned short *)(d + 0x3c),
               *(unsigned short *)(d + 0x3e));
        (void)f;
        fflush(stdout);
        Sleep(100);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "state";
    lib = LoadLibraryA("libScePad_x64.dll");
    if (!lib) {
        printf("cant load dll (%lu)\n", GetLastError());
        return 1;
    }
    init_fn init = (init_fn)GetProcAddress(lib, "scePadInit");
    open_fn opn = (open_fn)GetProcAddress(lib, "scePadOpen");
    rd = (read_fn)GetProcAddress(lib, "scePadReadState");
    set_fn vib = (set_fn)GetProcAddress(lib, "scePadSetVibration");
    set_fn light = (set_fn)GetProcAddress(lib, "scePadSetLightBar");
    trig1_fn off = (trig1_fn)GetProcAddress(lib, "ds_TriggerOff");
    trig3_fn feedback = (trig3_fn)GetProcAddress(lib, "ds_TriggerFeedback");
    trig4_fn weapon_raw = (trig4_fn)GetProcAddress(lib, "ds_TriggerWeapon");
    trig4_fn vibr = (trig4_fn)GetProcAddress(lib, "ds_TriggerVibration");
    hap_fn hap = (hap_fn)GetProcAddress(lib, "ds_Haptic");
    eng_fn eng = (eng_fn)GetProcAddress(lib, "ds_Engine");
    get_fn conn = (get_fn)GetProcAddress(lib, "ds_Connected");
    get_fn batt = (get_fn)GetProcAddress(lib, "ds_Battery");
    get_fn bt = (get_fn)GetProcAddress(lib, "ds_Bluetooth");

    init();
    handle = opn(1, 0, 0, NULL);
    printf("handle %d\n", handle);
    for (int i = 0; i < 50 && !conn(); i++)
        Sleep(100);
    printf("connected %d bluetooth %d battery %d\n", conn(), bt(), batt());

    if (!strcmp(mode, "state")) {
        show_state(argc > 2 ? atoi(argv[2]) : 10);
    } else if (!strcmp(mode, "light")) {
        unsigned char cols[][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 140, 0 }, { 0, 30, 160 } };
        for (int i = 0; i < 5; i++) {
            printf("light %d %d %d\n", cols[i][0], cols[i][1], cols[i][2]);
            light(handle, cols[i]);
            Sleep(1200);
        }
    } else if (!strcmp(mode, "rumble")) {
        unsigned char v[2];
        v[0] = 200; v[1] = 0;
        printf("large motor\n");
        vib(handle, v);
        Sleep(1200);
        v[0] = 0; v[1] = 200;
        printf("small motor\n");
        vib(handle, v);
        Sleep(1200);
        v[0] = 0; v[1] = 0;
        vib(handle, v);
        Sleep(300);
    } else if (!strcmp(mode, "haptics")) {
        for (int k = 0; k <= 12; k++) {
            printf("haptic %d\n", k);
            hap(k, 1.0f);
            Sleep(1300);
        }
        printf("engine\n");
        for (int i = 0; i <= 40; i++) {
            eng(i / 40.0f, i / 40.0f);
            Sleep(75);
        }
        eng(0, 0);
        Sleep(1500);
    } else if (!strcmp(mode, "triggers")) {
        printf("feedback, both triggers stiff from 30%%\n");
        feedback(2, 3, 6);
        show_state(4);
        printf("weapon on R2, click between 30 and 60%%\n");
        off(0);
        weapon_raw(1, 3, 6, 8);
        show_state(4);
        printf("vibration on both, 30hz\n");
        vibr(2, 1, 6, 30);
        show_state(4);
        off(2);
        Sleep(300);
    }
    return 0;
}

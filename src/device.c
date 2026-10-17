#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "dualsense.h"

CRITICAL_SECTION lock;
PadInput pad_in;
PadOutput pad_out;
volatile int connected = 0;
volatile int is_bt = 0;
int connect_count = 0;
volatile ULONGLONG connect_time = 0;

static CRITICAL_SECTION dev_lock;
static HANDLE dev = INVALID_HANDLE_VALUE;
static int in_len, out_len, feat_len;
static int vibration_v2 = 1;
static float gyro_scale[3] = { 1.0f / 1024, 1.0f / 1024, 1.0f / 1024 };
static volatile int running = 0;
static HANDLE reader_thread = NULL, writer_thread = NULL;
static HANDLE wake_event = NULL;
static HANDLE write_event = NULL;
static unsigned char out_seq = 0;
static int need_setup = 0;
static int need_full_mode = 0;
static int last_mute = 0;
static unsigned int crc_table[256];
static int write_fails = 0;
int hap_writes = 0;

static void make_crc_table(void)
{
    for (unsigned int i = 0; i < 256; i++) {
        unsigned int c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
}

static unsigned int bt_crc(unsigned char seed, const unsigned char *data, int len)
{
    unsigned int c = 0xFFFFFFFF;
    c = crc_table[(c ^ seed) & 0xFF] ^ (c >> 8);
    for (int i = 0; i < len; i++)
        c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static int get_feature(int id, unsigned char *buf)
{
    memset(buf, 0, feat_len);
    buf[0] = (unsigned char)id;
    return HidD_GetFeature(dev, buf, feat_len);
}

static void read_calibration(void)
{
    unsigned char buf[1024];
    if (feat_len <= 0 || feat_len > (int)sizeof(buf))
        return;
    if (!get_feature(0x05, buf)) {
        log_msg("calibration read failed (%lu)", GetLastError());
        return;
    }
    short pitch_bias = *(short *)&buf[1];
    short yaw_bias = *(short *)&buf[3];
    short roll_bias = *(short *)&buf[5];
    short pitch_plus = *(short *)&buf[7];
    short pitch_minus = *(short *)&buf[9];
    short yaw_plus = *(short *)&buf[11];
    short yaw_minus = *(short *)&buf[13];
    short roll_plus = *(short *)&buf[15];
    short roll_minus = *(short *)&buf[17];
    short speed_plus = *(short *)&buf[19];
    short speed_minus = *(short *)&buf[21];
    int speed2 = speed_plus + speed_minus;
    int d0 = abs(pitch_plus - pitch_bias) + abs(pitch_minus - pitch_bias);
    int d1 = abs(yaw_plus - yaw_bias) + abs(yaw_minus - yaw_bias);
    int d2 = abs(roll_plus - roll_bias) + abs(roll_minus - roll_bias);
    gyro_scale[0] = d0 ? (float)speed2 / d0 : 1.0f / 1024;
    gyro_scale[1] = d1 ? (float)speed2 / d1 : 1.0f / 1024;
    gyro_scale[2] = d2 ? (float)speed2 / d2 : 1.0f / 1024;
    if (gyro_scale[0] <= 0 || gyro_scale[0] > 1) gyro_scale[0] = 1.0f / 1024;
    if (gyro_scale[1] <= 0 || gyro_scale[1] > 1) gyro_scale[1] = 1.0f / 1024;
    if (gyro_scale[2] <= 0 || gyro_scale[2] > 1) gyro_scale[2] = 1.0f / 1024;
}

static void read_firmware(int pid)
{
    unsigned char buf[1024];
    vibration_v2 = 1;
    if (pid == 0x0DF2)
        return;
    if (feat_len < 64 || feat_len > (int)sizeof(buf))
        return;
    if (get_feature(0x20, buf)) {
        int ver = buf[44] | (buf[45] << 8);
        vibration_v2 = ver >= 0x0215;
        log_msg("firmware update version %04x, vibration v2 %d", ver, vibration_v2);
    }
}

static int try_open(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_msg("open failed %lu: %ls", GetLastError(), path);
        return 0;
    }
    HIDD_ATTRIBUTES attr;
    attr.Size = sizeof(attr);
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != 0x054C ||
        (attr.ProductID != 0x0CE6 && attr.ProductID != 0x0DF2)) {
        CloseHandle(h);
        return 0;
    }
    PHIDP_PREPARSED_DATA pp;
    HIDP_CAPS caps;
    if (!HidD_GetPreparsedData(h, &pp)) {
        CloseHandle(h);
        return 0;
    }
    HidP_GetCaps(pp, &caps);
    HidD_FreePreparsedData(pp);
    log_msg("candidate usage %x/%x in %d: %ls", caps.UsagePage, caps.Usage, caps.InputReportByteLength, path);
    if (caps.UsagePage != 1 || caps.Usage != 5 || caps.InputReportByteLength < 64) {
        CloseHandle(h);
        return 0;
    }

    EnterCriticalSection(&dev_lock);
    dev = h;
    in_len = caps.InputReportByteLength;
    out_len = caps.OutputReportByteLength;
    feat_len = caps.FeatureReportByteLength;
    is_bt = in_len != 64;
    read_calibration();
    read_firmware(attr.ProductID);
    LeaveCriticalSection(&dev_lock);

    log_msg("opened dualsense %04x over %s (in %d out %d feat %d)", attr.ProductID,
            is_bt ? "bluetooth" : "usb", in_len, out_len, feat_len);
    return 1;
}

static int open_device(void)
{
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO info = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(info, NULL, &guid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, NULL, 0, &need, NULL);
        if (need == 0)
            continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = malloc(need);
        if (!detail)
            continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, need, NULL, NULL)) {
            wchar_t lower[512];
            wcsncpy_s(lower, 512, detail->DevicePath, _TRUNCATE);
            _wcslwr_s(lower, 512);
            if (wcsstr(lower, L"054c"))
                found = try_open(detail->DevicePath);
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

static void close_device(void)
{
    EnterCriticalSection(&dev_lock);
    if (dev != INVALID_HANDLE_VALUE) {
        CancelIoEx(dev, NULL);
        CloseHandle(dev);
        dev = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&dev_lock);
    EnterCriticalSection(&lock);
    if (connected)
        log_msg("dualsense disconnected");
    connected = 0;
    memset(&pad_in, 0, sizeof(pad_in));
    pad_in.lx = pad_in.ly = pad_in.rx = pad_in.ry = 0x80;
    LeaveCriticalSection(&lock);
}

static void parse_input(const unsigned char *buf, int len)
{
    const unsigned char *d;
    if (buf[0] == 0x31 && len >= 78)
        d = buf + 2;
    else if (buf[0] == 0x01 && len >= 64 && !is_bt)
        d = buf + 1;
    else {
        if (buf[0] == 0x01 && is_bt)
            need_full_mode = 1;
        return;
    }

    EnterCriticalSection(&lock);
    pad_in.lx = d[0];
    pad_in.ly = d[1];
    pad_in.rx = d[2];
    pad_in.ry = d[3];
    pad_in.l2 = d[4];
    pad_in.r2 = d[5];
    pad_in.b0 = d[7];
    pad_in.b1 = d[8];
    pad_in.b2 = d[9];
    for (int i = 0; i < 3; i++) {
        short g = (short)(d[15 + i * 2] | (d[16 + i * 2] << 8));
        pad_in.gyro[i] = g * gyro_scale[i];
        pad_in.accel[i] = (short)(d[21 + i * 2] | (d[22 + i * 2] << 8));
    }
    for (int i = 0; i < 2; i++) {
        const unsigned char *t = d + 32 + i * 4;
        pad_in.touch_on[i] = !(t[0] & 0x80);
        pad_in.touch_id[i] = t[0] & 0x7F;
        pad_in.touch_x[i] = t[1] | ((t[2] & 0x0F) << 8);
        pad_in.touch_y[i] = (t[2] >> 4) | (t[3] << 4);
    }
    pad_in.battery = d[52] & 0x0F;
    pad_in.charging = (d[52] >> 4) & 0x0F;
    if (!connected) {
        connected = 1;
        connect_count++;
        connect_time = GetTickCount64();
        need_setup = 1;
        log_msg("dualsense input running, battery %d0%%", pad_in.battery);
    }
    int mute = (d[9] & 0x04) != 0;
    if (mute && !last_mute && cfg.mute_toggles_gyro) {
        gyro_enabled = !gyro_enabled;
        pad_out.mute_led = gyro_enabled ? 0 : 1;
        log_msg("gyro %s", gyro_enabled ? "on" : "off");
    }
    last_mute = mute;
    LeaveCriticalSection(&lock);
    if (need_setup)
        wake_writer();
}

static DWORD WINAPI reader_main(LPVOID arg)
{
    unsigned char buf[1024];
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    int quiet = 0;
    (void)arg;
    while (running) {
        if (dev == INVALID_HANDLE_VALUE) {
            if (!open_device()) {
                for (int i = 0; i < 10 && running; i++)
                    Sleep(100);
                continue;
            }
            quiet = 0;
        }
        if (need_full_mode) {
            need_full_mode = 0;
            EnterCriticalSection(&dev_lock);
            read_calibration();
            LeaveCriticalSection(&dev_lock);
        }
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD got = 0;
        int len = in_len < (int)sizeof(buf) ? in_len : (int)sizeof(buf);
        if (!ReadFile(dev, buf, len, NULL, &ov)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                log_msg("read failed (%lu)", err);
                close_device();
                continue;
            }
            if (WaitForSingleObject(ev, 1000) == WAIT_TIMEOUT) {
                CancelIoEx(dev, &ov);
                GetOverlappedResult(dev, &ov, &got, TRUE);
                quiet++;
                if (quiet == 2 && is_bt)
                    need_full_mode = 1;
                if (quiet >= 5) {
                    log_msg("no reports for 5s, reopening");
                    close_device();
                }
                continue;
            }
        }
        if (!GetOverlappedResult(dev, &ov, &got, FALSE)) {
            log_msg("read result failed (%lu)", GetLastError());
            close_device();
            continue;
        }
        quiet = 0;
        if (got > 0)
            parse_input(buf, (int)got);
    }
    CloseHandle(ev);
    return 0;
}

static int write_report(const unsigned char *data, int len)
{
    unsigned char buf[1024];
    int ok = 0;
    EnterCriticalSection(&dev_lock);
    if (dev != INVALID_HANDLE_VALUE && out_len > 0 && out_len <= (int)sizeof(buf) && len <= out_len) {
        memset(buf, 0, out_len);
        memcpy(buf, data, len);
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = write_event;
        ResetEvent(write_event);
        DWORD done = 0;
        if (WriteFile(dev, buf, out_len, NULL, &ov)) {
            ok = 1;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            // if (WaitForSingleObject(write_event, 500) == WAIT_OBJECT_0)
            if (WaitForSingleObject(write_event, 200) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(dev, &ov, &done, FALSE);
            else {
                CancelIoEx(dev, &ov);
                GetOverlappedResult(dev, &ov, &done, TRUE);
            }
        }
    }
    LeaveCriticalSection(&dev_lock);
    if (!ok) {
        write_fails++;
        if (write_fails <= 5 || write_fails % 500 == 0)
            log_msg("write of report %02x failed (%d so far)", data[0], write_fails);
    } else if (data[0] == 0x32) {
        hap_writes++;
    }
    return ok;
}

static int scale_level(int s, float k)
{
    s = (int)((s + 1) * k + 0.5f) - 1;
    if (s < 0) s = 0;
    if (s > 7) s = 7;
    return s;
}

static void scale_trigger(unsigned char *dst, const unsigned char *src)
{
    memcpy(dst, src, 11);
    if (!cfg.triggers) {
        memset(dst, 0, 11);
        dst[0] = 0x05;
        return;
    }
    if (dst[0] == 0) {
        dst[0] = 0x05;
        return;
    }
    if (cfg.trigger_strength == 100)
        return;
    float k = cfg.trigger_strength / 100.0f;
    if (dst[0] == 0x21 || dst[0] == 0x26) {
        unsigned int packed = dst[3] | (dst[4] << 8) | (dst[5] << 16) | (dst[6] << 24);
        unsigned int out = 0;
        for (int i = 0; i < 10; i++) {
            int s = scale_level((packed >> (i * 3)) & 7, k);
            out |= (unsigned int)s << (i * 3);
        }
        dst[3] = out & 0xFF;
        dst[4] = (out >> 8) & 0xFF;
        dst[5] = (out >> 16) & 0xFF;
        dst[6] = (out >> 24) & 0xFF;
    } else if (dst[0] == 0x25) {
        dst[3] = (unsigned char)scale_level(dst[3] & 7, k);
    } else if (dst[0] == 0x22 || dst[0] == 0x27) {
        int a = scale_level(dst[3] & 7, k);
        int b = scale_level((dst[3] >> 3) & 7, k);
        dst[3] = (unsigned char)(a | (b << 3));
    }
}

static int send_main_report(int setup, int hd_on, int flash_red)
{
    unsigned char c[47];
    PadOutput o;
    EnterCriticalSection(&lock);
    o = pad_out;
    LeaveCriticalSection(&lock);
    memset(c, 0, sizeof(c));

    if (setup) {
        c[38] = 0x02;
        c[41] = 0x02;
    } else {
        int big = o.motor_left * cfg.rumble_strength / 100;
        int little = o.motor_right * cfg.rumble_strength / 100;
        if (big > 255) big = 255;
        if (little > 255) little = 255;
        if (hd_on) {
            big = 0;
            little = 0;
        }
        c[0] = 0x04 | 0x08;
        if (!hd_on) {
            c[0] |= 0x02;
            if (vibration_v2)
                c[38] |= 0x04;
            else
                c[0] |= 0x01;
        }
        c[2] = (unsigned char)little;
        c[3] = (unsigned char)big;
        scale_trigger(&c[10], o.trig_r);
        scale_trigger(&c[21], o.trig_l);
        c[1] = 0x01 | 0x04 | 0x10;
        c[8] = o.mute_led;
        c[42] = 0x00;
        int leds = o.player_leds;
        if (cfg.battery_on_connect && GetTickCount64() - connect_time < 3000) {
            int n = (pad_in.battery + 1) / 2;
            leds = 0;
            for (int i = 0; i < n && i < 5; i++)
                leds |= 1 << i;
        }
        c[43] = (unsigned char)(cfg.player_leds ? (leds | 0x20) : 0x20);
        int r = o.r, g = o.g, b = o.b;
        if (light_override) {
            r = light_override_rgb[0];
            g = light_override_rgb[1];
            b = light_override_rgb[2];
        }
        if (flash_red) {
            r = 255;
            g = 0;
            b = 0;
        }
        c[44] = (unsigned char)(r * cfg.lightbar_brightness / 100);
        c[45] = (unsigned char)(g * cfg.lightbar_brightness / 100);
        c[46] = (unsigned char)(b * cfg.lightbar_brightness / 100);
    }

    if (is_bt) {
        unsigned char r[78];
        memset(r, 0, sizeof(r));
        r[0] = 0x31;
        r[1] = (unsigned char)(out_seq << 4);
        r[2] = 0x10;
        out_seq = (out_seq + 1) & 0x0F;
        memcpy(&r[3], c, 47);
        unsigned int crc = bt_crc(0xA2, r, 74);
        memcpy(&r[74], &crc, 4);
        return write_report(r, 78);
    } else {
        unsigned char r[63];
        memset(r, 0, sizeof(r));
        r[0] = 0x02;
        memcpy(&r[1], c, 47);
        return write_report(r, 63);
    }
}

static unsigned char hap_counter = 0;

static int send_haptics(void)
{
    unsigned char r[142];
    memset(r, 0, sizeof(r));
    r[0] = 0x32;
    r[1] = 0x00;
    r[2] = 0x11 | 0x80;
    r[3] = 7;
    r[4] = 0xFE;
    r[9] = 0xFF;
    r[10] = hap_counter++;
    r[11] = 0x12 | 0x80;
    r[12] = 64;
    haptics_fill((signed char *)&r[13], 32);
    unsigned int crc = bt_crc(0xA2, r, 138);
    memcpy(&r[138], &crc, 4);
    return write_report(r, 142);
}

static DWORD WINAPI writer_main(LPVOID arg)
{
    LARGE_INTEGER freq, now;
    (void)arg;
    QueryPerformanceFrequency(&freq);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    double period = 64.0 / 6000.0;
    QueryPerformanceCounter(&now);
    double next = (double)now.QuadPart / freq.QuadPart;
    double last_main = 0;
    double last_change = 0;
    double low_batt_timer = 0;
    PadOutput sent;
    int sent_valid = 0;
    int idle_packets = 0;
    int hd_was_on = 0;
    int last_override = -1;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (running) {
        next += period;
        QueryPerformanceCounter(&now);
        double t = (double)now.QuadPart / freq.QuadPart;
        if (next < t - period * 4)
            next = t + period;
        double wait = next - t;
        if (wait > 0) {
            HANDLE objs[2] = { timer, wake_event };
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(wait * 10000000.0);
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            DWORD w = WaitForMultipleObjects(2, objs, FALSE, 100);
            if (w == WAIT_OBJECT_0 + 1) {
                CancelWaitableTimer(timer);
                next -= period;
            }
        }
        if (!running)
            break;
        script_watchdog();
        if (!connected) {
            sent_valid = 0;
            continue;
        }
        QueryPerformanceCounter(&now);
        t = (double)now.QuadPart / freq.QuadPart;

        if (need_setup) {
            need_setup = 0;
            send_main_report(1, 0, 0);
            Sleep(10);
            sent_valid = 0;
        }

        int hd_on = cfg.rumble_hd && is_bt;
        if (hd_on) {
            if (haptics_busy()) {
                send_haptics();
                idle_packets = 0;
            } else if (idle_packets < 8) {
                send_haptics();
                idle_packets++;
            }
        }

        int flash = 0;
        if (cfg.low_battery_warning && pad_in.battery <= 1 && pad_in.charging == 0) {
            if (t - low_batt_timer > 60)
                low_batt_timer = t;
            double ph = t - low_batt_timer;
            flash = ph < 1.5 && fmod(ph, 0.5) < 0.25;
        }

        PadOutput o;
        EnterCriticalSection(&lock);
        o = pad_out;
        LeaveCriticalSection(&lock);
        int changed = !sent_valid || memcmp(&o, &sent, sizeof(o)) != 0 || hd_on != hd_was_on ||
                      light_override != last_override || flash;
        if (changed)
            last_change = t;
        if ((changed && t - last_main >= 0.008) || t - last_main >= 1.0 ||
            (t - last_change < 0.05 && t - last_main >= 0.02)) {
            if (send_main_report(0, hd_on, flash)) {
                sent = o;
                sent_valid = 1;
                hd_was_on = hd_on;
                last_override = light_override;
            }
            last_main = t;
        }
    }
    if (timer)
        CloseHandle(timer);
    return 0;
}

void wake_writer(void)
{
    if (wake_event)
        SetEvent(wake_event);
}

void device_start(void)
{
    if (running)
        return;
    make_crc_table();
    InitializeCriticalSection(&dev_lock);
    pad_in.lx = pad_in.ly = pad_in.rx = pad_in.ry = 0x80;
    wake_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    running = 1;
    reader_thread = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
    writer_thread = CreateThread(NULL, 0, writer_main, NULL, 0, NULL);
}

void device_reset_now(void)
{
    unsigned char r[78];
    unsigned char buf[1024];
    HANDLE h = dev;
    if (h == INVALID_HANDLE_VALUE || out_len <= 0 || out_len > (int)sizeof(buf))
        return;
    memset(r, 0, sizeof(r));
    if (is_bt) {
        r[0] = 0x31;
        r[1] = (unsigned char)(out_seq << 4);
        r[2] = 0x10;
        r[3] = 0x02 | 0x04 | 0x08;
        r[3 + 10] = 0x05;
        r[3 + 21] = 0x05;
        r[3 + 1] = 0x01 | 0x04 | 0x10;
        r[3 + 43] = 0x04 | 0x20;
        r[3 + 46] = 60;
        if (vibration_v2)
            r[3 + 38] = 0x04;
        else
            r[3] |= 0x01;
        unsigned int crc = bt_crc(0xA2, r, 74);
        memcpy(&r[74], &crc, 4);
    } else {
        r[0] = 0x02;
        r[1] = 0x02 | 0x04 | 0x08;
        r[1 + 10] = 0x05;
        r[1 + 21] = 0x05;
        r[1 + 1] = 0x01 | 0x04 | 0x10;
        r[1 + 43] = 0x04 | 0x20;
        r[1 + 46] = 60;
        if (vibration_v2)
            r[1 + 38] = 0x04;
        else
            r[1] |= 0x01;
    }
    memset(buf, 0, out_len);
    memcpy(buf, r, is_bt ? 78 : 63);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD done = 0;
    if (!WriteFile(h, buf, out_len, NULL, &ov) && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 150) == WAIT_OBJECT_0)
            GetOverlappedResult(h, &ov, &done, FALSE);
        else
            CancelIoEx(h, &ov);
    }
    if (ov.hEvent)
        CloseHandle(ov.hEvent);
}

void device_stop(void)
{
    if (!running)
        return;
    EnterCriticalSection(&lock);
    memset(pad_out.trig_l, 0, 11);
    memset(pad_out.trig_r, 0, 11);
    pad_out.motor_left = pad_out.motor_right = 0;
    LeaveCriticalSection(&lock);
    if (connected)
        send_main_report(0, 0, 0);
    running = 0;
    wake_writer();
    if (writer_thread) {
        WaitForSingleObject(writer_thread, 500);
        CloseHandle(writer_thread);
    }
    EnterCriticalSection(&dev_lock);
    if (dev != INVALID_HANDLE_VALUE)
        CancelIoEx(dev, NULL);
    LeaveCriticalSection(&dev_lock);
    if (reader_thread) {
        WaitForSingleObject(reader_thread, 1500);
        CloseHandle(reader_thread);
    }
}

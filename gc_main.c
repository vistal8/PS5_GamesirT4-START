/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ghost-Control v3: USB HID controller → virtual DualSense on PS5
 *
 * Follows Ghostpad's proven VDA path exactly:
 *   1. scePadVirtualDeviceAddDevice(userId=1, type=3)
 *   2. Monitor klogsrv:3232 for DEVICE_ADDED [type:1][subType:22]
 *   3. shellui_pad_force_bind(vDevId, fgUserId) via PT_ATTACH SceShellUI
 *   4. scePadVirtualDeviceInsertData(handle, &padData) directly from this process
 *
 * USB HID input (new piece in front of Ghostpad's VDI path):
 *   /dev/ugen2.2 → USB_IFACE_DRIVER_DETACH → USB_FS_OPEN ep=0x81
 *   → Nintendo handshake → 0x30/0x3f report loop → parse → VDI
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#endif

#include "shellui_pad.h"

/* ------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------ */

#define LOG_DIR  "/data/ghostpad"
#define LOG_PATH "/data/ghostpad/gc_status.log"
#define PID_PATH "/data/ghostpad/gc_main.pid"
#define LOG_MAX  480

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_log_fd = -1;

void ghostpad_status_log_reset(void) {
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
    mkdir(LOG_DIR, 0755);
    g_log_fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    pthread_mutex_unlock(&g_log_lock);
}

void ghostpad_status_log(const char *fmt, ...) {
    char buf[LOG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    va_end(ap);
    buf[LOG_MAX-1] = '\0';
    klog_printf("%s", buf);
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) {
        size_t n = strnlen(buf, sizeof(buf));
        write(g_log_fd, buf, n);
        if (n && buf[n-1] != '\n') write(g_log_fd, "\n", 1);
    }
    pthread_mutex_unlock(&g_log_lock);
}

#define gp_log(...) ghostpad_status_log("[GC] " __VA_ARGS__)

/* ------------------------------------------------------------------
 * SCE stubs
 * ------------------------------------------------------------------ */

extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *outUserId);
extern int32_t sceUserServiceGetForegroundUser(int32_t *outUserId);
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadGetHandle(int32_t userId, int32_t type, int32_t index);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t deviceType);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *padData);
extern int32_t sceKernelSendNotificationRequest(int unk0, void *req, size_t size, int unk1);

/* ------------------------------------------------------------------
 * SCE button constants
 * ------------------------------------------------------------------ */

#define SCE_PAD_BUTTON_L3        0x00000002u
#define SCE_PAD_BUTTON_R3        0x00000004u
#define SCE_PAD_BUTTON_OPTIONS   0x00000008u
#define SCE_PAD_BUTTON_UP        0x00000010u
#define SCE_PAD_BUTTON_RIGHT     0x00000020u
#define SCE_PAD_BUTTON_DOWN      0x00000040u
#define SCE_PAD_BUTTON_LEFT      0x00000080u
#define SCE_PAD_BUTTON_L2        0x00000100u
#define SCE_PAD_BUTTON_R2        0x00000200u
#define SCE_PAD_BUTTON_L1        0x00000400u
#define SCE_PAD_BUTTON_R1        0x00000800u
#define SCE_PAD_BUTTON_TRIANGLE  0x00001000u
#define SCE_PAD_BUTTON_CIRCLE    0x00002000u
#define SCE_PAD_BUTTON_CROSS     0x00004000u
#define SCE_PAD_BUTTON_SQUARE    0x00008000u
#define SCE_PAD_BUTTON_CREATE    0x00010000u
#define SCE_PAD_BUTTON_PS        0x00010000u
#define SCE_PAD_BUTTON_TOUCH_PAD 0x00100000u

typedef struct { uint16_t x; uint16_t y; uint8_t finger; uint8_t pad[3]; } ScePadTouch;
typedef struct {
    uint8_t fingers; uint8_t pad1[3]; uint32_t pad2; ScePadTouch touch[2];
} ScePadTouchData;
typedef struct {
    uint32_t buttons;
    struct { uint8_t x; uint8_t y; } leftStick;
    struct { uint8_t x; uint8_t y; } rightStick;
    struct { uint8_t l2; uint8_t r2; } analogButtons;
    uint16_t padding;
    struct { float x, y, z, w; } quat;
    struct { float x, y, z; } vel;
    struct { float x, y, z; } accel;
    ScePadTouchData touchData;
    uint8_t connected;
    uint8_t _align[3];
    uint64_t timestamp;
    uint8_t ext[16];
    uint8_t count;
    uint8_t unknown[15];
} ScePadData;

#define VIRTUAL_DEVICE_TYPE_DUALSENSE 3

/* ------------------------------------------------------------------
 * Shared state
 * ------------------------------------------------------------------ */

static volatile int32_t  g_vdi_handle  = -1;
static volatile int      g_vdi_ready   = 0;
/* Actual foreground userId — used for force_bind */
static int32_t           g_inject_uid  = 0x10000000;

/* klog monitoring: set when DEVICE_ADDED [type:1][subType:22] seen */
static pthread_mutex_t   g_klog_lock   = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_klog_vdev_id = 0;

/* ------------------------------------------------------------------
 * Notification
 * ------------------------------------------------------------------ */

typedef struct { char _unk[45]; char message[3075]; } NotifyRequest;
static void notify(const char *fmt, ...) {
    NotifyRequest req; va_list ap;
    memset(&req, 0, sizeof(req));
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ------------------------------------------------------------------
 * klog line parser — extract virtual DualSense DEVICE_ADDED deviceId
 * ------------------------------------------------------------------ */

static uint64_t parse_hex_str(const char *s) {
    uint64_t v = 0;
    while (*s) {
        char c = *s++;
        if (c >= '0' && c <= '9')      v = (v<<4)|(c-'0');
        else if (c >= 'a' && c <= 'f') v = (v<<4)|(c-'a'+10);
        else if (c >= 'A' && c <= 'F') v = (v<<4)|(c-'A'+10);
        else break;
    }
    return v;
}

static void parse_klog_line(const char *line) {
    /* Looking for: DEVICE_ADDED ... [type:1][subType:22][capabilityBattery:0]
     * That's the virtual DualSense device we created with VDA(type=3). */
    if (!strstr(line, "DEVICE_ADDED"))   return;
    if (!strstr(line, "subType:22"))     return;
    if (!strstr(line, "capabilityBattery:0")) return;

    /* Extract DeviceId:0x... or deviceId=0x... */
    const char *p = strstr(line, "DeviceId:0x");
    if (!p) p = strstr(line, "deviceId=0x");
    if (!p) return;
    p += 11; /* skip "DeviceId:0x" or "deviceId=0x" */

    uint64_t dev_id = parse_hex_str(p);
    if (!dev_id) return;

    pthread_mutex_lock(&g_klog_lock);
    if (!g_klog_vdev_id) {
        g_klog_vdev_id = dev_id;
        gp_log("klog: VDA device 0x%llx\n", (unsigned long long)dev_id);
    }
    pthread_mutex_unlock(&g_klog_lock);
}

/* ------------------------------------------------------------------
 * klog capture thread — connects to klogsrv:3232 (since /dev/klog
 * is unavailable to payload processes: errno=16 EBUSY)
 * ------------------------------------------------------------------ */

static void *klog_capture_thread(void *arg) {
    (void)arg;
    char buf[512];
    char line[1024];
    size_t line_len = 0;
    int fd = -1;

    /* Try /dev/klog first; fall back to klogsrv TCP on port 3232 */
    fd = open("/dev/klog", O_RDONLY);
    if (fd < 0) {
        gp_log("klog: /dev/klog errno=%d, trying klogsrv TCP 127.0.0.1:3232\n", errno);
        struct sockaddr_in sin;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { gp_log("klog: socket failed errno=%d\n", errno); return NULL; }
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = inet_addr("127.0.0.1");
        sin.sin_port = htons(3232);
        for (int retry = 0; retry < 10; retry++) {
            if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
                fd = sock;
                gp_log("klog: connected to klogsrv TCP 127.0.0.1:3232\n");
                break;
            }
            usleep(500000);
        }
        if (fd < 0) {
            gp_log("klog: could not connect to klogsrv errno=%d\n", errno);
            close(sock);
            return NULL;
        }
    }

    while (1) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EINTR) continue;
            gp_log("klog: read error errno=%d\n", errno);
            break;
        }
        if (len == 0) { usleep(10000); continue; }

        for (ssize_t i = 0; i < len; i++) {
            char c = buf[i];
            if (c == '\n' || line_len >= sizeof(line)-1) {
                line[line_len] = '\0';
                parse_klog_line(line);
                line_len = 0;
            } else if (c != '\r') {
                line[line_len++] = c;
            }
        }
    }
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------
 * Nintendo input parsing
 * ------------------------------------------------------------------ */

static uint8_t ntoh_stick(uint16_t v) {
    if (v > 4095) v = 4095;
    return (uint8_t)((v * 255u) / 4095u);
}

static uint32_t hat_to_dpad(uint8_t hat) {
    switch (hat) {
    case 0: return SCE_PAD_BUTTON_UP;
    case 1: return SCE_PAD_BUTTON_UP   | SCE_PAD_BUTTON_RIGHT;
    case 2: return SCE_PAD_BUTTON_RIGHT;
    case 3: return SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_RIGHT;
    case 4: return SCE_PAD_BUTTON_DOWN;
    case 5: return SCE_PAD_BUTTON_DOWN | SCE_PAD_BUTTON_LEFT;
    case 6: return SCE_PAD_BUTTON_LEFT;
    case 7: return SCE_PAD_BUTTON_UP   | SCE_PAD_BUTTON_LEFT;
    default: return 0;
    }
}

/* Report 0x30: full 60Hz input after Nintendo handshake
 * [3]=right btns  [4]=shared  [5]=left btns  [6-8]=lstick  [9-11]=rstick */
static void parse_0x30(const uint8_t *b, ScePadData *o) {
    uint8_t br=b[3], bs=b[4], bl=b[5];
    uint16_t lx=(uint16_t)(b[6]|((b[7]&0xF)<<8));
    uint16_t ly=(uint16_t)((b[7]>>4)|((uint16_t)b[8]<<4));
    uint16_t rx=(uint16_t)(b[9]|((b[10]&0xF)<<8));
    uint16_t ry=(uint16_t)((b[10]>>4)|((uint16_t)b[11]<<4));
    uint32_t btn=0;
    if(br&0x04) btn|=SCE_PAD_BUTTON_CROSS;
    if(br&0x08) btn|=SCE_PAD_BUTTON_CIRCLE;
    if(br&0x01) btn|=SCE_PAD_BUTTON_SQUARE;
    if(br&0x02) btn|=SCE_PAD_BUTTON_TRIANGLE;
    if(bl&0x40) btn|=SCE_PAD_BUTTON_L1;
    if(bl&0x80) btn|=SCE_PAD_BUTTON_L2;
    if(br&0x40) btn|=SCE_PAD_BUTTON_R1;
    if(br&0x80) btn|=SCE_PAD_BUTTON_R2;
    if(bs&0x08) btn|=SCE_PAD_BUTTON_L3;
    if(bs&0x04) btn|=SCE_PAD_BUTTON_R3;
    if(bs&0x02) btn|=SCE_PAD_BUTTON_OPTIONS;
    if(bs&0x01) btn|=SCE_PAD_BUTTON_CREATE;
    if(bs&0x10) btn|=SCE_PAD_BUTTON_PS;
    if(bs&0x20) btn|=SCE_PAD_BUTTON_TOUCH_PAD;
    if(bl&0x02) btn|=SCE_PAD_BUTTON_UP;
    if(bl&0x01) btn|=SCE_PAD_BUTTON_DOWN;
    if(bl&0x04) btn|=SCE_PAD_BUTTON_RIGHT;
    if(bl&0x08) btn|=SCE_PAD_BUTTON_LEFT;
    o->buttons=btn;
    o->leftStick.x=ntoh_stick(lx); o->leftStick.y=ntoh_stick(ly);
    o->rightStick.x=ntoh_stick(rx); o->rightStick.y=ntoh_stick(ry);
    o->analogButtons.l2=(bl&0x80)?255:0;
    o->analogButtons.r2=(br&0x80)?255:0;
    o->connected=1; o->quat.w=1.0f;
}

/* Report 0x3f: simple input on button change only
 * [1]=right btns  [2]=shared  [3]=HAT  [4]=lx [5]=ly [6]=rx [7]=ry [8]=L/ZL */
static void parse_0x3f(const uint8_t *b, ScePadData *o) {
    uint8_t b1=b[1],b2=b[2],hat=b[3],b8=b[8];
    uint32_t btn=0;
    if(b1&0x04) btn|=SCE_PAD_BUTTON_CROSS;
    if(b1&0x08) btn|=SCE_PAD_BUTTON_CIRCLE;
    if(b1&0x01) btn|=SCE_PAD_BUTTON_SQUARE;
    if(b1&0x02) btn|=SCE_PAD_BUTTON_TRIANGLE;
    if(b8&0x40) btn|=SCE_PAD_BUTTON_L1;
    if(b8&0x80) btn|=SCE_PAD_BUTTON_L2;
    if(b1&0x40) btn|=SCE_PAD_BUTTON_R1;
    if(b1&0x80) btn|=SCE_PAD_BUTTON_R2;
    if(b2&0x08) btn|=SCE_PAD_BUTTON_L3;
    if(b2&0x04) btn|=SCE_PAD_BUTTON_R3;
    if(b2&0x02) btn|=SCE_PAD_BUTTON_OPTIONS;
    if(b2&0x01) btn|=SCE_PAD_BUTTON_CREATE;
    if(b2&0x10) btn|=SCE_PAD_BUTTON_PS;
    if(b2&0x20) btn|=SCE_PAD_BUTTON_TOUCH_PAD;
    btn|=hat_to_dpad(hat);
    o->buttons=btn;
    o->leftStick.x=b[4]; o->leftStick.y=b[5];
    o->rightStick.x=b[6]; o->rightStick.y=b[7];
    o->analogButtons.l2=(b8&0x80)?255:0;
    o->analogButtons.r2=(b1&0x80)?255:0;
    o->connected=1; o->quat.w=1.0f;
}

/* ------------------------------------------------------------------
 * ugen device detection
 * ------------------------------------------------------------------ */

static const char *UGEN_PATHS[] = {
    "/dev/ugen2.2","/dev/ugen2.3","/dev/ugen2.4","/dev/ugen2.5",
    "/dev/ugen1.2","/dev/ugen0.2","/dev/ugen0.3",
};
#define N_UGEN_PATHS ((int)(sizeof(UGEN_PATHS)/sizeof(UGEN_PATHS[0])))

#define VID_NATIVE  0x2dc8u
#define PID_NATIVE  0x310bu
#define VID_SWITCH  0x057eu
#define PID_SWITCH  0x2009u

static int ugen_find_target(const char **out_path, uint16_t *out_vid, uint16_t *out_pid) {
    for (int i = 0; i < N_UGEN_PATHS; i++) {
        /* Try O_RDWR — PS5 ugen requires RDWR for USB ioctls.
         * USB_GET_DEVICEINFO returns ENOTTY with O_RDONLY on PS5.
         * O_NONBLOCK prevents blocking if the device is busy. */
        int fd = open(UGEN_PATHS[i], O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            if (errno != ENOENT)
                gp_log("ugen_find: open(%s) errno=%d\n", UGEN_PATHS[i], errno);
            continue;
        }
        struct usb_device_info di;
        memset(&di, 0, sizeof(di));
        int ok = 0;
        int r = ioctl(fd, USB_GET_DEVICEINFO, &di);
        if (r != 0) {
            if (errno == ENOTTY) {
                if (strncmp(UGEN_PATHS[i], "/dev/ugen2.", 11) == 0) {
                    gp_log("ugen_find: %s USB_GET_DEVICEINFO ENOTTY, trying as Nintendo candidate\n",
                           UGEN_PATHS[i]);
                    if (out_vid) *out_vid = VID_SWITCH;
                    if (out_pid) *out_pid = PID_SWITCH;
                    if (out_path) *out_path = UGEN_PATHS[i];
                    close(fd);
                    return 1;
                }
                gp_log("ugen_find: %s USB_GET_DEVICEINFO ENOTTY, ignored non-external bus\n",
                       UGEN_PATHS[i]);
                close(fd);
                continue;
            }
            if (errno != ENOENT)
                gp_log("ugen_find: %s USB_GET_DEVICEINFO errno=%d\n",
                       UGEN_PATHS[i], errno);
            close(fd);
            continue;
        }
        if (r == 0) {
            gp_log("ugen_find: %s VID=0x%04x PID=0x%04x\n",
                   UGEN_PATHS[i], di.udi_vendorNo, di.udi_productNo);
            if ((di.udi_vendorNo == VID_NATIVE && di.udi_productNo == PID_NATIVE) ||
                (di.udi_vendorNo == VID_SWITCH && di.udi_productNo == PID_SWITCH)) {
                if (out_vid) *out_vid = di.udi_vendorNo;
                if (out_pid) *out_pid = di.udi_productNo;
                if (out_path) *out_path = UGEN_PATHS[i];
                ok = 1;
            }
        } else {
            /* ENOTTY = PS5 ugen doesn't support this ioctl in current state.
             * Try a USB_FS_INIT probe to check if the device accepts ugen FS ops.
             * If it does, assume it's our target (only one USB game controller expected). */
            struct usb_fs_endpoint ep_probe;
            struct usb_fs_init init_probe;
            memset(&ep_probe, 0, sizeof(ep_probe));
            memset(&init_probe, 0, sizeof(init_probe));
            init_probe.pEndpoints = &ep_probe;
            init_probe.ep_index_max = 1;
            int ir = ioctl(fd, USB_FS_INIT, &init_probe);
            gp_log("ugen_find: %s DEVICEINFO errno=%d FS_INIT=%d\n",
                   UGEN_PATHS[i], errno, ir);
            if (ir == 0) {
                /* FS_INIT succeeded. Verify endpoint by opening ep=0x81 and
                 * checking max_pkt_length == 64 (8BitDo Nintendo IN ep is always 64). */
                struct usb_fs_open probe_open;
                memset(&probe_open, 0, sizeof(probe_open));
                probe_open.ep_index = 0;
                probe_open.ep_no = 0x81;
                probe_open.max_bufsize = 64;
                probe_open.max_frames = 1;
                if (ioctl(fd, USB_FS_OPEN, &probe_open) == 0 &&
                    probe_open.max_packet_length == 64) {
                    gp_log("ugen_find: %s ep=0x81 max_pkt=%u — 8BitDo!\n",
                           UGEN_PATHS[i], (unsigned)probe_open.max_packet_length);
                    struct usb_fs_close pc; memset(&pc,0,sizeof(pc)); pc.ep_index=0;
                    ioctl(fd, USB_FS_CLOSE, &pc);
                    if (out_vid) *out_vid = VID_SWITCH;
                    if (out_pid) *out_pid = PID_SWITCH;
                    if (out_path) *out_path = UGEN_PATHS[i];
                    ok = 1;
                } else {
                    gp_log("ugen_find: %s ep probe failed or wrong pkt size %u\n",
                           UGEN_PATHS[i], (unsigned)probe_open.max_packet_length);
                }
                struct usb_fs_uninit un; memset(&un,0,sizeof(un));
                ioctl(fd, USB_FS_UNINIT, &un);
            }
        }
        close(fd);
        if (ok) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Inject pad data via direct VDI (Ghostpad path)
 * ------------------------------------------------------------------ */

static void inject_pad(const ScePadData *pad) {
    int32_t h = g_vdi_handle;
    if (h < 0) return;
    int vr = scePadVirtualDeviceInsertData(h, pad);
    static uint32_t inject_count = 0;
    inject_count++;
    if (inject_count <= 8 || (inject_count % 300) == 0) {
        gp_log("VDI inject #%u ret=0x%08x buttons=0x%08x ls=%u,%u rs=%u,%u lt=%u rt=%u\n",
               inject_count, (uint32_t)vr, pad->buttons,
               pad->leftStick.x, pad->leftStick.y,
               pad->rightStick.x, pad->rightStick.y,
               pad->analogButtons.l2, pad->analogButtons.r2);
    }
    static int logged = 0;
    if (vr != 0 && !logged) {
        gp_log("VDI error 0x%08x (logged once)\n", (uint32_t)vr);
        logged = 1;
    }
}

static int usb_fs_send_out_report(int fd, struct usb_fs_endpoint *ep,
                                  const uint8_t *data, uint32_t len,
                                  const char *tag) {
    void *out_buffers[1] = { (void *)data };
    uint32_t out_lengths[1] = { len };
    struct usb_fs_start start;
    struct usb_fs_complete complete;

    ep->ppBuffer = out_buffers;
    ep->pLength = out_lengths;
    ep->nFrames = 1;
    ep->timeout = 100;
    ep->flags = 0;
    ep->aFrames = 0;
    ep->status = 0;

    memset(&start, 0, sizeof(start));
    start.ep_index = 1;
    if (ioctl(fd, USB_FS_START, &start) != 0) {
        int e = errno;
        gp_log("OUT %s START failed errno=%d ep_status=%d aFrames=%u len=%u\n",
               tag, e, ep->status, ep->aFrames, len);
        return -errno;
    }

    for (int wait = 0; wait < 20; wait++) {
        memset(&complete, 0, sizeof(complete));
        complete.ep_index = 1;
        if (ioctl(fd, USB_FS_COMPLETE, &complete) == 0) {
            gp_log("OUT %s complete ok wait=%d ep_status=%d aFrames=%u len=%u\n",
                   tag, wait, ep->status, ep->aFrames, out_lengths[0]);
            return 0;
        }
        if (errno != EBUSY) {
            int e = errno;
            gp_log("OUT %s COMPLETE failed errno=%d ep_status=%d aFrames=%u len=%u\n",
                   tag, e, ep->status, ep->aFrames, out_lengths[0]);
            return -errno;
        }
        usleep(50000);
    }

    gp_log("OUT %s COMPLETE still busy ep_status=%d aFrames=%u len=%u\n",
           tag, ep->status, ep->aFrames, out_lengths[0]);
    return -EBUSY;
}

static int usb_fs_send_out_cmd(int fd, struct usb_fs_endpoint *ep, uint8_t a, uint8_t b) {
    uint8_t out_buf[2] = { a, b };
    char tag[16];
    snprintf(tag, sizeof(tag), "%02x %02x", a, b);
    return usb_fs_send_out_report(fd, ep, out_buf, sizeof(out_buf), tag);
}

static int nintendo_send_subcmd(int fd, struct usb_fs_endpoint *ep,
                                uint8_t *seq, uint8_t subcmd,
                                const uint8_t *data, uint32_t data_len) {
    static const uint8_t neutral_rumble[8] = {
        0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40
    };
    uint8_t out_buf[64];
    char tag[32];
    uint32_t len = 11 + data_len;

    if (len > sizeof(out_buf))
        return -EINVAL;

    memset(out_buf, 0, sizeof(out_buf));
    out_buf[0] = 0x01;
    out_buf[1] = *seq & 0x0f;
    memcpy(out_buf + 2, neutral_rumble, sizeof(neutral_rumble));
    out_buf[10] = subcmd;
    if (data_len)
        memcpy(out_buf + 11, data, data_len);
    *seq = (uint8_t)((*seq + 1) & 0x0f);

    snprintf(tag, sizeof(tag), "subcmd %02x", subcmd);
    return usb_fs_send_out_report(fd, ep, out_buf, len, tag);
}

static void nintendo_maybe_poll_state(int fd, struct usb_fs_endpoint *ep,
                                      uint8_t *seq, int enabled) {
    static struct timeval last_poll;
    struct timeval now;
    long elapsed_ms;

    if (!enabled)
        return;
    gettimeofday(&now, NULL);
    elapsed_ms = (now.tv_sec - last_poll.tv_sec) * 1000L +
                 (now.tv_usec - last_poll.tv_usec) / 1000L;
    if (last_poll.tv_sec != 0 && elapsed_ms < 33)
        return;
    last_poll = now;
    (void)nintendo_send_subcmd(fd, ep, seq, 0x00, NULL, 0);
}

/* ------------------------------------------------------------------
 * USB HID reader thread
 * ------------------------------------------------------------------ */

/* Handshake states for Nintendo Pro Controller USB init */
#define HS_WAIT_81_01 0  /* waiting for 0x81 sub=0x01 */
#define HS_WAIT_81_02 1  /* saw 0x81 0x01, sent [80 02], waiting for 0x81 0x02 */
#define HS_STREAMING  2  /* subcmds sent, reading 0x00/0x30 data */

static void *usb_hid_thread(void *arg) {
    (void)arg;
    const char *dev_path = NULL;
    uint16_t vid=0, pid=0;
    struct usb_fs_endpoint eps[2];
    struct usb_fs_init init;
    struct usb_fs_open fs_open;
    struct usb_fs_open fs_out_open;
    struct usb_fs_start start;
    struct usb_fs_complete complete;
    struct usb_fs_stop stop;
    struct usb_fs_close fs_close;
    struct usb_fs_uninit uninit;
    uint8_t buf[64];
    void *buffers[1]; uint32_t lengths[1];
    int fd = -1;
    int out_opened = 0;

    gp_log("USB thread started\n");

retry_find:
    dev_path = NULL; vid = 0; pid = 0;
    out_opened = 0;
    { int _scan=0;
      while (!ugen_find_target(&dev_path, &vid, &pid)) {
          if (_scan==0 || _scan==5) gp_log("USB scan #%d: no 8BitDo found\n",_scan);
          _scan++; usleep(2000000);
      }
    }
    gp_log("USB candidate at %s VID=0x%04x PID=0x%04x\n", dev_path, vid, pid);
    {
        const char *ctlr_name =
            (vid==VID_SWITCH && pid==PID_SWITCH) ? "8BitDo (Nintendo Pro mode)" :
            (vid==VID_NATIVE && pid==PID_NATIVE) ? "8BitDo (Native mode)" :
            "Unknown controller";
        notify("Ghostcontrol: Detected %s", ctlr_name);
    }

    fd = open(dev_path, O_RDWR);
    if (fd < 0) { gp_log("open failed errno=%d\n", errno); goto retry_find; }

    /* Probe-matching two-stage USB FS setup. */
    memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
    init.pEndpoints=eps; init.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&init)!=0) {
        gp_log("USB_FS_INIT pass1 failed errno=%d\n",errno);
        close(fd); goto retry_find;
    }

    int iface0 = 0;
    int dr0 = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface0);
    int de0 = (dr0 == 0) ? 0 : errno;
    int iface1 = 1;
    int dr1 = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface1);
    int de1 = (dr1 == 0) ? 0 : errno;
    gp_log("USB_IFACE_DRIVER_DETACH pass1(0)=%d errno=%d, pass1(1)=%d errno=%d\n",
           dr0, de0, dr1, de1);

    memset(&fs_open,0,sizeof(fs_open));
    fs_open.ep_index=0; fs_open.ep_no=0x81;
    fs_open.max_bufsize=sizeof(buf); fs_open.max_frames=1;
    if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0) {
        gp_log("USB_FS_OPEN pass1 IN failed errno=%d\n",errno);
        goto uninit_retry;
    }
    gp_log("USB_FS_OPEN pass1 IN ok max_pkt=%u\n",
           (unsigned)fs_open.max_packet_length);

    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd); fd = -1;

    fd = open(dev_path, O_RDWR);
    if (fd < 0) { gp_log("reopen failed errno=%d\n", errno); goto retry_find; }

    iface0 = 0;
    dr0 = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface0);
    de0 = (dr0 == 0) ? 0 : errno;
    iface1 = 1;
    dr1 = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface1);
    de1 = (dr1 == 0) ? 0 : errno;
    gp_log("USB_IFACE_DRIVER_DETACH pass2(0)=%d errno=%d, pass2(1)=%d errno=%d\n",
           dr0, de0, dr1, de1);

    memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
    init.pEndpoints=eps; init.ep_index_max=2;
    if (ioctl(fd,USB_FS_INIT,&init)!=0) {
        gp_log("USB_FS_INIT pass2 failed errno=%d\n",errno);
        close(fd); goto retry_find;
    }

    memset(&fs_open,0,sizeof(fs_open));
    fs_open.ep_index=0; fs_open.ep_no=0x81;
    fs_open.max_bufsize=sizeof(buf); fs_open.max_frames=1;
    if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0) {
        gp_log("USB_FS_OPEN pass2 IN failed errno=%d\n",errno);
        goto uninit_retry;
    }
    gp_log("USB_FS_OPEN pass2 IN ok max_pkt=%u\n",
           (unsigned)fs_open.max_packet_length);
    if (fs_open.max_packet_length != 64) {
        gp_log("USB_FS_OPEN IN max_pkt=%u is not controller HID, rejecting %s\n",
               (unsigned)fs_open.max_packet_length, dev_path);
        goto reinit;
    }

    buffers[0]=buf; lengths[0]=sizeof(buf);
    eps[0].ppBuffer=buffers; eps[0].pLength=lengths;
    eps[0].nFrames=1; eps[0].timeout=200;
    eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

    out_opened = 0;
    memset(&fs_out_open,0,sizeof(fs_out_open));
    fs_out_open.ep_index=1; fs_out_open.ep_no=0x02;
    fs_out_open.max_bufsize=64; fs_out_open.max_frames=1;
    if (ioctl(fd,USB_FS_OPEN,&fs_out_open)==0) {
        out_opened = 1;
        gp_log("USB_FS_OPEN OUT ep=0x02 ok max_pkt=%u\n",
               (unsigned)fs_out_open.max_packet_length);
        if (pid == PID_SWITCH && fs_out_open.max_packet_length != 64) {
            gp_log("USB_FS_OPEN OUT max_pkt=%u is not Nintendo controller OUT, rejecting %s\n",
                   (unsigned)fs_out_open.max_packet_length, dev_path);
            goto reinit;
        }
    } else {
        gp_log("USB_FS_OPEN OUT ep=0x02 failed errno=%d\n", errno);
    }

    /* Proven init sequence (from usb_handshake_probe v11):
     * [80 02]+[80 04] blindly, then respond to 0x81 0x01→[80 02] and
     * 0x81 0x02→[80 04]+subcmds(0x40,0x48,0x30,0x03 LAST). */
    uint8_t nintendo_seq = 1;
    int hs_state = HS_WAIT_81_01;

    if (pid == PID_SWITCH && out_opened) {
        int c0 = usb_fs_send_out_cmd(fd, &eps[1], 0x80, 0x02);
        usleep(30000);
        int c1 = usb_fs_send_out_cmd(fd, &eps[1], 0x80, 0x04);
        gp_log("Nintendo blind [80 02]=%d [80 04]=%d; handshake state machine starting\n", c0, c1);
        usleep(50000);
    } else if (pid == PID_SWITCH) {
        gp_log("Nintendo OUT not open — skipping handshake, trying native read\n");
        hs_state = HS_STREAMING;
    }

    uint32_t pkt_count = 0;
    int usb_ready_notified = 0;

    while (1) {
        memset(buf,0,sizeof(buf));
        lengths[0]=sizeof(buf); eps[0].aFrames=0; eps[0].status=0;

        memset(&start,0,sizeof(start)); start.ep_index=0;
        if (ioctl(fd,USB_FS_START,&start)!=0) {
            if (errno==EBUSY) {
                memset(&stop,0,sizeof(stop)); stop.ep_index=0;
                ioctl(fd,USB_FS_STOP,&stop); usleep(5000);
            } else if (errno==ENXIO||errno==ENOTTY) {
                gp_log("START errno=%d — device gone, reinit\n",errno);
                goto reinit;
            } else {
                gp_log("START fatal errno=%d\n",errno); goto reinit;
            }
            continue;
        }

        int complete_ok = 0;
        int complete_errno = 0;
        int complete_wait = 0;
        /* Poll COMPLETE for up to 300ms (timeout=200ms + margin) */
        for (complete_wait = 0; complete_wait < 10; complete_wait++) {
            memset(&complete,0,sizeof(complete)); complete.ep_index=0;
            if (ioctl(fd,USB_FS_COMPLETE,&complete)==0) {
                complete_ok = 1;
                break;
            }
            complete_errno = errno;
            if (complete_errno==ENXIO||complete_errno==ENOTTY) {
                gp_log("COMPLETE errno=%d — device gone, reinit\n",complete_errno);
                goto reinit;
            }
            if (complete_errno != EBUSY) break;
            usleep(50000);
        }
        if (!complete_ok) {
            static uint32_t compl_err=0; compl_err++;
            if (compl_err==1||compl_err%20==0)
                gp_log("COMPLETE failed errno=%d wait=%d count=%u\n",
                       complete_errno,complete_wait,compl_err);
            memset(&stop,0,sizeof(stop)); stop.ep_index=0;
            ioctl(fd,USB_FS_STOP,&stop);
            continue;
        }

        if (lengths[0] < 1) continue;
        uint8_t rid = buf[0];
        uint32_t len = lengths[0];

        if (pkt_count < 10 || (pkt_count % 300 == 0)) {
            gp_log("PKT #%u hs=%d id=0x%02x len=%u: "
                   "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   pkt_count, hs_state, rid, len,
                   buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],
                   buf[6],buf[7],buf[8],buf[9],buf[10],buf[11]);
        }
        pkt_count++;

        ScePadData pad;
        memset(&pad, 0, sizeof(pad));
        pad.quat.w = 1.0f;

        /* Data packets: rid=0x00 (8BitDo streaming format, same layout as 0x30)
         * or rid=0x30 (first transition packet after subcmd 0x03).
         * Require buf[1]!=0 (non-zero timer) — all-zeros is a USB buffer
         * artifact after reconnect, not real data. */
        if ((rid == 0x00 || rid == 0x30) && len >= 12) {
            if (rid == 0x00 && buf[1] == 0) {
                /* All-zeros garbage packet — skip, handshake not done yet */
                continue;
            }
            if (hs_state != HS_STREAMING) {
                gp_log("rid=0x%02x timer=0x%02x — already streaming, skip handshake\n",
                       rid, buf[1]);
                hs_state = HS_STREAMING;
            }
            if (!usb_ready_notified) {
                notify("Ghostcontrol: Controller connected and streaming");
                usb_ready_notified = 1;
            }
            parse_0x30(buf, &pad);
            if (g_vdi_ready) inject_pad(&pad);
            continue;
        }

        if (rid == 0x3f && len >= 9) {
            if (!usb_ready_notified) {
                notify("Ghostcontrol: Controller connected and streaming");
                usb_ready_notified = 1;
            }
            parse_0x3f(buf, &pad);
            if (g_vdi_ready) inject_pad(&pad);
            continue;
        }

        if (rid == 0x21 && len >= 12) {
            gp_log("0x21 ACK subcmd=0x%02x hs=%d\n", (buf[12]&0x7f), hs_state);
            /* ACK for subcmd 0x03 might carry first input data — try parsing */
            parse_0x30(buf, &pad);
            if (g_vdi_ready && hs_state==HS_STREAMING) inject_pad(&pad);
            continue;
        }

        if (rid == 0x81) {
            /* Nintendo handshake. State machine from probe v11:
             * 0x81 0x01 → [80 02] → wait for 0x81 0x02
             * 0x81 0x02 → [80 04] → send subcmds 0x40,0x48,0x30,0x03 → streaming
             * If 0x81 arrives while HS_STREAMING (reconnect): reset and redo handshake. */
            if (hs_state == HS_STREAMING) {
                gp_log("0x81 sub=0x%02x while streaming — reconnect, restarting handshake\n", buf[1]);
                hs_state = HS_WAIT_81_01;
                usb_ready_notified = 0;
            }
            if (buf[1] == 0x01 && hs_state == HS_WAIT_81_01) {
                int hr = out_opened ? usb_fs_send_out_cmd(fd,&eps[1],0x80,0x02) : -ENODEV;
                gp_log("0x81 sub=0x01 → [80 02] ret=%d\n",hr);
                hs_state = HS_WAIT_81_02;
            } else if (buf[1] == 0x02 && hs_state <= HS_WAIT_81_02) {
                /* Complete handshake and send full subcmd init sequence */
                int hr = out_opened ? usb_fs_send_out_cmd(fd,&eps[1],0x80,0x04) : -ENODEV;
                gp_log("0x81 sub=0x02 → [80 04] ret=%d; sending subcmds\n",hr);
                usleep(50000);
                if (out_opened) {
                    uint8_t d1[]={0x01}; nintendo_send_subcmd(fd,&eps[1],&nintendo_seq,0x40,d1,1); usleep(50000);
                    uint8_t d2[]={0x01}; nintendo_send_subcmd(fd,&eps[1],&nintendo_seq,0x48,d2,1); usleep(50000);
                    uint8_t d3[]={0x01}; nintendo_send_subcmd(fd,&eps[1],&nintendo_seq,0x30,d3,1); usleep(50000);
                    uint8_t d4[]={0x30}; nintendo_send_subcmd(fd,&eps[1],&nintendo_seq,0x03,d4,1);
                    gp_log("Subcmds sent (0x40,0x48,0x30,0x03) — expecting 0x00 stream\n");
                }
                hs_state = HS_STREAMING;
            } else {
                gp_log("0x81 sub=0x%02x hs=%d (ignored)\n",buf[1],hs_state);
            }
            continue;
        }

        /* Unknown packet — log and skip */
        if (pkt_count < 20) {
            gp_log("UNKNOWN rid=0x%02x len=%u hs=%d\n",rid,len,hs_state);
        }
    }

reinit:
    if (usb_ready_notified) {
        notify("Ghostcontrol: Controller disconnected");
        usb_ready_notified = 0;
    }
    memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
    if (out_opened) {
        memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=1; ioctl(fd,USB_FS_CLOSE,&fs_close);
        out_opened = 0;
    }
    memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=0; ioctl(fd,USB_FS_CLOSE,&fs_close);
uninit_retry:
    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd); gp_log("USB: reinit in 300ms\n"); usleep(300000);
    goto retry_find;
}

/* ------------------------------------------------------------------
 * Credential elevation
 * ------------------------------------------------------------------ */

static void elevate_credentials(void) {
    pid_t mypid = getpid();
    uint8_t caps[16] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
    };
    kernel_set_ucred_authid(mypid, 0x3800000000010003l);
    kernel_set_ucred_caps(mypid, caps);
}

/* ------------------------------------------------------------------
 * main — follows Ghostpad's VDA + klog + force_bind + direct VDI path
 * ------------------------------------------------------------------ */

int main(void) {
    int32_t userId = -1, fgUser = -1;
    int ret;

    ghostpad_status_log_reset();
    gp_log("Ghost-Control v3 starting\n");
    notify("Ghostcontrol - by StonedModder");

    /* Kill previous instance via pidfile */
    {
        int pfd = open(PID_PATH, O_RDONLY);
        if (pfd >= 0) {
            char pbuf[16] = {0};
            read(pfd, pbuf, sizeof(pbuf)-1);
            close(pfd);
            pid_t old = (pid_t)atoi(pbuf);
            if (old > 0 && old != getpid()) {
                gp_log("Killing previous instance pid=%d\n", old);
                kill(old, SIGTERM);
                usleep(600000);
            }
        }
        int pfd2 = open(PID_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0600);
        if (pfd2 >= 0) {
            char pbuf[16];
            snprintf(pbuf, sizeof(pbuf), "%d", getpid());
            write(pfd2, pbuf, strlen(pbuf));
            close(pfd2);
        }
    }

    /* User service */
    sceUserServiceInitialize(NULL);
    sceUserServiceGetInitialUser(&userId);
    sceUserServiceGetForegroundUser(&fgUser);
    gp_log("userId=0x%08x fgUser=0x%08x\n", (uint32_t)userId, (uint32_t)fgUser);

    /* g_inject_uid: actual foreground user — used for force_bind.
     * userId is clamped to [0x10000000,0x1000000F] for direct scePad calls. */
    g_inject_uid = (fgUser > 0) ? fgUser : userId;
    if ((uint32_t)userId < 0x10000000u || (uint32_t)userId > 0x1000000Fu)
        userId = 0x10000000;
    gp_log("inject_uid=0x%08x direct_uid=0x%08x\n",
           (uint32_t)g_inject_uid, (uint32_t)userId);

    elevate_credentials();

    ret = scePadInit();
    gp_log("scePadInit: 0x%08x\n", ret);
    ret = scePadSetProcessPrivilege(1);
    gp_log("scePadSetProcessPrivilege: 0x%08x\n", ret);

    /* Clean orphaned virtual devices from previous runs */
    for (int dh = 0; dh < 64; dh++) {
        int32_t dr = scePadVirtualDeviceDeleteDevice(dh);
        if (dr == 0) gp_log("deleteDevice(%d): ok\n", dh);
    }

    /* Start klog monitoring BEFORE VDA so we never miss the DEVICE_ADDED event */
    pthread_t klog_tid;
    if (pthread_create(&klog_tid, NULL, klog_capture_thread, NULL) == 0) {
        pthread_detach(klog_tid);
        gp_log("klog thread started\n");
    }
    usleep(300000); /* let klog thread connect before VDA fires the event */

    /* ── VDA: create virtual DualSense ──────────────────────────────
     * PS5 VDA uses userId=1 (not the actual user ID).
     * Returns 0x803b0006 on PS5 — device IS created despite error code.
     * If ret >= 0, it may encode a handle in the return value or param fields. */
    struct { int32_t size; int32_t userId; int32_t pad[6]; } vdp;
    const int32_t SENTINEL = (int32_t)0xDEADBEEF;
    memset(&vdp, 0, sizeof(vdp));
    vdp.size = (int32_t)sizeof(vdp);
    vdp.userId = 1; /* PS5 VDA always uses userId=1 */
    for (int k=0; k<6; k++) vdp.pad[k] = SENTINEL;

    ret = scePadVirtualDeviceAddDevice(&vdp, VIRTUAL_DEVICE_TYPE_DUALSENSE);
    gp_log("VDA ret=0x%08x pad[0-3]=0x%08x 0x%08x 0x%08x 0x%08x\n",
           (uint32_t)ret, (uint32_t)vdp.pad[0], (uint32_t)vdp.pad[1],
           (uint32_t)vdp.pad[2], (uint32_t)vdp.pad[3]);

    /* Check if VDA returned a direct handle (positive return value) */
    int32_t direct_handle = -1;
    if (ret > 0) {
        direct_handle = ret;
        gp_log("VDA direct handle from ret: %d\n", direct_handle);
    }
    for (int k=0; k<6; k++) {
        if (vdp.pad[k] != SENTINEL && vdp.pad[k] > 0) {
            gp_log("VDA handle in pad[%d]=%d\n", k, vdp.pad[k]);
            if (direct_handle < 0) direct_handle = vdp.pad[k];
            break;
        }
    }

    /* ── Wait for klog to confirm DEVICE_ADDED (up to 10s) ─────────── */
    gp_log("Waiting for VDA device ID (klog + GetHandle fallback)...\n");
    uint64_t vdev_id = 0;
    for (int t = 0; t < 100 && !vdev_id; t++) {
        usleep(100000);
        pthread_mutex_lock(&g_klog_lock);
        vdev_id = g_klog_vdev_id;
        pthread_mutex_unlock(&g_klog_lock);
    }

    int32_t vdi_handle = -1;

    if (vdev_id) {
        gp_log("VDA device from klog: 0x%llx\n", (unsigned long long)vdev_id);
        vdi_handle = (int32_t)(vdev_id & 0xffffffffu);
        /* force_bind triggers the PS5 user-assignment dialog.
         * Without it the PS5 silently auto-assigns with no screen shown.
         * The user then presses a button on the controller to confirm. */
        int br = shellui_pad_force_bind(vdev_id, g_inject_uid);
        gp_log("force_bind(0x%llx, 0x%08x) ret=%d — dialog should appear\n",
               (unsigned long long)vdev_id, (uint32_t)g_inject_uid, br);

    } else if (direct_handle >= 0) {
        gp_log("klog timeout — using direct VDA handle %d\n", direct_handle);
        vdi_handle = direct_handle;
    } else {
        /* Last resort: GetHandle scan for existing virtual DualSense */
        gp_log("klog timeout — GetHandle scan...\n");
        static const int32_t uids[] = {1, 0x10000000, (int32_t)0xffffffff};
        for (int ui=0; ui<3 && vdi_handle<0; ui++)
            for (int idx=0; idx<8 && vdi_handle<0; idx++) {
                vdi_handle = scePadGetHandle(uids[ui], 3, idx);
                if (vdi_handle >= 0)
                    gp_log("GetHandle uid=0x%08x idx=%d h=%d\n",
                           (uint32_t)uids[ui], idx, vdi_handle);
            }
    }

    if (vdi_handle >= 0) {
        g_vdi_handle = vdi_handle;
        g_vdi_ready  = 1;
        gp_log("VDI READY handle=0x%x\n", (uint32_t)vdi_handle);
        notify("Ghostcontrol: Ready — select your user with the controller");
    } else {
        gp_log("ERROR: no VDI handle found\n");
        notify("Ghostcontrol: ERROR — no VDI handle");
    }

    /* Start USB HID reader thread */
    pthread_t usb_tid;
    if (pthread_create(&usb_tid, NULL, usb_hid_thread, NULL) == 0) {
        pthread_detach(usb_tid);
        gp_log("USB thread created\n");
    }

    /* Keep-alive + periodic status */
    uint32_t tick = 0;
    while (1) {
        usleep(1000000);
        tick++;
        if (tick % 10 == 0) {
            gp_log("alive tick=%u vdi_ready=%d handle=0x%x\n",
                   tick, g_vdi_ready, (uint32_t)g_vdi_handle);
        }
    }
    return 0;
}

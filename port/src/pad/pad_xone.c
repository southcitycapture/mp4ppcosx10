/* Xbox One controllers over USB, without a kernel driver -- every one on the
 * bus, up to PORT_PAD_MAX, each with its own device handle and reader (M34).
 *
 * Ported near-verbatim from the Snowboard Kids ports'
 * (~/Apps/snowboardkids-decomp/port/src/platform/input_xone.c): these pads
 * are not HID devices, they speak Microsoft's GIP protocol on a
 * vendor-specific interface (class 0xFF, subclass 0x47, protocol 0xD0), so
 * neither the system nor SDL sees a joystick. Mac OS X lets a process talk to
 * an unclaimed USB interface through IOUSBLib, which is all this needs: open
 * the interface, send the power-on packets, and read the interrupt reports on
 * a thread. Report layout after the Linux xpad driver:
 *
 *   byte 0  0x20 = input report (0x07 = guide button, bit 0 of byte 4)
 *   byte 4  0x04 menu  0x08 view  0x10 A  0x20 B  0x40 X  0x80 Y
 *   byte 5  0x01 up  0x02 down  0x04 left  0x08 right  0x10 LB  0x20 RB  0x40 LS  0x80 RS
 *   6,8     left/right trigger, little-endian 0..1023
 *   10..17  left X, left Y, right X, right Y: little-endian s16, Y positive is up
 *
 * GameCube mapping (chosen here, documented in --help and pad.c's header):
 *   left stick  -> main stick        right stick -> C stick
 *   A B X Y     -> GC A B X Y        LT/RT       -> triggerL/triggerR, plus
 *                                                    the PAD_TRIGGER_L/R click
 *                                                    bits past a threshold
 *   LB          -> Z                 menu (Start) -> START
 *   dpad        -> the GC pad's own digital dpad bits
 *
 * Each pad's reader runs on an SDL thread (SDL is already linked for
 * GX/audio; this just reuses it for a portable thread+atomics primitive) and
 * only ever writes its pad's `state`, which is a plain struct read back with
 * no lock -- torn reads are, at worst, one frame of a slightly wrong analog
 * value, which is the same trade-off the Snowboard Kids ports made.
 */
#include "pad_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if !defined(PORT_NO_SDL) && defined(PAD_XONE_REAL)
#include <SDL.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>

struct pad_xone_state {
    uint8_t buttons; /* 0x04 menu 0x08 view 0x10 A 0x20 B 0x40 X 0x80 Y */
    uint8_t dpad;    /* 0x01 up 0x02 down 0x04 left 0x08 right 0x10 LB 0x20 RB 0x40 LS 0x80 RS */
    uint8_t guide;
    uint16_t lt, rt;         /* 0..1023 */
    int16_t lx, ly, rx, ry;  /* Y positive is up */
};

typedef struct XonePad {
    IOUSBDeviceInterface187** xdev;
    IOUSBInterfaceInterface190** xintf;
    UInt8 in_pipe, out_pipe;
    SDL_Thread* reader;
    volatile int present;
    volatile struct pad_xone_state state;
    uint8_t seq;
    int report_log;
    char name[64];
} XonePad;
static XonePad pads[PORT_PAD_MAX];
static int npads;

static const uint16_t products[] = { 0x02D1, 0x02DD, 0x02E3, 0x02EA, 0x02FD, 0x0B00, 0x0B0A, 0x0B12, 0x0B20, 0 };

static int16_t le16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }

static void send_packet(XonePad* x, const uint8_t* pkt, int len) {
    uint8_t buf[64];
    memcpy(buf, pkt, len);
    buf[2] = x->seq++;
    (*x->xintf)->WritePipe(x->xintf, x->out_pipe, buf, len);
}

static int reader_main(void* arg) {
    XonePad* x = (XonePad*)arg;
    uint8_t buf[64];
    for (;;) {
        UInt32 size = sizeof(buf);
        IOReturn r = (*x->xintf)->ReadPipe(x->xintf, x->in_pipe, buf, &size);
        if (r != kIOReturnSuccess) {
            port_log("port> pad: xone: %s: read failed (%08x), controller gone\n", x->name, (unsigned)r);
            x->present = 0;
            return 0;
        }
        if (x->report_log > 0 && size >= 4) {
            unsigned i;
            x->report_log--;
            port_log("port> pad: xone: %s: report", x->name);
            for (i = 0; i < size && i < 20; i++) {
                port_log(" %02x", buf[i]);
            }
            port_log("\n");
        }
        if (size >= 18 && buf[0] == 0x20) {
            struct pad_xone_state s;
            s.buttons = buf[4];
            s.dpad = buf[5];
            s.lt = (uint16_t)le16(buf + 6);
            s.rt = (uint16_t)le16(buf + 8);
            s.lx = le16(buf + 10);
            s.ly = le16(buf + 12);
            s.rx = le16(buf + 14);
            s.ry = le16(buf + 16);
            s.guide = x->state.guide;
            x->state = s;
        } else if (size >= 5 && buf[0] == 0x07) {
            x->state.guide = buf[4] & 1;
        }
    }
}

static int open_interface(XonePad* x, io_service_t svc) {
    IOCFPlugInInterface** plug = NULL;
    SInt32 score;
    UInt8 n, i, cls, sub, proto;
    IOReturn r = IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (r != kIOReturnSuccess || plug == NULL) {
        port_log("port> pad: xone: interface plug-in failed (%08x)\n", (unsigned)r);
        return 0;
    }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID190), (LPVOID*)&x->xintf);
    (*plug)->Release(plug);
    if (x->xintf == NULL) {
        return 0;
    }
    (*x->xintf)->GetInterfaceClass(x->xintf, &cls);
    (*x->xintf)->GetInterfaceSubClass(x->xintf, &sub);
    (*x->xintf)->GetInterfaceProtocol(x->xintf, &proto);
    if (port_pad_debug) {
        port_log("port> pad: xone: interface class %02x/%02x/%02x\n", cls, sub, proto);
    }
    if (cls != 0xFF || sub != 0x47 || proto != 0xD0) {
        (*x->xintf)->Release(x->xintf);
        x->xintf = NULL;
        return 0;
    }
    if ((*x->xintf)->USBInterfaceOpen(x->xintf) != kIOReturnSuccess) {
        port_log("port> pad: xone: cannot open the control interface (another driver has it?)\n");
        (*x->xintf)->Release(x->xintf);
        x->xintf = NULL;
        return 0;
    }
    (*x->xintf)->GetNumEndpoints(x->xintf, &n);
    x->in_pipe = x->out_pipe = 0;
    for (i = 1; i <= n; i++) {
        UInt8 dir, num, type, interval;
        UInt16 maxp;
        if ((*x->xintf)->GetPipeProperties(x->xintf, i, &dir, &num, &type, &maxp, &interval) != kIOReturnSuccess) {
            continue;
        }
        if (type == kUSBInterrupt && dir == kUSBIn && x->in_pipe == 0) {
            x->in_pipe = i;
        }
        if (type == kUSBInterrupt && dir == kUSBOut && x->out_pipe == 0) {
            x->out_pipe = i;
        }
    }
    if (x->in_pipe == 0 || x->out_pipe == 0) {
        port_log("port> pad: xone: interrupt pipes not found (in %u out %u)\n", x->in_pipe, x->out_pipe);
        (*x->xintf)->USBInterfaceClose(x->xintf);
        (*x->xintf)->Release(x->xintf);
        x->xintf = NULL;
        return 0;
    }
    return 1;
}

static int open_device(XonePad* x, io_service_t svc, uint16_t pid) {
    IOCFPlugInInterface** plug = NULL;
    SInt32 score;
    UInt8 cfg = 0;
    IOUSBFindInterfaceRequest req;
    io_iterator_t it;
    io_service_t isvc;
    int ok = 0;
    IOReturn r = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (r != kIOReturnSuccess || plug == NULL) {
        port_log("port> pad: xone: device plug-in failed (%08x)\n", (unsigned)r);
        return 0;
    }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID187), (LPVOID*)&x->xdev);
    (*plug)->Release(plug);
    if (x->xdev == NULL) {
        port_log("port> pad: xone: no device interface\n");
        return 0;
    }
    r = (*x->xdev)->USBDeviceOpen(x->xdev);
    if (r != kIOReturnSuccess) {
        r = (*x->xdev)->USBDeviceOpenSeize(x->xdev);
    }
    if (r != kIOReturnSuccess) {
        port_log("port> pad: xone: cannot open device 045e:%04x (%08x)\n", pid, (unsigned)r);
        (*x->xdev)->Release(x->xdev);
        x->xdev = NULL;
        return 0;
    }
    (*x->xdev)->GetConfiguration(x->xdev, &cfg);
    if (port_pad_debug) {
        port_log("port> pad: xone: device 045e:%04x open, configuration %u\n", pid, cfg);
    }
    if (cfg == 0) {
        IOUSBConfigurationDescriptorPtr d;
        if ((*x->xdev)->GetConfigurationDescriptorPtr(x->xdev, 0, &d) == kIOReturnSuccess) {
            (*x->xdev)->SetConfiguration(x->xdev, d->bConfigurationValue);
        }
    }
    req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    if ((*x->xdev)->CreateInterfaceIterator(x->xdev, &req, &it) == kIOReturnSuccess) {
        while (!ok && (isvc = IOIteratorNext(it)) != 0) {
            ok = open_interface(x, isvc);
            IOObjectRelease(isvc);
        }
        IOObjectRelease(it);
    }
    if (!ok) {
        (*x->xdev)->USBDeviceClose(x->xdev);
        (*x->xdev)->Release(x->xdev);
        x->xdev = NULL;
    }
    return ok;
}

int pad_xone_open(void) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    io_iterator_t iter = 0;
    io_service_t svc;
    if (match == NULL) {
        return 0;
    }
    /* kIOMasterPortDefault: the name the 10.4u SDK (the ppc-darwin target's own
     * SDK) has; the host build's much newer system SDK renamed it to
     * kIOMainPortDefault and deprecated the old one, hence the pragma. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, match, &iter) != kIOReturnSuccess) {
#pragma clang diagnostic pop
        port_log("port> pad: xone: IOServiceGetMatchingServices failed\n");
        return 0;
    }
    while (npads < PORT_PAD_MAX && (svc = IOIteratorNext(iter)) != 0) {
        CFNumberRef pn = IORegistryEntryCreateCFProperty(svc, CFSTR(kUSBProductID), NULL, 0);
        CFNumberRef vn = IORegistryEntryCreateCFProperty(svc, CFSTR(kUSBVendorID), NULL, 0);
        SInt32 pid = 0, vid = 0;
        int i;
        if (pn != NULL) {
            CFNumberGetValue(pn, kCFNumberSInt32Type, &pid);
            CFRelease(pn);
        }
        if (vn != NULL) {
            CFNumberGetValue(vn, kCFNumberSInt32Type, &vid);
            CFRelease(vn);
        }
        if (port_pad_debug) {
            port_log("port> pad: xone: USB device %04x:%04x\n", (unsigned)vid, (unsigned)pid);
        }
        if (vid != 0x045E) {
            IOObjectRelease(svc);
            continue;
        }
        for (i = 0; products[i] != 0; i++) {
            if (products[i] == pid) {
                break;
            }
        }
        if (products[i] != 0) {
            XonePad* x = &pads[npads];
            memset(x, 0, sizeof(*x));
            if (open_device(x, svc, (uint16_t)pid)) {
                static const uint8_t s_init[] = { 0x05, 0x20, 0x00, 0x0F, 0x06 }; /* Xbox One S: leave the "wake" state */
                static const uint8_t power_on[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
                x->present = 1;
                if (pid == 0x02EA || pid == 0x02FD || pid == 0x0B12 || pid == 0x0B20) {
                    send_packet(x, s_init, sizeof(s_init));
                }
                send_packet(x, power_on, sizeof(power_on));
                snprintf(x->name, sizeof(x->name), "Xbox One controller %d (045e:%04x)", npads + 1, (unsigned)pid);
                x->report_log = port_pad_debug ? 6 : 0;
                x->reader = SDL_CreateThread(reader_main, "mp4-xone", x);
                port_log("port> pad: %s via IOUSBLib (pipes in %u out %u)\n", x->name, x->in_pipe, x->out_pipe);
                npads++;
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    return npads;
}

int pad_xone_count(void) { return npads; }
int pad_xone_present(int i) { return i >= 0 && i < npads && pads[i].present; }
const char* pad_xone_name(int i) { return (i >= 0 && i < npads) ? pads[i].name : "xone (none)"; }

static u8 scale_trigger(uint16_t v) {
    /* 0..1023 -> 0..255 */
    unsigned s = (unsigned)v >> 2;
    return (u8)(s > 255 ? 255 : s);
}

static s8 scale_stick(int16_t v) {
    /* -32768..32767 -> roughly the GC pot's own -100..100 */
    long s = ((long)v * 100) / 32767;
    if (s > 100) {
        s = 100;
    }
    if (s < -100) {
        s = -100;
    }
    return (s8)s;
}

void pad_xone_poll(int i, PortPadRaw* out) {
    struct pad_xone_state s;
    u16 b = 0;

    memset(out, 0, sizeof(*out));
    if (i < 0 || i >= npads) {
        return;
    }
    s = pads[i].state; /* struct copy: a torn read costs one frame, not a crash */

    out->stickX = scale_stick(s.lx);
    out->stickY = scale_stick(s.ly);
    out->substickX = scale_stick(s.rx);
    out->substickY = scale_stick(s.ry);
    out->triggerL = scale_trigger(s.lt);
    out->triggerR = scale_trigger(s.rt);

    if (s.buttons & 0x10) {
        b |= PAD_BUTTON_A;
    }
    if (s.buttons & 0x20) {
        b |= PAD_BUTTON_B;
    }
    if (s.buttons & 0x40) {
        b |= PAD_BUTTON_X;
    }
    if (s.buttons & 0x80) {
        b |= PAD_BUTTON_Y;
    }
    if (s.buttons & 0x04) {
        b |= PAD_BUTTON_START;
    }
    if (s.dpad & 0x01) {
        b |= PAD_BUTTON_UP;
    }
    if (s.dpad & 0x02) {
        b |= PAD_BUTTON_DOWN;
    }
    if (s.dpad & 0x04) {
        b |= PAD_BUTTON_LEFT;
    }
    if (s.dpad & 0x08) {
        b |= PAD_BUTTON_RIGHT;
    }
    if (s.dpad & 0x10) { /* LB */
        b |= PAD_TRIGGER_Z;
    }
    if (out->triggerL >= 200) {
        b |= PAD_TRIGGER_L;
    }
    if (out->triggerR >= 200) {
        b |= PAD_TRIGGER_R;
    }
    out->button = b;
}

/* GIP rumble: 0x09 report, motor mask 0x0F (both triggers and both motors),
 * strengths 0..100, on/off periods in 10 ms units, repeat count. */
void pad_xone_rumble(int i, int on) {
    uint8_t pkt[13] = { 0x09, 0x00, 0x00, 0x09, 0x00, 0x0F, 0x00, 0x00, 0, 0, 0xFF, 0x00, 0xFF };
    if (i < 0 || i >= npads || !pads[i].present || pads[i].xintf == NULL) {
        return;
    }
    if (on) {
        pkt[8] = 70;
        pkt[9] = 70;
    } else {
        pkt[8] = pkt[9] = 0;
        pkt[10] = pkt[12] = 0;
    }
    send_packet(&pads[i], pkt, sizeof(pkt));
}

void pad_xone_close(void) {
    int i;
    for (i = 0; i < npads; i++) {
        XonePad* x = &pads[i];
        if (x->xintf != NULL) {
            (*x->xintf)->USBInterfaceClose(x->xintf); /* aborts the reader's ReadPipe */
            (*x->xintf)->Release(x->xintf);
            x->xintf = NULL;
        }
        if (x->xdev != NULL) {
            (*x->xdev)->USBDeviceClose(x->xdev);
            (*x->xdev)->Release(x->xdev);
            x->xdev = NULL;
        }
        x->present = 0;
    }
    npads = 0;
}

#else /* PORT_NO_SDL || !PAD_XONE_REAL: no IOKit driver in this build */

int pad_xone_open(void) { return 0; }
int pad_xone_count(void) { return 0; }
int pad_xone_present(int i) { (void)i; return 0; }
const char* pad_xone_name(int i) { (void)i; return "xone (disabled)"; }
void pad_xone_poll(int i, PortPadRaw* out) { (void)i; memset(out, 0, sizeof(*out)); }
void pad_xone_rumble(int i, int on) { (void)i; (void)on; }
void pad_xone_close(void) {}

#endif

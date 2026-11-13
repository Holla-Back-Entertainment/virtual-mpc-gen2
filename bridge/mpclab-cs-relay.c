// mpclab-cs-relay: the USB cable of the virtual Gen 2 control surface (lab only).
//
// The lab poses as an ACVC (MPC XL) or ACVG (MPC Live III) surface with a configfs f_midi gadget
// (09e8:005a / 09e8:0058, 2 MIDI ports each way) on the second dummy UDC; dummy_hcd loops it back, so snd-usb-audio shows the
// app the USB card it looks for. This relay is the surface's firmware end: it holds the
// gadget-side rawmidi (card id argv[1], default ACVCSurface; both subdevices, both directions) and moves the
// bytes to and from the host over the virtio-serial port "mpclab.cs", where the protocol brain
// (panel/surface.py) answers the handshake and injects buttons and pads.
//
// Link framing, both ways: [port 0|1] [length 1..255] [length raw MIDI bytes].
// The relay never parses MIDI; it only frames whatever each subdevice delivers.
// Built against the device's alsa-lib (libasound.so.2), like the audio bridge.
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *CARD = "ACVCSurface";   // argv[1]: ACVGSurface for the Live III
#define LINK "/dev/virtio-ports/mpclab.cs"
#define PORTS 2

static snd_rawmidi_t *in[PORTS], *out[PORTS];

static int open_ports(void) {
    char name[64];
    for (int p = 0; p < PORTS; p++) {
        snprintf(name, sizeof name, "hw:%s,0,%d", CARD, p);
        int err = snd_rawmidi_open(&in[p], &out[p], name, SND_RAWMIDI_NONBLOCK);
        if (err < 0)
            return err;
    }
    return 0;
}

// write all of buf to a non-blocking fd; drop what the host cannot take within ~100 ms
static void link_write(int fd, const unsigned char *buf, size_t n) {
    for (int tries = 0; n && tries < 100; ) {
        ssize_t w = write(fd, buf, n);
        if (w > 0) { buf += w; n -= w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) { perror("link write"); return; }
        usleep(1000); tries++;
    }
}

int main(int argc, char **argv) {
    if (argc > 1)
        CARD = argv[1];
    // the gadget-side card appears a moment after the gadget is bound to its UDC
    int err = -1;
    for (int i = 0; i < 1000 && (err = open_ports()) < 0; i++)
        usleep(10000);
    if (err < 0) {
        fprintf(stderr, "relay: cannot open %s: %s\n", CARD, snd_strerror(err));
        return 1;
    }
    int link = open(LINK, O_RDWR | O_NONBLOCK);
    if (link < 0) { perror(LINK); return 1; }
    fprintf(stderr, "relay: %s <-> %s\n", CARD, LINK);

    struct pollfd pfd[1 + PORTS * 4];
    int nf[PORTS];
    unsigned char rx[4096], frame[2 + 255];
    size_t have = 0;                       // bytes of a partial host frame in rx
    for (;;) {
        int n = 0;
        pfd[n++] = (struct pollfd){ .fd = link, .events = POLLIN };
        for (int p = 0; p < PORTS; p++) {
            nf[p] = snd_rawmidi_poll_descriptors(in[p], pfd + n, 4);
            n += nf[p];
        }
        if (poll(pfd, n, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll"); return 1;
        }
        // app -> surface: frame what each gadget input subdevice received
        for (int p = 0; p < PORTS; p++) {
            ssize_t r;
            while ((r = snd_rawmidi_read(in[p], frame + 2, 255)) > 0) {
                frame[0] = p; frame[1] = r;
                link_write(link, frame, r + 2);
            }
        }
        // surface -> app
        if (pfd[0].revents & POLLIN) {
            ssize_t r = read(link, rx + have, sizeof rx - have);
            if (r > 0) have += r;
            size_t off = 0;
            while (have - off >= 2 && have - off >= (size_t)rx[off + 1] + 2) {
                unsigned port = rx[off], len = rx[off + 1];
                if (port < PORTS && len)
                    snd_rawmidi_write(out[port], rx + off + 2, len);   // non-blocking; 4 KB buffer
                off += 2 + len;
            }
            memmove(rx, rx + off, have - off);
            have -= off;
        } else if (pfd[0].revents & (POLLHUP | POLLERR)) {
            usleep(100000);                // host side closed: wait for it to come back
        }
    }
}

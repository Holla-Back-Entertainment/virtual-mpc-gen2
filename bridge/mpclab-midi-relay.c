// mpclab-midi-relay: the MPC XL's rear MIDI ports, MIDI In 1-2 and MIDI Out A-D (lab only).
//
// The stock XL has no serial MIDI: its DIN ports are USB MIDI behind the internal hub. The lab
// plays that part with a configfs f_midi gadget (mpclab-midi-gadget: 2 ports to the app, 4 from
// it) on the third dummy UDC; dummy_hcd loops it back, so snd-usb-audio gives the app an
// ordinary USB MIDI card. This relay is the far end of those cables: it holds the gadget-side
// rawmidi (card id argv[1], default XLRearMIDI) and gives every port its own virtio-serial port,
// raw MIDI bytes, no framing:
//   /dev/virtio-ports/mpclab.midi_in1, _in2     host -> gadget output 0, 1 -> app input  In 1, In 2
//   /dev/virtio-ports/mpclab.midi_outa .. _outd gadget input 0..3 -> host   app output Out A..D
// QEMU joins each virtio-serial port to its own TCP port (lab.py PORTS midi_*).
// Built against the device's alsa-lib (libasound.so.2), like the audio bridge.
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#define NIN 2                              // MIDI In 1-2: host -> app
#define NOUT 4                             // MIDI Out A-D: app -> host
static const char *IN_LINK[NIN] = { "mpclab.midi_in1", "mpclab.midi_in2" };
static const char *OUT_LINK[NOUT] = { "mpclab.midi_outa", "mpclab.midi_outb", "mpclab.midi_outc", "mpclab.midi_outd" };

static snd_rawmidi_t *tx[NIN], *rx[NOUT];  // gadget output (to the app), gadget input (from the app)
static int in_fd[NIN], out_fd[NOUT];

static int open_link(const char *name) {
    char path[64];
    snprintf(path, sizeof path, "/dev/virtio-ports/%s", name);
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        perror(path);
    return fd;
}

int main(int argc, char **argv) {
    const char *card = argc > 1 ? argv[1] : "XLRearMIDI";
    char name[64];
    // the gadget-side card appears a moment after the gadget is bound to its UDC
    int err = -1;
    for (int i = 0; i < 1000; i++) {
        int p;
        for (p = 0; p < NIN; p++) {
            snprintf(name, sizeof name, "hw:%s,0,%d", card, p);
            if ((err = snd_rawmidi_open(NULL, &tx[p], name, SND_RAWMIDI_NONBLOCK)) < 0)
                break;
        }
        for (int q = 0; err >= 0 && q < NOUT; q++) {
            snprintf(name, sizeof name, "hw:%s,0,%d", card, q);
            err = snd_rawmidi_open(&rx[q], NULL, name, SND_RAWMIDI_NONBLOCK);
        }
        if (err >= 0)
            break;
        for (int k = 0; k < NIN; k++) if (tx[k]) { snd_rawmidi_close(tx[k]); tx[k] = NULL; }
        for (int k = 0; k < NOUT; k++) if (rx[k]) { snd_rawmidi_close(rx[k]); rx[k] = NULL; }
        usleep(10000);
    }
    if (err < 0) {
        fprintf(stderr, "midi relay: cannot open %s: %s\n", card, snd_strerror(err));
        return 1;
    }
    for (int p = 0; p < NIN; p++)
        if ((in_fd[p] = open_link(IN_LINK[p])) < 0) return 1;
    for (int q = 0; q < NOUT; q++)
        if ((out_fd[q] = open_link(OUT_LINK[q])) < 0) return 1;
    fprintf(stderr, "midi relay: %s <-> mpclab.midi_in1,2 / midi_outa..d\n", card);

    struct pollfd pfd[NIN + NOUT * 4];
    int hup[NIN] = { 0 };
    unsigned char buf[1024];
    for (;;) {
        int n = 0;
        for (int p = 0; p < NIN; p++)        // a closed host side reads as POLLHUP: back off
            pfd[n++] = (struct pollfd){ .fd = in_fd[p], .events = hup[p] ? 0 : POLLIN };
        for (int q = 0; q < NOUT; q++)
            n += snd_rawmidi_poll_descriptors(rx[q], pfd + n, 4);
        if (poll(pfd, n, 200) < 0) {
            if (errno == EINTR) continue;
            perror("poll"); return 1;
        }
        // host -> app (MIDI In 1-2)
        for (int p = 0; p < NIN; p++) {
            if (hup[p]) { hup[p]--; continue; }
            if (pfd[p].revents & POLLIN) {
                ssize_t r = read(in_fd[p], buf, sizeof buf);
                if (r > 0)
                    snd_rawmidi_write(tx[p], buf, r);   // non-blocking; 4 KB buffer
            } else if (pfd[p].revents & (POLLHUP | POLLERR)) {
                hup[p] = 5;                              // ~1 s at the 200 ms poll timeout
            }
        }
        // app -> host (MIDI Out A-D). Nobody listening on the host: QEMU drops it or the port
        // stays full; either way drop rather than hold up the other ports.
        for (int q = 0; q < NOUT; q++) {
            ssize_t r;
            while ((r = snd_rawmidi_read(rx[q], buf, sizeof buf)) > 0) {
                unsigned char *b = buf;
                for (int tries = 0; r > 0 && tries < 5; ) {
                    ssize_t w = write(out_fd[q], b, r);
                    if (w > 0) { b += w; r -= w; continue; }
                    if (w < 0 && errno != EAGAIN && errno != EINTR) break;
                    usleep(1000); tries++;
                }
            }
        }
    }
}

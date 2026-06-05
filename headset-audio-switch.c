/*
 * headset-audio-switch — auto-route audio when SteelSeries Arctis 7 powers on/off.
 *
 * Build:  cc -O2 -s -o headset-audio-switch hs-audio-switch.c -lpulse
 * Deps:   libc, libpulse (already on any PulseAudio/PipeWire system)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <pulse/pulseaudio.h>

#define POLL_INTERVAL 1
#define UNUSED(x)     ((void)(x))

/* ── Globals ─────────────────────────────────────────────────────────── */

static volatile bool running = true;
static pa_mainloop  *pa_loop = NULL;
static pa_context   *pa_ctx  = NULL;
static char *headset_sink = NULL;
static char *fallback_sink = NULL;
static bool  script_switched = false;

/* ── Signal handler ──────────────────────────────────────────────────── */

static void on_signal(int sig) {
    UNUSED(sig);
    running = false;
}

/* ── Logging ─────────────────────────────────────────────────────────── */

static void log_msg(const char *fmt, ...) {
    char buf[256];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int off = strftime(buf, sizeof(buf), "[%H:%M:%S] ", &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    puts(buf);
    fflush(stdout);
}

/* ── PulseAudio helpers ──────────────────────────────────────────────── */

/* Pump PA mainloop (non-blocking), spin until ctx is ready */
static bool pa_wait_ready(void) {
    for (int i = 0; i < 200; i++) {
        pa_mainloop_iterate(pa_loop, 0, NULL);
        pa_context_state_t s = pa_context_get_state(pa_ctx);
        if (s == PA_CONTEXT_READY)  return true;
        if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) return false;
        usleep(5000); /* 5ms */
    }
    return false;
}

static void _pa_state_cb(pa_context *c, void *userdata) {
    UNUSED(c); UNUSED(userdata);
}

/* ── PA operations (synchronous via manual mainloop pumping) ─────────── */

struct pa_cb_data {
    bool done;
    char *sink_name;
    char **sinks;
    int   sink_count;
};

static void _pa_server_info_cb(pa_context *c, const pa_server_info *i, void *ud) {
    UNUSED(c); struct pa_cb_data *d = ud;
    if (i && i->default_sink_name) {
        free(d->sink_name);
        d->sink_name = strdup(i->default_sink_name);
    }
    d->done = true;
}

static void _pa_sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud) {
    UNUSED(c); struct pa_cb_data *d = ud;
    if (eol) { d->done = true; return; }
    d->sink_count++;
    d->sinks = realloc(d->sinks, sizeof(char *) * d->sink_count);
    d->sinks[d->sink_count - 1] = strdup(i->name);
}

static void _pa_simple_cb(pa_context *c, int success, void *ud) {
    UNUSED(c); UNUSED(success); struct pa_cb_data *d = ud;
    d->done = true;
}

static char *pa_get_default_sink(void) {
    struct pa_cb_data d = {0};
    pa_operation *op = pa_context_get_server_info(pa_ctx, _pa_server_info_cb, &d);
    if (!op) return NULL;
    for (int i = 0; i < 200 && !d.done; i++) {
        pa_mainloop_iterate(pa_loop, 0, NULL);
        usleep(5000);
    }
    pa_operation_unref(op);
    return d.sink_name;
}

static bool pa_set_default_sink(const char *name) {
    struct pa_cb_data d = {0};
    pa_operation *op = pa_context_set_default_sink(pa_ctx, name, _pa_simple_cb, &d);
    if (!op) return false;
    for (int i = 0; i < 200 && !d.done; i++) {
        pa_mainloop_iterate(pa_loop, 0, NULL);
        usleep(5000);
    }
    pa_operation_unref(op);
    return d.done;
}

static bool pa_is_headset(const char *name) {
    return name && (strcasestr(name, "steelseries") || strcasestr(name, "arctis"));
}

static bool pa_is_hdmi(const char *name) {
    return name && strcasestr(name, "hdmi");
}

static bool pa_has_game(const char *name) {
    return name && strcasestr(name, "game");
}

static char *pa_find_headset_game_sink(void) {
    struct pa_cb_data d = {0};
    pa_operation *op = pa_context_get_sink_info_list(pa_ctx, _pa_sink_info_cb, &d);
    if (!op) return NULL;
    for (int i = 0; i < 200 && !d.done; i++) {
        pa_mainloop_iterate(pa_loop, 0, NULL);
        usleep(5000);
    }
    pa_operation_unref(op);

    /* prefer game sink */
    for (int i = 0; i < d.sink_count; i++) {
        if (pa_is_headset(d.sinks[i]) && pa_has_game(d.sinks[i])) {
            char *s = d.sinks[i];
            for (int j = 0; j < d.sink_count; j++)
                if (j != i) free(d.sinks[j]);
            free(d.sinks);
            return s;
        }
    }
    for (int i = 0; i < d.sink_count; i++) {
        if (pa_is_headset(d.sinks[i])) {
            char *s = d.sinks[i];
            for (int j = 0; j < d.sink_count; j++)
                if (j != i) free(d.sinks[j]);
            free(d.sinks);
            return s;
        }
    }
    for (int i = 0; i < d.sink_count; i++) free(d.sinks[i]);
    free(d.sinks);
    return NULL;
}

static char *pa_find_fallback_sink(void) {
    struct pa_cb_data d = {0};
    pa_operation *op = pa_context_get_sink_info_list(pa_ctx, _pa_sink_info_cb, &d);
    if (!op) return NULL;
    for (int i = 0; i < 200 && !d.done; i++) {
        pa_mainloop_iterate(pa_loop, 0, NULL);
        usleep(5000);
    }
    pa_operation_unref(op);

    for (int i = 0; i < d.sink_count; i++) {
        if (!pa_is_headset(d.sinks[i]) && !pa_is_hdmi(d.sinks[i])) {
            char *s = d.sinks[i];
            for (int j = 0; j < d.sink_count; j++)
                if (j != i) free(d.sinks[j]);
            free(d.sinks);
            return s;
        }
    }
    for (int i = 0; i < d.sink_count; i++) free(d.sinks[i]);
    free(d.sinks);
    return NULL;
}

/* ── PA connect / disconnect ─────────────────────────────────────────── */

static bool pa_connect(void) {
    pa_loop = pa_mainloop_new();
    if (!pa_loop) return false;
    pa_ctx = pa_context_new(pa_mainloop_get_api(pa_loop), "headset-audio-switch");
    if (!pa_ctx) { pa_mainloop_free(pa_loop); pa_loop = NULL; return false; }
    pa_context_set_state_callback(pa_ctx, _pa_state_cb, NULL);

    if (pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOFAIL, NULL) < 0) {
        pa_context_unref(pa_ctx);
        pa_mainloop_free(pa_loop);
        pa_ctx = NULL; pa_loop = NULL;
        return false;
    }
    if (!pa_wait_ready()) {
        pa_context_disconnect(pa_ctx);
        pa_context_unref(pa_ctx);
        pa_mainloop_free(pa_loop);
        pa_ctx = NULL; pa_loop = NULL;
        return false;
    }
    return true;
}

static void pa_disconnect(void) {
    if (!pa_ctx) return;
    pa_context_disconnect(pa_ctx);
    pa_context_unref(pa_ctx);
    pa_mainloop_free(pa_loop);
    pa_ctx = NULL; pa_loop = NULL;
}

/* ── HID: Arctis 7 battery query ─────────────────────────────────────── */

static char *find_hidraw(void) {
    static char path[272];
    DIR *d = opendir("/sys/class/hidraw");
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char ue[320];
        snprintf(ue, sizeof(ue), "/sys/class/hidraw/%s/device/uevent", e->d_name);
        int ufd = open(ue, O_RDONLY);
        if (ufd < 0) continue;
        char buf[512];
        ssize_t n = read(ufd, buf, sizeof(buf) - 1);
        close(ufd);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strstr(buf, "1038") && (strstr(buf, "12AD") || strstr(buf, "12ad") || strstr(buf, "1260"))) {
            snprintf(path, sizeof(path), "/dev/%s", e->d_name);
            closedir(d);
            return path;
        }
    }
    closedir(d);
    return NULL;
}

static int query_battery(int fd) {
    unsigned char pkt[32] = {0};
    pkt[1] = 0x06; pkt[2] = 0x18;
    write(fd, pkt, 32);
    fd_set fds;
    FD_ZERO(&fds); FD_SET(fd, &fds);
    struct timeval tv = {0, 200000};
    if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) return -1;
    unsigned char resp[32];
    if (read(fd, resp, sizeof(resp)) >= 3) return resp[2];
    return -1;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int interval = POLL_INTERVAL;
    if (argc >= 3 && strcmp(argv[1], "--interval") == 0)
        interval = atoi(argv[2]);

    /* HID */
    char *hidraw = find_hidraw();
    if (!hidraw) { fprintf(stderr, "ERROR: dongle not found\n"); return 1; }
    int hfd = open(hidraw, O_RDWR);
    if (hfd < 0) { perror("open hidraw"); return 1; }

    /* PulseAudio */
    if (!pa_connect()) { fprintf(stderr, "ERROR: PA connect failed\n"); close(hfd); return 1; }

    headset_sink = pa_find_headset_game_sink();
    if (!headset_sink) { log_msg("ERROR: headset sink not found"); pa_disconnect(); close(hfd); return 1; }
    log_msg("Headset: %s", headset_sink);

    /* Initial state */
    int battery = query_battery(hfd);
    bool headset_on = (battery > 0);
    char *current = pa_get_default_sink();

    if (current && pa_is_headset(current)) {
        fallback_sink = pa_find_fallback_sink();
        log_msg("Currently on headset. Fallback: %s", fallback_sink ? fallback_sink : "(none)");
    } else if (current && pa_is_hdmi(current)) {
        fallback_sink = pa_find_fallback_sink();
        log_msg("Currently on HDMI (ignored). Fallback: %s", fallback_sink ? fallback_sink : "(none)");
    } else if (current) {
        fallback_sink = strdup(current);
        log_msg("Fallback: %s", fallback_sink);
    }

    /* Apply initial rules */
    if (headset_on && current && !pa_is_headset(current)) {
        log_msg("Headset ON (%d%%) -> switching from %s", battery,
                strrchr(current, '.') ? strrchr(current, '.') + 1 : current);
        pa_set_default_sink(headset_sink);
        free(current);
        current = strdup(headset_sink);
        script_switched = true;
    } else if (!headset_on && current && pa_is_headset(current) && fallback_sink) {
        log_msg("Headset OFF -> switching to fallback");
        pa_set_default_sink(fallback_sink);
        free(current);
        current = strdup(fallback_sink);
        script_switched = true;
    }

    int prev_battery = battery;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    log_msg("Watching...");

    while (running) {
        sleep(interval);

        char *new_current = pa_get_default_sink();
        if (new_current) {
            bool new_headset = pa_is_headset(new_current);
            bool new_hdmi    = pa_is_hdmi(new_current);
            bool changed     = !current || strcmp(current, new_current) != 0;

            if (changed && !script_switched) {
                if (new_headset)
                    log_msg("User switched to headset");
                else if (new_hdmi)
                    log_msg("User switched to HDMI (ignored for fallback)");
                else {
                    log_msg("User switched to %s -> new fallback",
                            strrchr(new_current, '.') ? strrchr(new_current, '.') + 1 : new_current);
                    free(fallback_sink);
                    fallback_sink = strdup(new_current);
                }
            }
            free(current);
            current = new_current;
        }
        script_switched = false;

        battery = query_battery(hfd);
        headset_on = (battery > 0);

        if (battery != prev_battery) {
            if (battery > 0) log_msg("Battery: %d%%", battery);
            else log_msg("Headset OFF");
        }

        if (headset_on && current && !pa_is_headset(current)) {
            log_msg("Headset ON -> switching to headset");
            pa_set_default_sink(headset_sink);
            free(current);
            current = strdup(headset_sink);
            script_switched = true;
        } else if (!headset_on && current && pa_is_headset(current)) {
            if (fallback_sink) {
                log_msg("Headset OFF -> switching to fallback");
                pa_set_default_sink(fallback_sink);
                free(current);
                current = strdup(fallback_sink);
                script_switched = true;
            } else {
                log_msg("Headset OFF, no fallback - staying on headset");
            }
        }
        prev_battery = battery;
    }

    free(current);
    pa_disconnect();
    close(hfd);
    free(headset_sink);
    free(fallback_sink);
    return 0;
}

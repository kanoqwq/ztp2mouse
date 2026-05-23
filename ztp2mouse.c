#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_UINPUT_DEV "/dev/uinput"
#define DEFAULT_DEVICE_NAME "ztp_virtual_mouse"
#define DEFAULT_SENSITIVITY 1.35
#define DEFAULT_SCROLL_SCALE 40.0
#define DEFAULT_TAP_TIMEOUT_MS 180
#define DEFAULT_TAP_MOVE_LIMIT 14
#define DEFAULT_JUMP_LIMIT 200
#define DEFAULT_DOUBLE_TAP_TIMEOUT_MS 300
#define DEFAULT_DRAG_START_THRESHOLD 18
#define RECONNECT_DELAY_MS 1000
#define MAX_MT_SLOTS 4

struct options {
    const char *src_dev;
    const char *uinput_dev;
    const char *device_name;
    double sensitivity;
    double scroll_scale;
    int tap_timeout_ms;
    int tap_move_limit;
    int jump_limit;
    int double_tap_timeout_ms;
    int drag_start_threshold;
    int grab_device;
    int daemon_mode;
    int verbose;
};

struct gesture_state {
    int x;
    int y;
    int abs_x;
    int abs_y;
    int abs_x_valid;
    int abs_y_valid;
    int mt_x[MAX_MT_SLOTS];
    int mt_y[MAX_MT_SLOTS];
    int mt_x_valid[MAX_MT_SLOTS];
    int mt_y_valid[MAX_MT_SLOTS];
    int last_x;
    int last_y;
    int x_valid;
    int y_valid;
    int motion_dirty;
    int last_valid;
    int finger_down;
    int finger_count;
    int max_finger_count;
    int current_slot;
    int slot_active[MAX_MT_SLOTS];
    int touch_started;
    int touch_moved;
    int touch_origin_x;
    int touch_origin_y;
    int touch_origin_valid;
    int suppress_tap_until_release;
    int source_button_down;
    int active_button_code;
    struct timespec down_ts;
    struct timespec last_tap_ts;
    double wheel_remainder;
    int last_tap_x;
    int last_tap_y;
    int last_tap_valid;
    int drag_active;
    int drag_pending_begin;
    int drag_arming;
    int drag_origin_x;
    int drag_origin_y;
};

static int g_verbose = 0;

static void log_msg(const char *level, const char *fmt, ...) {
    va_list ap;

    fprintf(stderr, "[%s] ", level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void log_errno(const char *what) {
    log_msg("ERR", "%s: %s", what, strerror(errno));
}

static long elapsed_ms(const struct timespec *start, const struct timespec *end) {
    long sec = (long)(end->tv_sec - start->tv_sec) * 1000L;
    long nsec = (long)(end->tv_nsec - start->tv_nsec) / 1000000L;
    return sec + nsec;
}

static void sleep_ms(unsigned int ms) {
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static int round_away_from_zero(double value) {
    if (value > 0.0) {
        return (int)(value + 0.5);
    }
    if (value < 0.0) {
        return (int)(value - 0.5);
    }
    return 0;
}

static int trim_deadzone_delta(int delta, int threshold) {
    if (delta > threshold) {
        return delta - threshold;
    }
    if (delta < -threshold) {
        return delta + threshold;
    }
    return 0;
}

static int emit_event(int fd, int type, int code, int value) {
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        return -1;
    }
    return 0;
}

static int emit_sync(int fd) {
    return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int emit_click(int fd, int code, int value) {
    if (emit_event(fd, EV_KEY, code, value) < 0) {
        return -1;
    }
    return emit_sync(fd);
}

static int pick_button_code(int fingers) {
    if (fingers >= 3) {
        return BTN_MIDDLE;
    }
    if (fingers == 2) {
        return BTN_RIGHT;
    }
    return BTN_LEFT;
}

static void clear_motion_state(struct gesture_state *st) {
    st->last_valid = 0;
    st->motion_dirty = 0;
    st->wheel_remainder = 0.0;
}

static void clear_pending_tap(struct gesture_state *st) {
    st->last_tap_valid = 0;
}

static int emit_button_click(int mouse_fd, int button_code) {
    if (emit_click(mouse_fd, button_code, 1) < 0 || emit_click(mouse_fd, button_code, 0) < 0) {
        log_errno("emit tap button");
        return -1;
    }
    return 0;
}

static int begin_drag(struct gesture_state *st, int mouse_fd) {
    st->active_button_code = BTN_LEFT;
    st->source_button_down = 1;
    st->drag_pending_begin = 0;
    st->drag_arming = 1;
    st->drag_origin_x = st->x;
    st->drag_origin_y = st->y;
    clear_motion_state(st);
    clear_pending_tap(st);
    if (emit_click(mouse_fd, st->active_button_code, 1) < 0) {
        return -1;
    }
    if (g_verbose) {
        log_msg("DBG", "drag begin at x=%d y=%d", st->x, st->y);
    }
    return 0;
}

static int end_drag(struct gesture_state *st, int mouse_fd) {
    if (emit_click(mouse_fd, BTN_LEFT, 0) < 0) {
        return -1;
    }
    st->source_button_down = 0;
    st->active_button_code = 0;
    st->drag_pending_begin = 0;
    st->drag_arming = 0;
    clear_pending_tap(st);
    clear_motion_state(st);
    st->touch_started = 0;
    st->touch_moved = 0;
    st->touch_origin_valid = 0;
    st->suppress_tap_until_release = 1;
    st->max_finger_count = 0;
    st->drag_origin_x = 0;
    st->drag_origin_y = 0;
    if (g_verbose) {
        log_msg("DBG", "drag end");
    }
    return 0;
}

static int create_mouse(const struct options *opt) {
    struct uinput_setup usetup;
    int fd = open(opt->uinput_dev, O_WRONLY | O_NONBLOCK);

    if (fd < 0) {
        log_errno("open /dev/uinput");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_Y) < 0 ||
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL) < 0) {
        log_errno("configure uinput device");
        close(fd);
        return -1;
    }

    memset(&usetup, 0, sizeof(usetup));
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "%s", opt->device_name);
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x19d2;
    usetup.id.product = 0x2024;
    usetup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        log_errno("UI_DEV_SETUP");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        log_errno("UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    sleep_ms(300);
    log_msg("INF", "created virtual mouse '%s'", opt->device_name);
    return fd;
}

static void destroy_mouse(int fd) {
    if (fd >= 0) {
        ioctl(fd, UI_DEV_DESTROY);
        close(fd);
    }
}

static int open_source_device(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_errno("open source event");
    }
    return fd;
}

static int set_device_grab(int fd, int enabled) {
    if (ioctl(fd, EVIOCGRAB, enabled) < 0) {
        return -1;
    }
    return 0;
}

static void reset_gesture(struct gesture_state *st) {
    memset(st, 0, sizeof(*st));
    st->current_slot = 0;
}

static void start_touch(struct gesture_state *st) {
    st->touch_started = 1;
    st->touch_moved = 0;
    st->suppress_tap_until_release = 0;
    st->touch_origin_valid = 0;
    if (st->x_valid && st->y_valid) {
        st->touch_origin_x = st->x;
        st->touch_origin_y = st->y;
        st->touch_origin_valid = 1;
    }
    st->last_valid = 0;
    st->max_finger_count = st->finger_count > 0 ? st->finger_count : 1;
    clock_gettime(CLOCK_MONOTONIC, &st->down_ts);
}

static int maybe_emit_tap(const struct options *opt, const struct gesture_state *st, int mouse_fd) {
    struct timespec now;
    long held_ms;
    int button_code;

    if (!st->touch_started || st->touch_moved || st->source_button_down ||
        st->suppress_tap_until_release) {
        return 0;
    }

    if (st->max_finger_count >= 3) {
        return 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    held_ms = elapsed_ms(&st->down_ts, &now);
    if (held_ms > opt->tap_timeout_ms) {
        return 0;
    }

    button_code = pick_button_code(st->max_finger_count);
    if (emit_button_click(mouse_fd, button_code) < 0) {
        return -1;
    }

    if (g_verbose) {
        log_msg("DBG", "tap detected (%ld ms, fingers=%d)", held_ms, st->max_finger_count);
    }
    return 0;
}

static void update_finger_count(struct gesture_state *st, int code, int value) {
    if (code == BTN_TOOL_FINGER && value) {
        st->finger_count = 1;
    } else if (code == BTN_TOOL_FINGER && !value && st->finger_count == 1) {
        st->finger_count = 0;
    } else if (code == BTN_TOOL_DOUBLETAP && value) {
        st->finger_count = 2;
    } else if (code == BTN_TOOL_DOUBLETAP && !value && st->finger_count == 2) {
        st->finger_count = st->finger_down ? 1 : 0;
    } else if (code == BTN_TOOL_TRIPLETAP && value) {
        st->finger_count = 3;
    } else if (code == BTN_TOOL_TRIPLETAP && !value && st->finger_count == 3) {
        st->finger_count = st->finger_down ? 1 : 0;
    }

    if (st->finger_count > st->max_finger_count) {
        st->max_finger_count = st->finger_count;
    }
}

static int recount_active_slots(const struct gesture_state *st) {
    int i;
    int count = 0;

    for (i = 0; i < MAX_MT_SLOTS; ++i) {
        if (st->slot_active[i]) {
            ++count;
        }
    }
    return count;
}

static void update_slot_tracking(struct gesture_state *st, int code, int value) {
    if (code == ABS_MT_SLOT) {
        if (value >= 0 && value < MAX_MT_SLOTS) {
            st->current_slot = value;
        }
        return;
    }

    if (code != ABS_MT_TRACKING_ID) {
        return;
    }

    if (st->current_slot < 0 || st->current_slot >= MAX_MT_SLOTS) {
        st->current_slot = 0;
    }

    st->slot_active[st->current_slot] = (value >= 0) ? 1 : 0;
    st->finger_count = recount_active_slots(st);
    if (st->finger_count > st->max_finger_count) {
        st->max_finger_count = st->finger_count;
    }
}

static void refresh_pointer_position(struct gesture_state *st) {
    if (st->abs_x_valid && st->abs_y_valid) {
        st->x = st->abs_x;
        st->y = st->abs_y;
        st->x_valid = 1;
        st->y_valid = 1;
        return;
    }

    if (st->mt_x_valid[0] && st->mt_y_valid[0]) {
        st->x = st->mt_x[0];
        st->y = st->mt_y[0];
        st->x_valid = 1;
        st->y_valid = 1;
    }
}

static int handle_motion(const struct options *opt, struct gesture_state *st, int mouse_fd) {
    int dx;
    int dy;

    if (!st->finger_down || !st->motion_dirty || !st->x_valid || !st->y_valid) {
        st->motion_dirty = 0;
        return 0;
    }

    if (st->suppress_tap_until_release) {
        st->motion_dirty = 0;
        return 0;
    }

    if (st->finger_count >= 3 && !st->drag_active) {
        int arm_dx;
        int arm_dy;

        if (!st->drag_arming) {
            st->drag_arming = 1;
            st->drag_origin_x = st->x;
            st->drag_origin_y = st->y;
            st->motion_dirty = 0;
            return 0;
        }

        arm_dx = st->x - st->drag_origin_x;
        arm_dy = st->y - st->drag_origin_y;
        st->motion_dirty = 0;
        if (abs(arm_dx) < opt->drag_start_threshold &&
            abs(arm_dy) < opt->drag_start_threshold) {
            if (g_verbose) {
                log_msg("DBG", "three-finger drag arming dx=%d dy=%d threshold=%d", arm_dx, arm_dy, opt->drag_start_threshold);
            }
            return 0;
        }

        if (begin_drag(st, mouse_fd) < 0) {
            log_errno("begin drag");
            return -1;
        }
        st->drag_active = 1;
        dx = arm_dx;
        dy = arm_dy;
        st->drag_arming = 0;
        st->last_x = st->x;
        st->last_y = st->y;
        st->last_valid = 1;
        st->touch_moved = 1;
        if (g_verbose) {
            log_msg("DBG", "three-finger drag unlocked at x=%d y=%d dx=%d dy=%d", st->x, st->y, dx, dy);
        }

        dx = round_away_from_zero((double)dx * opt->sensitivity);
        dy = round_away_from_zero((double)dy * opt->sensitivity);
        if (dx == 0 && dy == 0) {
            return 0;
        }
        if (emit_event(mouse_fd, EV_REL, REL_X, dx) < 0 ||
            emit_event(mouse_fd, EV_REL, REL_Y, dy) < 0 ||
            emit_sync(mouse_fd) < 0) {
            log_errno("emit initial three-finger drag motion");
            return -1;
        }
        return 0;
    }

    if (st->drag_active && st->drag_arming) {
        int arm_dx = st->x - st->drag_origin_x;
        int arm_dy = st->y - st->drag_origin_y;

        st->motion_dirty = 0;
        if (abs(arm_dx) < opt->drag_start_threshold &&
            abs(arm_dy) < opt->drag_start_threshold) {
            if (g_verbose) {
                log_msg("DBG", "drag arming dx=%d dy=%d threshold=%d", arm_dx, arm_dy, opt->drag_start_threshold);
            }
            return 0;
        }

        dx = trim_deadzone_delta(arm_dx, opt->drag_start_threshold);
        dy = trim_deadzone_delta(arm_dy, opt->drag_start_threshold);
        st->drag_arming = 0;
        st->last_x = st->x;
        st->last_y = st->y;
        st->last_valid = 1;
        st->touch_moved = 1;
        if (g_verbose) {
            log_msg("DBG", "drag movement unlocked at x=%d y=%d dx=%d dy=%d", st->x, st->y, dx, dy);
        }

        dx = round_away_from_zero((double)dx * opt->sensitivity);
        dy = round_away_from_zero((double)dy * opt->sensitivity);
        if (dx == 0 && dy == 0) {
            return 0;
        }
        if (emit_event(mouse_fd, EV_REL, REL_X, dx) < 0 ||
            emit_event(mouse_fd, EV_REL, REL_Y, dy) < 0 ||
            emit_sync(mouse_fd) < 0) {
            log_errno("emit initial drag motion");
            return -1;
        }
        return 0;
    }

    if (!st->last_valid) {
        if (!st->touch_origin_valid) {
            st->touch_origin_x = st->x;
            st->touch_origin_y = st->y;
            st->touch_origin_valid = 1;
        }
        st->last_x = st->x;
        st->last_y = st->y;
        st->last_valid = 1;
        st->motion_dirty = 0;
        return 0;
    }

    dx = st->x - st->last_x;
    dy = st->y - st->last_y;
    st->last_x = st->x;
    st->last_y = st->y;
    st->motion_dirty = 0;

    if (abs(dx) > opt->jump_limit || abs(dy) > opt->jump_limit) {
        if (g_verbose) {
            log_msg("DBG", "discarded jump dx=%d dy=%d", dx, dy);
        }
        return 0;
    }

    if (st->max_finger_count <= 1) {
        if (st->touch_origin_valid &&
            (abs(st->x - st->touch_origin_x) > opt->tap_move_limit ||
             abs(st->y - st->touch_origin_y) > opt->tap_move_limit)) {
            st->touch_moved = 1;
        }
    } else if (abs(dx) > opt->tap_move_limit || abs(dy) > opt->tap_move_limit) {
        st->touch_moved = 1;
    }

    if (st->finger_count == 2 && !st->drag_active) {
        int wheel;

        st->wheel_remainder += (double)dy / opt->scroll_scale;
        wheel = round_away_from_zero(st->wheel_remainder);
        if (wheel != 0) {
            st->wheel_remainder -= (double)wheel;
            if (emit_event(mouse_fd, EV_REL, REL_WHEEL, wheel) < 0 || emit_sync(mouse_fd) < 0) {
                log_errno("emit REL_WHEEL");
                return -1;
            }
        }
        return 0;
    }

    dx = round_away_from_zero((double)dx * opt->sensitivity);
    dy = round_away_from_zero((double)dy * opt->sensitivity);
    if (dx == 0 && dy == 0) {
        return 0;
    }

    if (emit_event(mouse_fd, EV_REL, REL_X, dx) < 0 ||
        emit_event(mouse_fd, EV_REL, REL_Y, dy) < 0 ||
        emit_sync(mouse_fd) < 0) {
        log_errno("emit relative motion");
        return -1;
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -i <path>    input device (required)\n"
            "  -u <path>    uinput device (default: %s)\n"
            "  -n <name>    virtual mouse name\n"
            "  -s <value>   pointer sensitivity (default: %.2f)\n"
            "  -w <value>   two-finger wheel scale (default: %.2f)\n"
            "  -t <ms>      tap timeout in milliseconds (default: %d)\n"
            "  -m <px>      tap move limit before cancel (default: %d)\n"
            "  -j <px>      jump filter threshold (default: %d)\n"
            "  -T <ms>      double tap timeout for drag (default: %d)\n"
            "  -D <px>      drag start threshold after second tap (default: %d)\n"
            "  -G           do not grab the source input device\n"
            "  -d           run as daemon\n"
            "  -v           verbose logging\n",
            prog,
            DEFAULT_UINPUT_DEV,
            DEFAULT_SENSITIVITY,
            DEFAULT_SCROLL_SCALE,
            DEFAULT_TAP_TIMEOUT_MS,
            DEFAULT_TAP_MOVE_LIMIT,
            DEFAULT_JUMP_LIMIT,
            DEFAULT_DOUBLE_TAP_TIMEOUT_MS,
            DEFAULT_DRAG_START_THRESHOLD);
}

static int parse_int_arg(const char *value, int *out) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);

    if (value[0] == '\0' || end == NULL || *end != '\0') {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int parse_double_arg(const char *value, double *out) {
    char *end = NULL;
    double parsed = strtod(value, &end);

    if (value[0] == '\0' || end == NULL || *end != '\0') {
        return -1;
    }
    *out = parsed;
    return 0;
}

static int parse_args(int argc, char **argv, struct options *opt) {
    int i;

    opt->src_dev = NULL;
    opt->uinput_dev = DEFAULT_UINPUT_DEV;
    opt->device_name = DEFAULT_DEVICE_NAME;
    opt->sensitivity = DEFAULT_SENSITIVITY;
    opt->scroll_scale = DEFAULT_SCROLL_SCALE;
    opt->tap_timeout_ms = DEFAULT_TAP_TIMEOUT_MS;
    opt->tap_move_limit = DEFAULT_TAP_MOVE_LIMIT;
    opt->jump_limit = DEFAULT_JUMP_LIMIT;
    opt->double_tap_timeout_ms = DEFAULT_DOUBLE_TAP_TIMEOUT_MS;
    opt->drag_start_threshold = DEFAULT_DRAG_START_THRESHOLD;
    opt->grab_device = 1;
    opt->daemon_mode = 0;
    opt->verbose = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            opt->src_dev = argv[++i];
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            opt->uinput_dev = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            opt->device_name = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            if (parse_double_arg(argv[++i], &opt->sensitivity) < 0 || opt->sensitivity <= 0.0) {
                return -1;
            }
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            if (parse_double_arg(argv[++i], &opt->scroll_scale) < 0 || opt->scroll_scale <= 0.0) {
                return -1;
            }
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &opt->tap_timeout_ms) < 0 || opt->tap_timeout_ms < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &opt->tap_move_limit) < 0 || opt->tap_move_limit < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &opt->jump_limit) < 0 || opt->jump_limit < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &opt->double_tap_timeout_ms) < 0 || opt->double_tap_timeout_ms < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &opt->drag_start_threshold) < 0 || opt->drag_start_threshold < 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "-G") == 0) {
            opt->grab_device = 0;
        } else if (strcmp(argv[i], "-d") == 0) {
            opt->daemon_mode = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            opt->verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            return -1;
        }
    }

    if (opt->src_dev == NULL || opt->src_dev[0] == '\0') {
        return -1;
    }

    return 0;
}

static int maybe_daemonize(void) {
    pid_t pid = fork();

    if (pid < 0) {
        log_errno("fork");
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    if (setsid() < 0) {
        log_errno("setsid");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        log_errno("fork");
        return -1;
    }
    if (pid > 0) {
        exit(0);
    }

    umask(0);
    chdir("/");
    close(STDIN_FILENO);
    return 0;
}

static int forward_loop(const struct options *opt, int mouse_fd) {
    struct gesture_state st;
    struct input_event ev;
    int in_fd;

    reset_gesture(&st);

    for (;;) {
        in_fd = open_source_device(opt->src_dev);
        if (in_fd < 0) {
            sleep_ms(RECONNECT_DELAY_MS);
            continue;
        }

        if (opt->grab_device) {
            if (set_device_grab(in_fd, 1) < 0) {
                log_errno("EVIOCGRAB source event");
                close(in_fd);
                sleep_ms(RECONNECT_DELAY_MS);
                continue;
            }
            log_msg("INF", "grabbed source device %s", opt->src_dev);
        }

        log_msg("INF", "forwarding %s -> %s", opt->src_dev, opt->device_name);
        reset_gesture(&st);

        for (;;) {
            ssize_t n = read(in_fd, &ev, sizeof(ev));

            if (n == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    update_finger_count(&st, ev.code, ev.value);

                    if (ev.code == BTN_TOUCH) {
                        int was_down = st.finger_down;
                        st.finger_down = ev.value ? 1 : 0;
                        if (st.finger_down && !was_down) {
                            start_touch(&st);
                            if (st.finger_count == 0) {
                                st.finger_count = 1;
                            }
                            st.drag_pending_begin = 0;
                        } else if (!st.finger_down && was_down) {
                            if (st.drag_active) {
                                if (end_drag(&st, mouse_fd) < 0) {
                                    log_errno("end drag");
                                    if (opt->grab_device) {
                                        set_device_grab(in_fd, 0);
                                    }
                                    close(in_fd);
                                    return -1;
                                }
                                st.drag_active = 0;
                                if (st.finger_count == 1) {
                                    st.finger_count = 0;
                                }
                                continue;
                            }
                            if (st.touch_started &&
                                !st.suppress_tap_until_release &&
                                !st.touch_moved &&
                                !st.source_button_down &&
                                st.max_finger_count == 1 &&
                                st.x_valid &&
                                st.y_valid) {
                                if (emit_button_click(mouse_fd, BTN_LEFT) < 0) {
                                    if (opt->grab_device) {
                                        set_device_grab(in_fd, 0);
                                    }
                                    close(in_fd);
                                    return -1;
                                }
                                if (g_verbose) {
                                    log_msg("DBG", "single tap emitted");
                                }
                            } else if (maybe_emit_tap(opt, &st, mouse_fd) < 0) {
                                if (opt->grab_device) {
                                    set_device_grab(in_fd, 0);
                                }
                                close(in_fd);
                                return -1;
                            }
                            if (st.touch_moved || st.max_finger_count != 1) {
                                clear_pending_tap(&st);
                            }
                            clear_motion_state(&st);
                            st.touch_started = 0;
                            st.touch_moved = 0;
                            st.touch_origin_valid = 0;
                            st.suppress_tap_until_release = 0;
                            st.max_finger_count = 0;
                            st.drag_arming = 0;
                            if (st.finger_count == 1) {
                                st.finger_count = 0;
                            }
                        }
                    } else if (ev.code == BTN_MOUSE) {
                        if (ev.value) {
                            int fingers = st.finger_count > 0 ? st.finger_count : (st.max_finger_count > 0 ? st.max_finger_count : 1);
                            st.active_button_code = pick_button_code(fingers);
                            st.source_button_down = 1;
                            if (emit_click(mouse_fd, st.active_button_code, 1) < 0) {
                                log_errno("emit button press");
                                if (opt->grab_device) {
                                    set_device_grab(in_fd, 0);
                                }
                                close(in_fd);
                                return -1;
                            }
                        } else if (st.source_button_down) {
                            int button_code = st.active_button_code != 0 ? st.active_button_code : BTN_LEFT;
                            st.source_button_down = 0;
                            st.active_button_code = 0;
                            clear_motion_state(&st);
                            if (emit_click(mouse_fd, button_code, 0) < 0) {
                                log_errno("emit button release");
                                if (opt->grab_device) {
                                    set_device_grab(in_fd, 0);
                                }
                                close(in_fd);
                                return -1;
                            }
                        }
                    }
                } else if (ev.type == EV_ABS) {
                    update_slot_tracking(&st, ev.code, ev.value);

                    if (ev.code == ABS_X) {
                        st.abs_x = ev.value;
                        st.abs_x_valid = 1;
                        st.motion_dirty = 1;
                    } else if (ev.code == ABS_Y) {
                        st.abs_y = ev.value;
                        st.abs_y_valid = 1;
                        st.motion_dirty = 1;
                    } else if (ev.code == ABS_MT_POSITION_X) {
                        if (st.current_slot >= 0 && st.current_slot < MAX_MT_SLOTS) {
                            st.mt_x[st.current_slot] = ev.value;
                            st.mt_x_valid[st.current_slot] = 1;
                            if (st.current_slot == 0) {
                                st.motion_dirty = 1;
                            }
                        }
                    } else if (ev.code == ABS_MT_POSITION_Y) {
                        if (st.current_slot >= 0 && st.current_slot < MAX_MT_SLOTS) {
                            st.mt_y[st.current_slot] = ev.value;
                            st.mt_y_valid[st.current_slot] = 1;
                            if (st.current_slot == 0) {
                                st.motion_dirty = 1;
                            }
                        }
                    }

                    if (st.motion_dirty) {
                        refresh_pointer_position(&st);
                    }
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    refresh_pointer_position(&st);
                    if (st.drag_active && st.finger_count < 3) {
                        if (end_drag(&st, mouse_fd) < 0) {
                            log_errno("end drag");
                            if (opt->grab_device) {
                                set_device_grab(in_fd, 0);
                            }
                            close(in_fd);
                            return -1;
                        }
                        st.drag_active = 0;
                    }
                    if (handle_motion(opt, &st, mouse_fd) < 0) {
                        if (opt->grab_device) {
                            set_device_grab(in_fd, 0);
                        }
                        close(in_fd);
                        return -1;
                    }
                }
                continue;
            }

            if (n < 0 && errno == EINTR) {
                continue;
            }

            if (n < 0) {
                log_errno("read input");
            } else {
                log_msg("ERR", "short read from input device");
            }
            if (opt->grab_device) {
                set_device_grab(in_fd, 0);
            }
            close(in_fd);
            sleep_ms(RECONNECT_DELAY_MS);
            break;
        }
    }
}

int main(int argc, char **argv) {
    struct options opt;
    int mouse_fd;

    if (parse_args(argc, argv, &opt) < 0) {
        usage(argv[0]);
        return 1;
    }

    g_verbose = opt.verbose;

    if (opt.daemon_mode && maybe_daemonize() < 0) {
        return 1;
    }

    mouse_fd = create_mouse(&opt);
    if (mouse_fd < 0) {
        return 1;
    }

    if (forward_loop(&opt, mouse_fd) < 0) {
        destroy_mouse(mouse_fd);
        return 1;
    }

    destroy_mouse(mouse_fd);
    return 0;
}

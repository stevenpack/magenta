// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/protocol/display.h>
#include <ddk/protocol/input.h>

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <gfx/gfx.h>
#include <hid/hid.h>
#include <hid/usages.h>
#include <magenta/device/console.h>
#include <magenta/listnode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <threads.h>
#include <unistd.h>

#include <mxio/io.h>
#include <mxio/watcher.h>

#include <magenta/syscalls.h>
#include <magenta/syscalls/log.h>
#include <magenta/syscalls/object.h>

#define VCDEBUG 1

#include "vc.h"
#include "vcdebug.h"

#define VC_DEVNAME "vc"

#define LOW_REPEAT_KEY_FREQUENCY_MICRO 250000000
#define HIGH_REPEAT_KEY_FREQUENCY_MICRO 50000000

// framebuffer
static gfx_surface g_hw_gfx;
static mx_device_t* g_fb_device;
static mx_display_protocol_t* g_fb_display_protocol;

static thrd_t g_input_poll_thread;

// single driver instance
static bool g_vc_initialized = false;

static struct list_node g_vc_list = LIST_INITIAL_VALUE(g_vc_list);
static unsigned g_vc_count = 0;
static vc_device_t* g_active_vc;
static unsigned g_active_vc_index;
static vc_battery_info_t g_battery_info;
static mtx_t g_vc_lock = MTX_INIT;

static uint32_t hid_key_to_ansi_code(uint8_t keycode, int modifiers,
                                     keychar_t* keymap, char* buf,
                                     size_t buf_size);

static int modifiers_from_keycode(uint8_t keycode) {
    switch (keycode) {
    case HID_USAGE_KEY_LEFT_SHIFT:
        return MOD_LSHIFT;
    case HID_USAGE_KEY_RIGHT_SHIFT:
        return MOD_RSHIFT;
    case HID_USAGE_KEY_LEFT_ALT:
        return MOD_LALT;
    case HID_USAGE_KEY_RIGHT_ALT:
        return MOD_RALT;
    case HID_USAGE_KEY_LEFT_CTRL:
        return MOD_LCTRL;
    case HID_USAGE_KEY_RIGHT_CTRL:
        return MOD_RCTRL;
    }
    return 0;
}

// Process key sequences that affect the console (scrolling, switching
// console, etc.) without sending input to the current console.  This
// returns whether this key press was handled.
static bool vc_handle_control_keys(uint8_t keycode, int modifiers) {
    switch (keycode) {
    case HID_USAGE_KEY_F1 ... HID_USAGE_KEY_F10:
        if (modifiers & MOD_ALT) {
            vc_set_active_console(keycode - HID_USAGE_KEY_F1);
            return true;
        }
        break;

    case HID_USAGE_KEY_F11:
        if (g_active_vc && (modifiers & MOD_ALT)) {
            vc_device_set_fullscreen(g_active_vc, !(g_active_vc->flags & VC_FLAG_FULLSCREEN));
            return true;
        }
        break;

    case HID_USAGE_KEY_TAB:
        if (modifiers & MOD_ALT) {
            if (modifiers & MOD_SHIFT) {
                vc_set_active_console(g_active_vc_index == 0 ? g_vc_count - 1 : g_active_vc_index - 1);
            } else {
                vc_set_active_console(g_active_vc_index == g_vc_count - 1 ? 0 : g_active_vc_index + 1);
            }
            return true;
        }
        break;

    case HID_USAGE_KEY_UP:
        if (modifiers & MOD_ALT) {
            vc_device_scroll_viewport(g_active_vc, -1);
            return true;
        }
        break;
    case HID_USAGE_KEY_DOWN:
        if (modifiers & MOD_ALT) {
            vc_device_scroll_viewport(g_active_vc, 1);
            return true;
        }
        break;
    case HID_USAGE_KEY_PAGEUP:
        if (modifiers & MOD_SHIFT) {
            vc_device_scroll_viewport(g_active_vc, -(vc_device_rows(g_active_vc) / 2));
            return true;
        }
        break;
    case HID_USAGE_KEY_PAGEDOWN:
        if (modifiers & MOD_SHIFT) {
            vc_device_scroll_viewport(g_active_vc, vc_device_rows(g_active_vc) / 2);
            return true;
        }
        break;

    case HID_USAGE_KEY_DELETE:
        // Provide a CTRL-ALT-DEL reboot sequence
        if ((modifiers & MOD_CTRL) && (modifiers & MOD_ALT)) {
            int fd;
            // Send the reboot command to devmgr
            if ((fd = open("/dev/class/misc/dmctl", O_WRONLY)) >= 0) {
                write(fd, "reboot", strlen("reboot"));
                close(fd);
            }
            return true;
        }
        break;
    }
    return false;
}

static void vc_handle_key_press(uint8_t keycode, int modifiers) {
    if (vc_handle_control_keys(keycode, modifiers))
        return;

    // TODO: ensure active vc can't change while this is going on
    mtx_lock(&g_active_vc->fifo.lock);
    if (mx_hid_fifo_size(&g_active_vc->fifo) == 0) {
        g_active_vc->flags |= VC_FLAG_RESETSCROLL;
    }
    char output[4];
    uint32_t length = hid_key_to_ansi_code(
        keycode, modifiers, g_active_vc->keymap, output, sizeof(output));
    if (length > 0) {
        // This writes multi-byte sequences atomically, so that if space
        // isn't available for the full sequence -- if the program running
        // on the console is currently not reading input -- then nothing is
        // written.  This has the nice property that we won't get partial
        // key code sequences in that case.
        mx_hid_fifo_write(&g_active_vc->fifo, output, length);

        device_state_set(&g_active_vc->device, DEV_STATE_READABLE);
    }
    mtx_unlock(&g_active_vc->fifo.lock);
}

static void vc_process_kb_report(uint8_t* report_buf, hid_keys_t* key_state,
                                 int* cur_idx, int* prev_idx,
                                 hid_keys_t* key_pressed,
                                 hid_keys_t* key_released, int* modifiers) {
    // process the key
    uint8_t keycode;
    hid_keys_t key_delta;

    hid_kbd_parse_report(report_buf, &key_state[*cur_idx]);
    hid_kbd_pressed_keys(&key_state[*prev_idx], &key_state[*cur_idx], &key_delta);
    if (key_pressed) {
        memcpy(key_pressed, &key_delta, sizeof(key_delta));
    }
    hid_for_every_key(&key_delta, keycode) {
        *modifiers |= modifiers_from_keycode(keycode);
        vc_handle_key_press(keycode, *modifiers);
    }

    hid_kbd_released_keys(&key_state[*prev_idx], &key_state[*cur_idx], &key_delta);
    if (key_released) {
        memcpy(key_released, &key_delta, sizeof(key_delta));
    }
    hid_for_every_key(&key_delta, keycode) {
        *modifiers &= ~modifiers_from_keycode(keycode);
    }

    // swap key states
    *cur_idx = 1 - *cur_idx;
    *prev_idx = 1 - *prev_idx;
}

static int vc_input_thread(void* arg) {
    int fd = (int)(uintptr_t)arg;

    uint8_t previous_report_buf[8];
    uint8_t report_buf[8];
    hid_keys_t key_state[2];
    memset(&key_state[0], 0, sizeof(hid_keys_t));
    memset(&key_state[1], 0, sizeof(hid_keys_t));
    int cur_idx = 0;
    int prev_idx = 1;
    int modifiers = 0;
    uint64_t repeat_interval = MX_TIME_INFINITE;
    bool repeat_enabled = true;
    char* flag = getenv("gfxconsole.keyrepeat");
    if (flag && (!strcmp(flag, "0") || !strcmp(flag, "false"))) {
        printf("vc: Key repeat disabled\n");
        repeat_enabled = false;
    }

    for (;;) {
        mx_status_t rc = mxio_wait_fd(fd, MXIO_EVT_READABLE, NULL, repeat_interval);

        if (rc == ERR_TIMED_OUT) {
            // Times out only when need to repeat.
            vc_process_kb_report(previous_report_buf, key_state, &cur_idx, &prev_idx, NULL, NULL, &modifiers);
            vc_process_kb_report(report_buf, key_state, &cur_idx, &prev_idx, NULL, NULL, &modifiers);
            // Accelerate key repeat until reaching the high frequency
            repeat_interval = repeat_interval * 3 / 4;
            repeat_interval = repeat_interval < HIGH_REPEAT_KEY_FREQUENCY_MICRO ? HIGH_REPEAT_KEY_FREQUENCY_MICRO : repeat_interval;
            continue;
        }

        memcpy(previous_report_buf, report_buf, sizeof(report_buf));
        ssize_t r = read(fd, report_buf, sizeof(report_buf));
        if (r < 0) {
            break; // will be restarted by poll thread if needed
        }
        if ((size_t)(r) != sizeof(report_buf)) {
            repeat_interval = MX_TIME_INFINITE;
            continue;
        }
        // eat the input if there is no active vc
        if (!g_active_vc) {
            repeat_interval = MX_TIME_INFINITE;
            continue;
        }

        hid_keys_t key_pressed, key_released;
        vc_process_kb_report(report_buf, key_state, &cur_idx, &prev_idx,
                             &key_pressed, &key_released, &modifiers);

        if (repeat_enabled) {
            // Check if any non modifiers were pressed
            bool pressed = false, released = false;
            for (int i = 0; i < 7; i++) {
                if (key_pressed.keymask[i]) {
                    pressed = true;
                    break;
                }
            }
            // Check if any key was released
            for (int i = 0; i < 8; i++) {
                if (key_released.keymask[i]) {
                    released = true;
                    break;
                }
            }

            if (released) {
                // Do not repeat released keys, block on next mxio_wait_fd
                repeat_interval = MX_TIME_INFINITE;
            } else if (pressed) {
                // Set timeout on next mxio_wait_fd
                repeat_interval = LOW_REPEAT_KEY_FREQUENCY_MICRO;
            }
        }
    }
    return 0;
}

#define DEV_INPUT "/dev/class/input"

static mx_status_t vc_input_device_added(int dirfd, const char* fn, void* cookie) {
    int fd;
    if ((fd = openat(dirfd, fn, O_RDONLY)) < 0) {
        return NO_ERROR;
    }

    printf("vc: new input device %s/%s\n", DEV_INPUT, fn);

    // test to see if this is a device we can read
    int proto = INPUT_PROTO_NONE;
    ssize_t rc = ioctl_input_get_protocol(fd, &proto);
    if (rc > 0 && proto != INPUT_PROTO_KBD) {
        // skip devices that aren't keyboards
        close(fd);
        return NO_ERROR;
    }

    // start a thread to wait on the fd
    char tname[64];
    thrd_t t;
    snprintf(tname, sizeof(tname), "vc-input-%s", fn);
    int ret = thrd_create_with_name(&t, vc_input_thread, (void*)(uintptr_t)fd, tname);
    if (ret != thrd_success) {
        xprintf("vc: input thread %s did not start (return value=%d)\n", tname, ret);
        close(fd);
    }
    thrd_detach(t);
    return NO_ERROR;
}

static int vc_input_devices_poll_thread(void* arg) {
    int dirfd;
    if ((dirfd = open(DEV_INPUT, O_DIRECTORY | O_RDONLY)) < 0) {
        return -1;
    }
    mxio_watch_directory(dirfd, vc_input_device_added, NULL);
    close(dirfd);
    return -1;
}

static void __vc_set_active(vc_device_t* dev, unsigned index) {
    // must be called while holding g_vc_lock
    if (g_active_vc)
        g_active_vc->active = false;
    dev->active = true;
    g_active_vc = dev;
    g_active_vc->flags &= ~VC_FLAG_HASINPUT;
    g_active_vc_index = index;
}

mx_status_t vc_set_console_to_active(vc_device_t* dev) {
    if (dev == NULL)
        return ERR_INVALID_ARGS;

    unsigned i = 0;
    vc_device_t* device = NULL;
    mtx_lock(&g_vc_lock);
    list_for_every_entry (&g_vc_list, device, vc_device_t, node) {
        if (device == dev)
            break;
        i++;
    }
    if (i == g_vc_count) {
        mtx_unlock(&g_vc_lock);
        return ERR_INVALID_ARGS;
    }
    __vc_set_active(dev, i);
    mtx_unlock(&g_vc_lock);
    vc_device_render(g_active_vc);
    return NO_ERROR;
}

mx_status_t vc_set_active_console(unsigned console) {
    if (console >= g_vc_count)
        return ERR_INVALID_ARGS;

    unsigned i = 0;
    vc_device_t* device = NULL;
    mtx_lock(&g_vc_lock);
    list_for_every_entry (&g_vc_list, device, vc_device_t, node) {
        if (i == console)
            break;
        i++;
    }
    if (device == g_active_vc) {
        mtx_unlock(&g_vc_lock);
        return NO_ERROR;
    }
    __vc_set_active(device, console);
    mtx_unlock(&g_vc_lock);
    vc_device_render(g_active_vc);
    return NO_ERROR;
}

void vc_get_status_line(char* str, int n) {
    vc_device_t* device = NULL;
    char* ptr = str;
    unsigned i = 0;
    // TODO add process name, etc.
    mtx_lock(&g_vc_lock);
    list_for_every_entry (&g_vc_list, device, vc_device_t, node) {
        if (n <= 0) {
            break;
        }

        int lines = vc_device_get_scrollback_lines(device);
        int chars = snprintf(ptr, n, "%s[%u] %s%c    %c%c \033[m",
                             device->active ? "\033[36m\033[1m" : "",
                             i,
                             device->title,
                             device->flags & VC_FLAG_HASINPUT ? '*' : ' ',
                             lines > 0 && -device->vpy < lines ? '<' : ' ',
                             device->vpy < 0 ? '>' : ' ');
        ptr += chars;
        n -= chars;
        i++;
    }
    mtx_unlock(&g_vc_lock);
}

void vc_get_battery_info(vc_battery_info_t* info) {
    mtx_lock(&g_vc_lock);
    memcpy(info, &g_battery_info, sizeof(vc_battery_info_t));
    mtx_unlock(&g_vc_lock);
}

// implement device protocol:

static mx_status_t vc_device_release(mx_device_t* dev) {
    vc_device_t* vc = get_vc_device(dev);

    mtx_lock(&g_vc_lock);
    list_delete(&vc->node);
    g_vc_count -= 1;

    if (vc->active) {
        g_active_vc = NULL;
        if (g_active_vc_index >= g_vc_count) {
            g_active_vc_index = g_vc_count - 1;
        }
    }

    // need to fixup g_active_vc and g_active_vc_index after deletion
    vc_device_t* d = NULL;
    unsigned i = 0;
    list_for_every_entry (&g_vc_list, d, vc_device_t, node) {
        if (g_active_vc) {
            if (d == g_active_vc) {
                g_active_vc_index = i;
                break;
            }
        } else {
            if (i == g_active_vc_index) {
                __vc_set_active(d, i);
                break;
            }
        }
        i++;
    }
    mtx_unlock(&g_vc_lock);

    vc_device_free(vc);

    // redraw the status line, or the full screen
    if (g_active_vc) {
        vc_device_render(g_active_vc);
    }
    return NO_ERROR;
}

// Converts the given HID keycode into an equivalent ANSI (VT100) byte
// sequence, for the given modifier key state and keymap.  This writes the
// result into |buf| and returns the number of bytes that were written.
static uint32_t hid_key_to_ansi_code(uint8_t keycode, int modifiers,
                                     keychar_t* keymap, char* buf,
                                     size_t buf_size) {
    // Consistency check: Max size of byte sequences we produce below.
    if (buf_size != 4)
        return 0;

    uint8_t ch = hid_map_key(keycode, modifiers & MOD_SHIFT, keymap);
    if (ch) {
        if (modifiers & MOD_CTRL) {
            uint8_t sub = modifiers & MOD_SHIFT ? 'A' : 'a';
            buf[0] = (char)(ch - sub + 1);
        } else {
            buf[0] = ch;
        }
        return 1;
    }

    switch (keycode) {
    // generate special stuff for a few different keys
    case HID_USAGE_KEY_ENTER:
    case HID_USAGE_KEY_KP_ENTER:
        buf[0] = '\n';
        return 1;
    case HID_USAGE_KEY_BACKSPACE:
        buf[0] = '\b';
        return 1;
    case HID_USAGE_KEY_TAB:
        buf[0] = '\t';
        return 1;
    case HID_USAGE_KEY_ESC:
        buf[0] = 0x1b;
        return 1;

    // generate vt100 key codes for arrows
    case HID_USAGE_KEY_UP:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 65;
        return 3;
    case HID_USAGE_KEY_DOWN:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 66;
        return 3;
    case HID_USAGE_KEY_RIGHT:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 67;
        return 3;
    case HID_USAGE_KEY_LEFT:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 68;
        return 3;
    case HID_USAGE_KEY_HOME:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 'H';
        return 3;
    case HID_USAGE_KEY_END:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = 'F';
        return 3;
    case HID_USAGE_KEY_DELETE:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = '3';
        buf[3] = '~';
        return 4;
    case HID_USAGE_KEY_PAGEUP:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = '5';
        buf[3] = '~';
        return 4;
    case HID_USAGE_KEY_PAGEDOWN:
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = '6';
        buf[3] = '~';
        return 4;
    }
    // ignore unknown keys; character keys were handled above
    return 0;
}

static ssize_t vc_device_read(mx_device_t* dev, void* buf, size_t count, mx_off_t off) {
    vc_device_t* vc = get_vc_device(dev);

    mtx_lock(&vc->fifo.lock);
    ssize_t result = mx_hid_fifo_read(&vc->fifo, buf, count);
    if (mx_hid_fifo_size(&vc->fifo) == 0) {
        device_state_clr(dev, DEV_STATE_READABLE);
    }
    mtx_unlock(&vc->fifo.lock);

    if (result == 0)
        result = ERR_SHOULD_WAIT;
    return result;
}

static ssize_t vc_device_write(mx_device_t* dev, const void* buf, size_t count, mx_off_t off) {
    vc_device_t* vc = get_vc_device(dev);
    mtx_lock(&vc->lock);
    vc->invy0 = vc_device_rows(vc) + 1;
    vc->invy1 = -1;
    const uint8_t* str = (const uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        vc->textcon.putc(&vc->textcon, str[i]);
    }
    if (vc->invy1 >= 0) {
        vc_gfx_invalidate(vc, 0, vc->invy0, vc->columns, vc->invy1 - vc->invy0);
    }
    if (!vc->active && !(vc->flags & VC_FLAG_HASINPUT)) {
        vc->flags |= VC_FLAG_HASINPUT;
        vc_device_write_status(vc);
        vc_gfx_invalidate_status(vc);
    }
    mtx_unlock(&vc->lock);
    return count;
}

static ssize_t vc_device_ioctl(mx_device_t* dev, uint32_t op, const void* cmd, size_t cmdlen, void* reply, size_t max) {
    vc_device_t* vc = get_vc_device(dev);
    switch (op) {
    case IOCTL_CONSOLE_GET_DIMENSIONS: {
        auto* dims = reinterpret_cast<ioctl_console_dimensions_t*>(reply);
        if (max < sizeof(*dims)) {
            return ERR_BUFFER_TOO_SMALL;
        }
        dims->width = vc->columns;
        dims->height = vc_device_rows(vc);
        return sizeof(*dims);
    }
    case IOCTL_CONSOLE_SET_ACTIVE_VC:
        return vc_set_console_to_active(vc);
    case IOCTL_DISPLAY_GET_FB: {
        if (max < sizeof(ioctl_display_get_fb_t)) {
            return ERR_BUFFER_TOO_SMALL;
        }
        auto* fb = reinterpret_cast<ioctl_display_get_fb_t*>(reply);
        fb->info.format = vc->gfx->format;
        fb->info.width = vc->gfx->width;
        fb->info.height = vc->gfx->height;
        fb->info.stride = vc->gfx->stride;
        fb->info.pixelsize = vc->gfx->pixelsize;
        fb->info.flags = 0;
        //TODO: take away access to the vmo when the client closes the device
        mx_status_t status = mx_handle_duplicate(vc->gfx_vmo, MX_RIGHT_SAME_RIGHTS, &fb->vmo);
        if (status < 0) {
            return status;
        } else {
            return sizeof(ioctl_display_get_fb_t);
        }
    }
    case IOCTL_DISPLAY_FLUSH_FB:
        vc_gfx_invalidate_all(vc);
        return NO_ERROR;
    case IOCTL_DISPLAY_FLUSH_FB_REGION: {
        auto* rect = reinterpret_cast<const ioctl_display_region_t*>(cmd);
        if (cmdlen < sizeof(*rect)) {
            return ERR_INVALID_ARGS;
        }
        vc_gfx_invalidate_region(vc, rect->x, rect->y, rect->width, rect->height);
        return NO_ERROR;
    }
    case IOCTL_DISPLAY_SET_FULLSCREEN: {
        if (cmdlen < sizeof(uint32_t) || !cmd) {
            return ERR_INVALID_ARGS;
        }
        vc_device_set_fullscreen(vc, !!*(uint32_t*)cmd);
        return NO_ERROR;
    }
    default:
        return ERR_NOT_SUPPORTED;
    }
}

static mx_protocol_device_t vc_device_proto;

extern mx_driver_t _driver_vc_root;

// opening the root device returns a new vc device instance
static mx_status_t vc_root_open(mx_device_t* dev, mx_device_t** dev_out, uint32_t flags) {
    mx_status_t status;
    vc_device_t* device;
    if ((status = vc_device_alloc(&g_hw_gfx, &device)) < 0) {
        return status;
    }

    // init the new device
    char name[8];
    snprintf(name, sizeof(name), "vc%u", g_vc_count);
    device_init(&device->device, &_driver_vc_root, name, &vc_device_proto);

    if (dev) {
        // if called normally, add the instance
        // if dev is null, we're creating the log console
        device->device.protocol_id = MX_PROTOCOL_CONSOLE;
        status = device_add_instance(&device->device, dev);
        if (status != NO_ERROR) {
            vc_device_free(device);
            return status;
        }
    }

    // add to the vc list
    mtx_lock(&g_vc_lock);
    list_add_tail(&g_vc_list, &device->node);
    g_vc_count++;
    mtx_unlock(&g_vc_lock);

    // make this the active vc if it's the first one
    if (!g_active_vc) {
        vc_set_active_console(0);
    } else {
        vc_device_render(g_active_vc);
    }

    *dev_out = &device->device;
    return NO_ERROR;
}

static int vc_log_reader_thread(void* arg) {
    auto* dev = reinterpret_cast<mx_device_t*>(arg);
    mx_handle_t h;

    if (mx_log_create(MX_LOG_FLAG_READABLE, &h) < 0) {
        printf("vc log listener: cannot open log\n");
        return -1;
    }

    char buf[MX_LOG_RECORD_MAX];
    mx_log_record_t* rec = (mx_log_record_t*)buf;
    mx_status_t status;
    for (;;) {
        if ((status = mx_log_read(h, MX_LOG_RECORD_MAX, rec, 0)) < 0) {
            if (status == ERR_SHOULD_WAIT) {
                mx_object_wait_one(h, MX_LOG_READABLE, MX_TIME_INFINITE, NULL);
                continue;
            }
            break;
        }
        char tmp[64];
        snprintf(tmp, 64, "\033[32m%05d.%03d\033[39m] \033[31m%05" PRIu64 ".\033[36m%05" PRIu64 "\033[39m> ",
                 (int)(rec->timestamp / 1000000000ULL),
                 (int)((rec->timestamp / 1000000ULL) % 1000ULL),
                 rec->pid, rec->tid);
        vc_device_write(dev, tmp, strlen(tmp), 0);
        vc_device_write(dev, rec->data, rec->datalen, 0);
        if ((rec->datalen == 0) || (rec->data[rec->datalen - 1] != '\n')) {
            vc_device_write(dev, "\n", 1, 0);
        }
    }

    const char* oops = "<<LOG ERROR>>\n";
    vc_device_write(dev, oops, strlen(oops), 0);

    return 0;
}

static int vc_battery_poll_thread(void* arg) {
    char str[16];
    for (;;) {
        int fd = open("/dev/class/misc/acpi-battery", O_RDONLY);
        if (fd < 0) {
            printf("vc: no battery\n");
            return -1;
        }
        ssize_t r = read(fd, str, sizeof(str));
        close(fd);
        if (r < 0) {
            break;
        }
        mtx_lock(&g_vc_lock);
        if (str[0] == 'e') {
            g_battery_info.state = ERROR;
            g_battery_info.pct = -1;
        } else {
            if (str[0] == 'c') {
                g_battery_info.state = CHARGING;
                g_battery_info.pct = atoi(&str[1]);
            } else {
                g_battery_info.state = NOT_CHARGING;
                g_battery_info.pct = atoi(str);
            }
        }
        mtx_unlock(&g_vc_lock);
        if (g_active_vc) {
            vc_device_write_status(g_active_vc);
            vc_gfx_invalidate_status(g_active_vc);
        }
        mx_nanosleep(MX_MSEC(1000));
    }
    return 0;
}

static mx_status_t vc_misc_device_added(int dirfd, const char* fn, void* cookie) {
    if (strcmp("acpi-battery", fn)) {
        return NO_ERROR;
    }
    printf("vc: found battery\n");
    thrd_t t;
    int rc = thrd_create_with_name(&t, vc_battery_poll_thread, NULL, "vc-battery-poll");
    if (rc != thrd_success) {
        return -1;
    }
    thrd_detach(t);
    return NO_ERROR;
}

static int vc_misc_poll_thread(void* arg) {
    int dirfd;
    if ((dirfd = open("/dev/class/misc", O_DIRECTORY | O_RDONLY)) < 0) {
        return -1;
    }
    mxio_watch_directory(dirfd, vc_misc_device_added, NULL);
    close(dirfd);
    return 0;
}

static mx_protocol_device_t vc_root_proto;

static void display_flush(uint starty, uint endy)
{
    g_fb_display_protocol->flush(g_fb_device);
}

static mx_status_t vc_root_bind(mx_driver_t* drv, mx_device_t* dev, void** cookie) {
    if (g_vc_initialized) {
        // disallow multiple instances
        return ERR_NOT_SUPPORTED;
    }

    mx_display_protocol_t* disp;
    mx_status_t status;
    if ((status = device_get_protocol(dev, MX_PROTOCOL_DISPLAY, (void**)&disp)) < 0) {
        return status;
    }

    // get display info
    mx_display_info_t info;
    if ((status = disp->get_mode(dev, &info)) < 0) {
        return status;
    }

    // get framebuffer
    void* framebuffer;
    if ((status = disp->get_framebuffer(dev, &framebuffer)) < 0) {
        return status;
    }

    // initialize the hw surface
    if ((status = gfx_init_surface(&g_hw_gfx, framebuffer, info.width, info.height, info.stride, info.format, 0)) < 0) {
        return status;
    }

    // save some state
    g_fb_device = dev;
    g_fb_display_protocol = disp;

    // if the underlying device requires flushes, set the pointer to a flush op
    if (disp->flush) {
        g_hw_gfx.flush = display_flush;
    }

    // publish the root vc device. opening this device will create a new vc
    mx_device_t* device;
    status = device_create(&device, drv, VC_DEVNAME, &vc_root_proto);
    if (status != NO_ERROR) {
        return status;
    }

    // start a thread to listen for new input devices
    int ret = thrd_create_with_name(&g_input_poll_thread, vc_input_devices_poll_thread, NULL, "vc-inputdev-poll");
    if (ret != thrd_success) {
        xprintf("vc: input polling thread did not start (return value=%d)\n", ret);
    }

    device->protocol_id = MX_PROTOCOL_CONSOLE;
    status = device_add(device, dev);
    if (status != NO_ERROR) {
        goto fail;
    }

    g_vc_initialized = true;
    xprintf("initialized vc on display %s, width=%u height=%u stride=%u format=%u\n",
            dev->name, info.width, info.height, info.stride, info.format);

    if (vc_root_open(NULL, &dev, 0) == NO_ERROR) {
        thrd_t t;
        thrd_create_with_name(&t, vc_log_reader_thread, dev, "vc-log-reader");
    }

    thrd_t u;
    thrd_create_with_name(&u, vc_misc_poll_thread, NULL, "vc-misc-poll");

    return NO_ERROR;
fail:
    free(device);
    // TODO clean up threads
    return status;
}

mx_driver_t _driver_vc_root;

__attribute__((constructor)) static void initialize(void) {
    vc_device_proto.release = vc_device_release;
    vc_device_proto.read = vc_device_read;
    vc_device_proto.write = vc_device_write;
    vc_device_proto.ioctl = vc_device_ioctl;

    vc_root_proto.open = vc_root_open;

    _driver_vc_root.ops.bind = vc_root_bind;
}

MAGENTA_DRIVER_BEGIN(_driver_vc_root, "virtconsole", "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_DISPLAY),
MAGENTA_DRIVER_END(_driver_vc_root)

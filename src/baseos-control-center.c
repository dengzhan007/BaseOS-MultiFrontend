/*
 * BaseOS H700 framebuffer frontend selector / diagnostic control center.
 * The H700 kernel has no fbcon; this UI draws directly to /dev/fb0.
 * It does not launch or modify frontend binaries.
 */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "baseos-control-center-input.h"

#define MAX_INPUTS 24
#define MAX_ENTRIES 64
#define FIELD 320
#define LABEL 160
#define CATALOG "/run/baseos-frontend-catalog"
#define SELECTION "/run/baseos-frontend-selection"
#define MANAGER "/usr/sbin/baseos-frontend-manager"
#define FONT "/usr/share/baseos/boot.ttf"
#define TEST_AXIS_LX (KEY_MAX + 1)
#define TEST_AXIS_LY (KEY_MAX + 2)
#define TEST_AXIS_RX (KEY_MAX + 3)
#define TEST_AXIS_RY (KEY_MAX + 4)

typedef struct {
    char id[64], label[LABEL], package[64], profile[64];
    char root[FIELD], package_dir[FIELD], profile_config[FIELD];
    char kind[24], status[24];
    int priority;
} Frontend;

typedef struct { int fd; BaseOSInputState state; } Input;

static Frontend entries[MAX_ENTRIES];
static Input inputs[MAX_INPUTS];
static int entry_count, input_count, selected, offset;
static int fb_fd = -1;
static uint8_t *fb_mem;
static uint8_t *fb_canvas;
static size_t fb_size;
static size_t fb_frame_size;
static struct fb_var_screeninfo fb_var;
static struct fb_fix_screeninfo fb_fix;
static FT_Library ft_lib;
static FT_Face ft_face;
static int confirm_mode;
static int confirm_action;
static int page_diagnostics;
static int last_tested_key = -1;
static char profile_source_id[64];
static int text_entry_mode;
static int text_entry_cursor;
static int text_entry_caps;
static char text_entry[48];
/* RG34XX/H700 vendor button layout: L2/R2 report BTN_SELECT/BTN_START and
 * MENU reports BTN_TL2, so the on-screen names cannot follow the Linux names. */
static int h700_keys = 1;
static char status_text[160] = "Ready";
static char current_default_id[64] = "auto";
static volatile sig_atomic_t running = 1;

enum { CONFIRM_NONE, CONFIRM_PROFILE, CONFIRM_PACKAGE };
enum { TEXT_ENTRY_NONE, TEXT_ENTRY_CREATE_PROFILE, TEXT_ENTRY_RENAME_PROFILE };

static void stop_ui(int sig) { (void)sig; running = 0; }

static int bit_set(const unsigned long *bits, int n)
{
    return (bits[n / (8 * (int)sizeof(unsigned long))] >>
            (n % (8 * (int)sizeof(unsigned long)))) & 1UL;
}

static int open_inputs(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *de;
    if (!dir) return 0;
    input_count = 0;
    while ((de = readdir(dir)) != NULL && input_count < MAX_INPUTS) {
        char path[FIELD];
        unsigned long keybits[(KEY_MAX + 8 * sizeof(unsigned long)) /
                              (8 * sizeof(unsigned long))];
        unsigned long absbits[(ABS_MAX + 8 * sizeof(unsigned long)) /
                              (8 * sizeof(unsigned long))];
        int fd, has_key_input, has_hat_input, has_axis_input;
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        if (snprintf(path, sizeof(path), "/dev/input/%s", de->d_name) >=
            (int)sizeof(path)) continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        memset(keybits, 0, sizeof(keybits));
        memset(absbits, 0, sizeof(absbits));
        has_key_input = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0 &&
            (bit_set(keybits, BTN_SOUTH) || bit_set(keybits, BTN_EAST) ||
             bit_set(keybits, BTN_WEST) || bit_set(keybits, BTN_C) ||
             bit_set(keybits, BTN_Z) || bit_set(keybits, BTN_NORTH) ||
             bit_set(keybits, BTN_START) || bit_set(keybits, BTN_SELECT) ||
             bit_set(keybits, BTN_TL) || bit_set(keybits, BTN_TR) ||
             bit_set(keybits, BTN_TL2) || bit_set(keybits, BTN_TR2) ||
             bit_set(keybits, BTN_THUMBL) || bit_set(keybits, BTN_THUMBR) ||
             bit_set(keybits, BTN_MODE) ||
             bit_set(keybits, KEY_UP) || bit_set(keybits, KEY_DOWN) ||
             bit_set(keybits, KEY_LEFT) || bit_set(keybits, KEY_RIGHT) ||
             bit_set(keybits, BTN_DPAD_UP) || bit_set(keybits, BTN_DPAD_DOWN) ||
             bit_set(keybits, BTN_DPAD_LEFT) || bit_set(keybits, BTN_DPAD_RIGHT));
        has_hat_input = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0 &&
            (bit_set(absbits, ABS_HAT0X) || bit_set(absbits, ABS_HAT0Y));
        has_axis_input = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0 &&
            (bit_set(absbits, ABS_X) || bit_set(absbits, ABS_Y) ||
             bit_set(absbits, ABS_RX) || bit_set(absbits, ABS_RY));
        if (!has_key_input && !has_hat_input && !has_axis_input) {
            close(fd);
            continue;
        }
        Input *input = &inputs[input_count++];
        input->fd = fd;
        input->state.hat_x = 0;
        input->state.hat_y = 0;
        struct input_absinfo absinfo;
        if (bit_set(absbits, ABS_HAT0X) &&
            ioctl(fd, EVIOCGABS(ABS_HAT0X), &absinfo) == 0)
            input->state.hat_x = absinfo.value;
        if (bit_set(absbits, ABS_HAT0Y) &&
            ioctl(fd, EVIOCGABS(ABS_HAT0Y), &absinfo) == 0)
            input->state.hat_y = absinfo.value;
    }
    closedir(dir);
    return input_count;
}

static void close_inputs(void)
{
    for (int i = 0; i < input_count; ++i)
        if (inputs[i].fd >= 0) close(inputs[i].fd);
    input_count = 0;
}

static int key_is_down(int key)
{
    unsigned long state[(KEY_MAX + 8 * sizeof(unsigned long)) /
                        (8 * sizeof(unsigned long))];
    memset(state, 0, sizeof(state));
    for (int i = 0; i < input_count; ++i) {
        if (ioctl(inputs[i].fd, EVIOCGKEY(sizeof(state)), state) == 0 &&
            bit_set(state, key)) return 1;
    }
    return 0;
}

static int boot_selector_held(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *de;
    FILE *diag = fopen("/data/baseos-input-boot.log", "a");
    int held = 0;

    if (diag) {
        fprintf(diag, "\\n=== boot input probe ===\\n");
        fflush(diag);
    }
    if (!dir) {
        if (diag) { fprintf(diag, "opendir /dev/input failed: %s\\n", strerror(errno)); fclose(diag); }
        return 0;
    }

    while ((de = readdir(dir)) != NULL) {
        char path[FIELD], name[256] = "unknown";
        unsigned long keybits[(KEY_MAX + 8 * sizeof(unsigned long)) /
                              (8 * sizeof(unsigned long))];
        unsigned long state[(KEY_MAX + 8 * sizeof(unsigned long)) /
                            (8 * sizeof(unsigned long))];
        int fd, has_west, has_z, down_west = 0, down_z = 0;

        if (strncmp(de->d_name, "event", 5) != 0) continue;
        if (snprintf(path, sizeof(path), "/dev/input/%s", de->d_name) >=
            (int)sizeof(path)) continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (diag) fprintf(diag, "%s open failed: %s\\n", path, strerror(errno));
            continue;
        }
        memset(keybits, 0, sizeof(keybits));
        memset(state, 0, sizeof(state));
        (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        int caps_ok = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0;
        has_west = caps_ok && bit_set(keybits, BTN_WEST);
        has_z = caps_ok && bit_set(keybits, BTN_Z);
        int state_ok = ioctl(fd, EVIOCGKEY(sizeof(state)), state) >= 0;
        if (state_ok) {
            down_west = bit_set(state, BTN_WEST);
            down_z = bit_set(state, BTN_Z);
        }
        if (diag)
            fprintf(diag, "%s name=%.120s caps=%s L1(BTN_WEST)=%d/%d R1(BTN_Z)=%d/%d\\n",
                    de->d_name, name, caps_ok ? "ok" : "error",
                    has_west, down_west, has_z, down_z);
        if ((has_west && down_west) || (has_z && down_z)) held = 1;
        close(fd);
    }
    closedir(dir);
    if (diag) { fprintf(diag, "selector_held=%d\\n", held); fclose(diag); }
    return held;
}

static uint32_t pack_channel(uint8_t c, struct fb_bitfield field)
{
    if (!field.length) return 0;
    uint32_t max = field.length >= 31 ? 0x7fffffffU :
                   ((1U << field.length) - 1U);
    return (((uint32_t)c * max + 127U) / 255U) << field.offset;
}

static uint32_t colour(uint8_t r, uint8_t g, uint8_t b)
{
    return pack_channel(r, fb_var.red) |
           pack_channel(g, fb_var.green) |
           pack_channel(b, fb_var.blue) |
           (fb_var.transp.length ? pack_channel(255, fb_var.transp) : 0);
}

static void put_pixel(int x, int y, uint32_t p)
{
    if (!fb_mem || x < 0 || y < 0 ||
        x >= (int)fb_var.xres || y >= (int)fb_var.yres) return;
    unsigned bytes = fb_var.bits_per_pixel / 8;
    if (bytes < 2 || bytes > 4) return;
    uint8_t *dst = fb_canvas + (size_t)y * fb_fix.line_length + (size_t)x * bytes;
    if (bytes == 2) *(uint16_t *)dst = (uint16_t)p;
    else if (bytes == 3) { dst[0] = p; dst[1] = p >> 8; dst[2] = p >> 16; }
    else *(uint32_t *)dst = p;
}

static void fill_rect(int x, int y, int w, int h, uint32_t p)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_var.xres) w = (int)fb_var.xres - x;
    if (y + h > (int)fb_var.yres) h = (int)fb_var.yres - y;
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) put_pixel(xx, yy, p);
}

static void draw_text(int x, int baseline, const char *s, int px, uint32_t fg)
{
    if (!ft_face || !s) return;
    FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)px);
    int pen = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 0x20 || *p > 0x7e) continue;
        if (FT_Load_Char(ft_face, *p, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
            continue;
        FT_GlyphSlot glyph = ft_face->glyph;
        FT_Bitmap *bm = &glyph->bitmap;
        int gx = pen + glyph->bitmap_left;
        int gy = baseline - glyph->bitmap_top;
        for (unsigned row = 0; row < bm->rows; ++row) {
            for (unsigned col = 0; col < bm->width; ++col) {
                unsigned alpha = bm->buffer[(size_t)row * (size_t)abs(bm->pitch) + col];
                if (!alpha) continue;
                int xx = gx + (int)col, yy = gy + (int)row;
                if (xx < 0 || yy < 0 || xx >= (int)fb_var.xres ||
                    yy >= (int)fb_var.yres) continue;
                /* Alpha blend in the native channel format. */
                uint8_t *src = fb_canvas + (size_t)yy * fb_fix.line_length +
                               (size_t)xx * (fb_var.bits_per_pixel / 8);
                uint32_t under = 0;
                unsigned bytes = fb_var.bits_per_pixel / 8;
                if (bytes == 2) under = *(uint16_t *)src;
                else if (bytes == 3) under = src[0] | ((uint32_t)src[1] << 8) |
                                                ((uint32_t)src[2] << 16);
                else if (bytes == 4) under = *(uint32_t *)src;
                uint8_t ur = fb_var.red.length ?
                    (((under >> fb_var.red.offset) & ((1U << fb_var.red.length)-1U)) * 255U /
                     ((1U << fb_var.red.length)-1U)) : 0;
                uint8_t ug = fb_var.green.length ?
                    (((under >> fb_var.green.offset) & ((1U << fb_var.green.length)-1U)) * 255U /
                     ((1U << fb_var.green.length)-1U)) : 0;
                uint8_t ub = fb_var.blue.length ?
                    (((under >> fb_var.blue.offset) & ((1U << fb_var.blue.length)-1U)) * 255U /
                     ((1U << fb_var.blue.length)-1U)) : 0;
                /* Recover the requested RGB from the packed colour. */
                uint8_t fr = fb_var.red.length ?
                    (((fg >> fb_var.red.offset) & ((1U << fb_var.red.length)-1U)) * 255U /
                     ((1U << fb_var.red.length)-1U)) : 0;
                uint8_t fgreen = fb_var.green.length ?
                    (((fg >> fb_var.green.offset) & ((1U << fb_var.green.length)-1U)) * 255U /
                     ((1U << fb_var.green.length)-1U)) : 0;
                uint8_t fb = fb_var.blue.length ?
                    (((fg >> fb_var.blue.offset) & ((1U << fb_var.blue.length)-1U)) * 255U /
                     ((1U << fb_var.blue.length)-1U)) : 0;
                unsigned a = alpha;
                uint8_t nr = (fr * a + ur * (255U - a)) / 255U;
                uint8_t ng = (fgreen * a + ug * (255U - a)) / 255U;
                uint8_t nb = (fb * a + ub * (255U - a)) / 255U;
                put_pixel(xx, yy, colour(nr, ng, nb));
            }
        }
        pen += glyph->advance.x >> 6;
    }
}

/* Advance width of a string in the loaded face. Only printable ASCII has
 * glyphs in boot.ttf, so anything else is skipped here exactly as draw_text
 * skips it while drawing; measuring and drawing must agree or a name is cut
 * at the wrong place. */
static int text_width(const char *s, int px)
{
    int width = 0;
    if (!ft_face || !s) return 0;
    FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)px);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 0x20 || *p > 0x7e) continue;
        if (!FT_Load_Char(ft_face, *p, FT_LOAD_DEFAULT))
            width += (int)(ft_face->glyph->advance.x >> 6);
    }
    return width;
}

/* Draw one line clipped to max_width. A clipped line ends in "..." so a long
 * profile or package id reads as truncated instead of as a different name. */
static void draw_line(int x, int baseline, const char *s, int px,
                      uint32_t fg, int max_width)
{
    char tmp[220];
    size_t len = strlen(s);
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, s, len);
    tmp[len] = 0;
    if (ft_face && text_width(tmp, px) > max_width) {
        int ell = text_width("...", px);
        while (len > 0) {
            tmp[--len] = 0;
            if (text_width(tmp, px) + ell <= max_width) break;
        }
        if (len + 3 < sizeof(tmp)) memcpy(tmp + len, "...", 4);
    }
    draw_text(x, baseline, tmp, px, fg);
}

static void read_field(char **fields, int n, int index, char *dst, size_t cap)
{
    if (index >= n || !fields[index]) { dst[0] = 0; return; }
    snprintf(dst, cap, "%s", fields[index]);
}

static void load_current_default(void)
{
    FILE *fp = fopen("/run/baseos.conf", "r");
    char line[256];
    snprintf(current_default_id, sizeof(current_default_id), "auto");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "frontend_id=", 12) != 0) continue;
        char *value = line + 12;
        value[strcspn(value, "\\r\\n")] = 0;
        if (*value && strlen(value) < sizeof(current_default_id))
            snprintf(current_default_id, sizeof(current_default_id), "%s", value);
    }
    fclose(fp);
}

static int entry_is_default(const Frontend *entry)
{
    return strcmp(current_default_id, "auto") != 0 &&
           strcmp(current_default_id, entry->id) == 0;
}

static int load_catalog(void)
{
    load_current_default();
    FILE *fp = fopen(CATALOG, "r");
    char line[1800];
    entry_count = 0;
    if (!fp) return 0;
    while (entry_count < MAX_ENTRIES && fgets(line, sizeof(line), fp)) {
        char *fields[10] = {0}, *save = NULL, *p;
        int n = 0;
        line[strcspn(line, "\r\n")] = 0;
        p = strtok_r(line, "|", &save);
        while (p && n < 10) { fields[n++] = p; p = strtok_r(NULL, "|", &save); }
        if (n != 10) continue;
        Frontend *e = &entries[entry_count];
        read_field(fields,n,0,e->id,sizeof(e->id));
        read_field(fields,n,1,e->label,sizeof(e->label));
        read_field(fields,n,2,e->package,sizeof(e->package));
        read_field(fields,n,3,e->profile,sizeof(e->profile));
        read_field(fields,n,4,e->root,sizeof(e->root));
        read_field(fields,n,5,e->package_dir,sizeof(e->package_dir));
        read_field(fields,n,6,e->profile_config,sizeof(e->profile_config));
        read_field(fields,n,8,e->kind,sizeof(e->kind));
        read_field(fields,n,9,e->status,sizeof(e->status));
        e->priority = atoi(fields[7]);
        if (!e->id[0] || !e->label[0]) continue;
        ++entry_count;
    }
    fclose(fp);
    if (selected > entry_count) selected = entry_count;
    if (selected < 0) selected = 0;
    return entry_count;
}

static int run_manager(const char *action, const char *id)
{
    pid_t pid = fork();
    int status = 0;
    if (pid < 0) return -1;
    if (pid == 0) {
        if (id) execl(MANAGER, "baseos-frontend-manager", action, id, (char *)NULL);
        else execl(MANAGER, "baseos-frontend-manager", action, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int write_selection(const char *id)
{
    int fd = open(SELECTION, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t n = strlen(id);
    int ok = write(fd, id, n) == (ssize_t)n && write(fd, "\n", 1) == 1;
    fsync(fd);
    close(fd);
    return ok ? 0 : -1;
}

static void reload_catalog(void)
{
    run_manager("catalog", NULL);
    load_catalog();
}

static void set_status(const char *message)
{
    snprintf(status_text, sizeof(status_text), "%s", message ? message : "");
}


static int framebuffer_open(void)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return -1;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fb_fix) < 0) return -1;
    if (fb_var.bits_per_pixel != 16 && fb_var.bits_per_pixel != 24 &&
        fb_var.bits_per_pixel != 32) return -1;
    fb_size = fb_fix.smem_len;
    if (!fb_size) fb_size = (size_t)fb_fix.line_length * fb_var.yres_virtual;
    fb_frame_size = (size_t)fb_fix.line_length * fb_var.yres;
    if (!fb_frame_size || fb_frame_size > fb_size) return -1;
    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) { fb_mem = NULL; return -1; }
    fb_canvas = calloc(1, fb_frame_size);
    if (!fb_canvas) return -1;
    if (FT_Init_FreeType(&ft_lib) != 0) return -1;
    if (FT_New_Face(ft_lib, FONT, 0, &ft_face) != 0) return -1;
    return 0;
}

static void draw_header(const char *title, const char *subtitle)
{
    int w = (int)fb_var.xres, h = (int)fb_var.yres;
    fill_rect(0, 0, w, h, colour(9, 15, 24));
    fill_rect(0, 0, w, 70, colour(18, 35, 52));
    fill_rect(0, 69, w, 2, colour(41, 190, 165));
    draw_text(28, 37, "BASEOS / RG34XX", 23, colour(78, 220, 193));
    draw_text(28, 59, title, 19, colour(245, 248, 252));
    /* Right-aligned status line. It used to start at a fixed x with a fixed
     * width, which clipped long package ids mid-glyph; measure first so the
     * text sits against the margin and ends in "..." when it cannot fit. */
    int sub_max = w - 380;
    if (sub_max < 200) sub_max = 200;
    int sub_w = text_width(subtitle, 15);
    int sub_x = w - 28 - (sub_w < sub_max ? sub_w : sub_max);
    draw_line(sub_x, 43, subtitle, 15, colour(173, 190, 205), sub_max);
}

static void draw_footer(const char *text)
{
    int h = (int)fb_var.yres, w = (int)fb_var.xres;
    fill_rect(0, h - 58, w, 58, colour(18, 35, 52));
    draw_line(24, h - 37, status_text, 12, colour(123, 192, 180), w - 48);
    draw_line(24, h - 15, text, 12, colour(203, 215, 224), w - 48);
}

static void render_selector(void)
{
    int w = (int)fb_var.xres;
    int row_h = 50;
    char default_label[96];
    snprintf(default_label, sizeof(default_label), "Default: %s", current_default_id);
    draw_header("Frontend selector", default_label);
    draw_text(28, 105, "Installed frontend profiles", 17, colour(157, 177, 194));
    char count[80];
    snprintf(count, sizeof(count), "%d profile(s)", entry_count);
    draw_text(w - 180, 105, count, 14, colour(157, 177, 194));
    int visible = (int)fb_var.yres > 430 ? 5 : 4;
    if (entry_count > visible) {
        if (selected < offset) offset = selected;
        if (selected >= offset + visible) offset = selected - visible + 1;
    } else offset = 0;
    for (int i = 0; i < visible && offset + i < entry_count; ++i) {
        int index = offset + i, y = 125 + i * row_h;
        Frontend *e = &entries[index];
        int is_selected = index == selected;
        fill_rect(20, y, w - 40, 42,
                  is_selected ? colour(31, 69, 78) : colour(17, 27, 39));
        if (is_selected) fill_rect(20, y, 5, 42, colour(63, 220, 183));
        char label[220], meta[220];
        snprintf(label, sizeof(label), "%s", e->label);
        snprintf(meta, sizeof(meta), "%s / %s", e->package, e->profile);
        draw_line(38, y + 21, label, 17,
                  strcmp(e->status,"disabled")==0 ? colour(145,155,165) :
                  is_selected ? colour(255,255,255) : colour(216,226,235), w - 260);
        draw_line(38, y + 37, meta, 12, colour(145, 165, 180), w - 250);
        const char *tag = strcmp(e->status,"disabled")==0 ? "[DISABLED]" :
                          entry_is_default(e) ? "[DEFAULT]" :
                          (strcmp(e->kind,"managed")==0 ? "[MANAGED]" : "[COMPAT]");
        draw_text(w - 160, y + 26, tag, 11,
                  strcmp(e->status,"disabled")==0 ? colour(236,164,108) :
                  entry_is_default(e) ? colour(243,194,104) : colour(104,195,173));
    }
    int diag_y = 125 + visible * row_h + 3;
    fill_rect(20, diag_y, w - 40, 39,
              selected == entry_count ? colour(31,69,78) : colour(17,27,39));
    draw_text(38, diag_y + 25, "BaseOS diagnostics", 16,
              selected == entry_count ? colour(255,255,255) : colour(192,208,220));
    if (entry_count == 0) {
        draw_text(30, 170, "No compatible frontend found.", 18, colour(240,190,112));
        draw_text(30, 198, "Copy an intact frontend folder into Frontends/.", 15, colour(185,200,212));
    }
    draw_footer("UP/DOWN Move   A Launch once   START Set default   SELECT Enable/disable   X Uninstall   B Exit");
}

static void render_confirm(void)
{
    int w = (int)fb_var.xres, h = (int)fb_var.yres;
    draw_header("Confirm removal", "This cannot be undone");
    Frontend *e = selected < entry_count ? &entries[selected] : NULL;
    if (e) {
        draw_text(30, 145, confirm_action == CONFIRM_PROFILE ?
                  "Remove this configuration profile?" : "Delete this frontend package?",
                  20, colour(245,248,252));
        draw_line(30, 185, e->label, 19, colour(240,240,240), w - 60);
        draw_line(30, 217, e->package, 15, colour(170,190,205), w - 60);
        draw_line(30, 270, confirm_action == CONFIRM_PROFILE ?
                  "The shared program and other profiles will remain." :
                  "All profiles and this program folder will be removed.",
                  15, colour(215,225,235), w - 60);
        draw_text(30, 300, "Games, Saves and private instance data are preserved.",
                  14, colour(100,210,180));
    }
    draw_footer("A Confirm   B Cancel");
    (void)h;
}

static void render_diagnostics(void)
{
    struct utsname u;
    int w = (int)fb_var.xres;
    char line[160];
    draw_header("System diagnostics", "BaseOS / RG34XX");
    draw_text(28, 112, "Device", 14, colour(123,192,180));
    draw_text(190, 112, "Anbernic RG34XX / Allwinner H700", 15, colour(236,242,247));
    if (uname(&u) == 0) {
        draw_text(28, 151, "Kernel", 14, colour(123,192,180));
        draw_line(190, 151, u.release, 14, colour(236,242,247), w - 215);
    }
    draw_text(28, 190, "Frontend catalog", 14, colour(123,192,180));
    snprintf(line, sizeof(line), "%d compatible profile(s)", entry_count);
    draw_text(190, 190, line, 14, colour(236,242,247));

    int mounted = 0;
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char row[512];
        while (fgets(row, sizeof(row), fp))
            if (strstr(row, " /mnt/sdcard ")) { mounted = 1; break; }
        fclose(fp);
    }
    draw_text(28, 229, "Storage", 14, colour(123,192,180));
    draw_text(190, 229, mounted ? "/mnt/sdcard mounted" : "No user-data card mounted",
              14, colour(236,242,247));

    fp = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");
    char battery[32] = "unknown";
    if (fp) {
        if (fgets(battery, sizeof(battery), fp)) battery[strcspn(battery, "\r\n")] = 0;
        fclose(fp);
    }
    draw_text(28, 268, "Battery", 14, colour(123,192,180));
    snprintf(line, sizeof(line), "%s%%", battery);
    draw_text(190, 268, line, 14, colour(236,242,247));

    draw_text(28, 307, "USB Mass Storage", 14, colour(123,192,180));
    draw_text(190, 307, "Hold MENU while powering on", 14, colour(236,242,247));
    draw_text(28, 346, "Boot log", 14, colour(123,192,180));
    draw_line(190, 346, "/mnt/sdcard/baseos-boot.log", 14,
              colour(236,242,247), w - 215);
    draw_footer("A Button test   B Frontend selector   START Refresh");
}

static const char *input_key_name(int key)
{
    if (h700_keys) {
        /* Labels describe the printed button, not the kernel code name. */
        switch (key) {
        case BTN_WEST: return "L1";
        case BTN_Z: return "R1";
        case BTN_SELECT: return "L2";
        case BTN_START: return "R2";
        case BTN_TL: return "Select";
        case BTN_TR: return "Start";
        case BTN_TL2: return "Menu";
        case BTN_TR2: return "Extra trigger";
        case BTN_SOUTH: return "A / South";
        case BTN_EAST: return "B / East";
        case BTN_C: return "X";
        case BTN_NORTH: return "Y";
        case BTN_MODE: return "Menu / Mode";
        case KEY_UP: case BTN_DPAD_UP: return "D-pad Up";
        case KEY_DOWN: case BTN_DPAD_DOWN: return "D-pad Down";
        case KEY_LEFT: case BTN_DPAD_LEFT: return "D-pad Left";
        case KEY_RIGHT: case BTN_DPAD_RIGHT: return "D-pad Right";
        default: return "Other input";
        }
    }
    switch (key) {
    case BTN_WEST: return "L1";
    case BTN_Z: return "R1";
    case BTN_TL: return "Select";
    case BTN_TR: return "Start";
    case BTN_TL2: return "L2";
    case BTN_TR2: return "R2";
    case BTN_SOUTH: return "A / South";
    case BTN_EAST: return "B / East";
    case BTN_C: return "X";
    case BTN_NORTH: return "Y";
    case BTN_START: return "Start";
    case BTN_SELECT: return "Select";
    case BTN_MODE: return "Menu / Mode";
    case KEY_UP: case BTN_DPAD_UP: return "D-pad Up";
    case KEY_DOWN: case BTN_DPAD_DOWN: return "D-pad Down";
    case KEY_LEFT: case BTN_DPAD_LEFT: return "D-pad Left";
    case KEY_RIGHT: case BTN_DPAD_RIGHT: return "D-pad Right";
    default: return "Other input";
    }
}

static void draw_disc(int cx, int cy, int radius, uint32_t fill)
{
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x)
            if (x * x + y * y <= radius * radius)
                put_pixel(cx + x, cy + y, fill);
}

static void draw_button_tile(int cx, int cy, int width, int height,
                             const char *label, int active)
{
    uint32_t bg = active ? colour(35, 174, 150) : colour(38, 54, 70);
    uint32_t fg = active ? colour(255, 255, 255) : colour(195, 210, 222);
    fill_rect(cx - width / 2, cy - height / 2, width, height, bg);
    int tw = text_width(label, 12);
    draw_text(cx - tw / 2, cy + 4, label, 12, fg);
}

static void draw_stick(int cx, int cy, const char *label, int active)
{
    uint32_t rim = active ? colour(50, 220, 180) : colour(91, 112, 130);
    draw_disc(cx, cy, 27, rim);
    draw_disc(cx, cy, 21, colour(20, 32, 46));
    draw_disc(cx, cy, 8, active ? colour(50, 220, 180) : colour(125, 145, 160));
    int tw = text_width(label, 12);
    draw_text(cx - tw / 2, cy + 48, label, 12,
              active ? colour(110, 245, 212) : colour(195, 210, 222));
}

static void render_button_test(void)
{
    int w = (int)fb_var.xres;
    int cx = w / 2;
    int dpad_x = w * 22 / 100;
    int face_x = w * 78 / 100;
    int left_stick_x = w * 40 / 100;
    int right_stick_x = w * 60 / 100;
    int cy = 258;
    int stick_y = 337;
    draw_header("Button test", "Handheld control map");
    draw_text(28, 99, "Press each control; the matching key lights up.", 14,
              colour(225,235,243));

    /* Shoulder controls and the device-specific H700 labels. */
    draw_button_tile(w * 15 / 100, 137, 68, 27, "L2", last_tested_key == BTN_SELECT);
    draw_button_tile(w * 30 / 100, 137, 68, 27, "L1", last_tested_key == BTN_WEST);
    draw_button_tile(w * 70 / 100, 137, 68, 27, "R1", last_tested_key == BTN_Z);
    draw_button_tile(w * 85 / 100, 137, 68, 27, "R2", last_tested_key == BTN_START);

    /* Select / Menu / Start cluster. */
    draw_button_tile(cx - 76, 183, 62, 25, "SELECT", last_tested_key == BTN_TL);
    draw_button_tile(cx, 183, 62, 25, "MENU", last_tested_key == BTN_TL2);
    draw_button_tile(cx + 76, 183, 62, 25, "START", last_tested_key == BTN_TR);

    /* Four-way D-pad. */
    draw_button_tile(dpad_x, cy - 30, 36, 27, "UP",
                     last_tested_key == KEY_UP);
    draw_button_tile(dpad_x, cy + 30, 36, 27, "DOWN",
                     last_tested_key == KEY_DOWN);
    draw_button_tile(dpad_x - 34, cy, 36, 27, "LEFT",
                     last_tested_key == KEY_LEFT);
    draw_button_tile(dpad_x + 34, cy, 36, 27, "RIGHT",
                     last_tested_key == KEY_RIGHT);

    /* Face buttons. */
    draw_button_tile(face_x, cy - 34, 32, 27, "Y", last_tested_key == BTN_NORTH);
    draw_button_tile(face_x - 36, cy, 32, 27, "X", last_tested_key == BTN_C);
    draw_button_tile(face_x + 36, cy, 32, 27, "B", last_tested_key == BTN_EAST);
    draw_button_tile(face_x, cy + 34, 32, 27, "A", last_tested_key == BTN_SOUTH);

    /* Analog sticks and their push buttons are shown even on models without
     * physical sticks, so the same page documents the full H700 control set. */
    draw_stick(left_stick_x, stick_y, "L3 / LEFT STICK",
               last_tested_key == BTN_THUMBL ||
               last_tested_key == TEST_AXIS_LX || last_tested_key == TEST_AXIS_LY);
    draw_stick(right_stick_x, stick_y, "R3 / RIGHT STICK",
               last_tested_key == BTN_THUMBR ||
               last_tested_key == TEST_AXIS_RX || last_tested_key == TEST_AXIS_RY);

    const char *name = last_tested_key < 0 ? "Waiting for input" :
                       input_key_name(last_tested_key);
    draw_line(28, 402, name, 13, colour(110, 230, 200), w - 56);
    draw_footer("Hold MENU + START together to return to diagnostics");
}

static void render(void)
{
    if (page_diagnostics == 1) render_diagnostics();
    else if (page_diagnostics == 2) render_button_test();
    else if (confirm_mode) render_confirm();
    else render_selector();
    /* Compose off-screen, then update the visible framebuffer in one copy. */
    if (fb_mem && fb_canvas && fb_frame_size)
        memcpy(fb_mem, fb_canvas, fb_frame_size);
}

static int next_key(int timeout_ms)
{
    struct pollfd pfd[MAX_INPUTS];
    if (!input_count && !open_inputs()) { usleep(200000); return -1; }
    for (int i = 0; i < input_count; ++i) {
        pfd[i].fd = inputs[i].fd;
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
    }
    int n = poll(pfd, (nfds_t)input_count, timeout_ms);
    if (n <= 0) return -1;
    for (int i = 0; i < input_count; ++i) {
        if (!(pfd[i].revents & POLLIN)) continue;
        struct input_event ev;
        while (read(inputs[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_ABS) {
                switch (ev.code) {
                case ABS_X: return TEST_AXIS_LX;
                case ABS_Y: return TEST_AXIS_LY;
                case ABS_RX: return TEST_AXIS_RX;
                case ABS_RY: return TEST_AXIS_RY;
                default: break;
                }
            }
            int key = baseos_map_input_event(&inputs[i].state,
                                             ev.type, ev.code, ev.value);
            if (key >= 0) return key;
        }
    }
    return -1;
}

static void handle_selector_key(int key)
{
    if (confirm_mode) {
        if (key == BTN_EAST) {
            confirm_mode = CONFIRM_NONE;
            set_status("Cancelled");
            return;
        }
        if (key == BTN_SOUTH && selected < entry_count) {
            Frontend *e = &entries[selected];
            const char *action = confirm_action == CONFIRM_PROFILE ? "uninstall" :
                                 "uninstall-package";
            if (run_manager(action, e->id) == 0)
                set_status("Removed successfully");
            else
                set_status("Removal failed");
            confirm_mode = CONFIRM_NONE;
            reload_catalog();
        }
        return;
    }
    if (key == KEY_UP || key == BTN_DPAD_UP) {
        if (selected > 0) --selected;
    } else if (key == KEY_DOWN || key == BTN_DPAD_DOWN) {
        if (selected < entry_count) ++selected;
    } else if (key == BTN_EAST) {
        running = 0;
    } else if (key == BTN_SOUTH) {
        if (selected == entry_count) {
            page_diagnostics = 1;
            set_status("Diagnostics");
            return;
        }
        if (selected < entry_count && strcmp(entries[selected].status,"disabled") != 0) {
            if (write_selection(entries[selected].id) == 0)
                running = 0;
            else
                set_status("Unable to write one-boot selection");
        }
    } else if (baseos_is_start_key(key)) {
        if (selected < entry_count && strcmp(entries[selected].status,"disabled") != 0) {
            if (run_manager("set-default", entries[selected].id) == 0 &&
                write_selection(entries[selected].id) == 0) {
                set_status("Default saved");
                running = 0;
            } else {
                set_status("Could not save default profile");
            }
        }
    } else if (baseos_is_select_key(key)) {
        if (selected < entry_count) {
            const char *action = strcmp(entries[selected].status,"disabled") == 0 ?
                                 "enable-package" : "disable-package";
            if (run_manager(action, entries[selected].id) == 0) {
                set_status("Frontend status updated");
                reload_catalog();
            } else {
                set_status("Could not change frontend status");
            }
        }
    } else if (key == BTN_WEST) {
        if (selected < entry_count && strcmp(entries[selected].kind,"managed") == 0) {
            confirm_action = CONFIRM_PROFILE;
            confirm_mode = 1;
        }
    } else if (key == BTN_NORTH) {
        if (selected < entry_count) {
            confirm_action = CONFIRM_PACKAGE;
            confirm_mode = 1;
        }
    }
}

static void handle_diagnostics_key(int key)
{
    if (page_diagnostics == 1) {
        if (key == BTN_SOUTH) {
            page_diagnostics = 2;
            last_tested_key = -1;
            set_status("Press buttons to test");
        } else if (key == BTN_EAST) {
            page_diagnostics = 0;
            set_status("Frontend selector");
        } else if (baseos_is_start_key(key)) {
            reload_catalog();
            set_status("Diagnostics refreshed");
        }
    } else if (page_diagnostics == 2) {
        if (key == BTN_EAST || key == BTN_SOUTH) {
            page_diagnostics = 1;
            set_status("Diagnostics");
        } else {
            last_tested_key = key;
        }
    }
}

static void detect_key_layout(void)
{
    FILE *fp = fopen("/etc/baseos-release", "r");
    char line[160];
    if (!fp) return;  /* every current target is H700; keep the vendor layout */
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "BASEOS_PLATFORM=", 16) != 0) continue;
        h700_keys = strcmp(line + 16, "h700") == 0;
        break;
    }
    fclose(fp);
}

static int run_ui(void)
{
    if (framebuffer_open() != 0) return 2;
    detect_key_layout();
    signal(SIGINT, stop_ui);
    signal(SIGTERM, stop_ui);
    open_inputs();
    load_catalog();
    load_catalog();
    render();
    while (running) {
        int key = next_key(500);
        if (key < 0) continue;
        if (page_diagnostics) handle_diagnostics_key(key);
        else handle_selector_key(key);
        if (running) render();
    }
    close_inputs();
    if (ft_face) FT_Done_Face(ft_face);
    if (ft_lib) FT_Done_FreeType(ft_lib);
    free(fb_canvas);
    if (fb_mem) munmap(fb_mem, fb_size);
    if (fb_fd >= 0) close(fb_fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--boot-held") == 0)
        return boot_selector_held() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--diagnostics") == 0) {
        page_diagnostics = 1;
        return run_ui();
    }
    if (argc == 2 && strcmp(argv[1], "--selector") == 0)
        return run_ui();
    fprintf(stderr, "usage: %s [--selector|--diagnostics|--boot-held]\n", argv[0]);
    return 2;
}

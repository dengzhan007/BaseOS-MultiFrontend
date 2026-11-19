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
#define CJK_FONT "/usr/share/baseos/boot-cjk.ttc"
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
static FT_Face ft_cjk_face;
static int confirm_mode;
static int confirm_action;
static int page_diagnostics;
static int diagnostics_selection;
static int ui_chinese = 1;
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

static const char *ui_translate(const char *s);
static void set_status(const char *message);

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

static unsigned int utf8_next(const unsigned char **cursor)
{
    const unsigned char *p = *cursor;
    unsigned int cp;
    if (p[0] < 0x80) {
        *cursor = p + 1;
        return p[0];
    }
    if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
        cp = ((unsigned int)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        *cursor = p + 2;
        return cp;
    }
    if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 &&
        (p[2] & 0xc0) == 0x80) {
        cp = ((unsigned int)(p[0] & 0x0f) << 12) |
             ((unsigned int)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *cursor = p + 3;
        return cp;
    }
    if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 &&
        (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        cp = ((unsigned int)(p[0] & 0x07) << 18) |
             ((unsigned int)(p[1] & 0x3f) << 12) |
             ((unsigned int)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        *cursor = p + 4;
        return cp;
    }
    *cursor = p + 1;
    return '?';
}

static FT_Face face_for_codepoint(unsigned int cp)
{
    if (cp > 0x7f && ft_cjk_face) return ft_cjk_face;
    return ft_face;
}

static void draw_text(int x, int baseline, const char *s, int px, uint32_t fg)
{
    if (!ft_face || !s) return;
    s = ui_translate(s);
    int pen = x;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned int cp = utf8_next(&p);
        FT_Face face = face_for_codepoint(cp);
        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0 ||
            FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) {
            if (face != ft_face &&
                (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)px) != 0 ||
                 FT_Load_Char(ft_face, '?', FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)))
                continue;
            if (face == ft_face) continue;
            face = ft_face;
        }
        FT_GlyphSlot glyph = face->glyph;
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
        pen += (int)(glyph->advance.x >> 6);
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
    s = ui_translate(s);
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned int cp = utf8_next(&p);
        FT_Face face = face_for_codepoint(cp);
        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0 ||
            FT_Load_Char(face, cp, FT_LOAD_DEFAULT)) {
            if (face != ft_face &&
                FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)px) == 0 &&
                FT_Load_Char(ft_face, '?', FT_LOAD_DEFAULT) == 0)
                width += (int)(ft_face->glyph->advance.x >> 6);
        } else {
            width += (int)(face->glyph->advance.x >> 6);
        }
    }
    return width;
}

/* Draw one line clipped to max_width. A clipped line ends in "..." so a long
 * profile or package id reads as truncated instead of as a different name. */
static void draw_line(int x, int baseline, const char *s, int px,
                      uint32_t fg, int max_width)
{
    char tmp[220];
    s = ui_translate(s);
    size_t len = strlen(s);
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, s, len);
    tmp[len] = 0;
    if (ft_face && text_width(tmp, px) > max_width) {
        int ell = text_width("...", px);
        while (len > 0) {
            --len;
            while (len > 0 && ((unsigned char)tmp[len] & 0xc0) == 0x80)
                --len;
            tmp[len] = 0;
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

static int find_entry_by_id(const char *id)
{
    if (!id || !*id) return -1;
    for (int i = 0; i < entry_count; ++i)
        if (strcmp(entries[i].id, id) == 0) return i;
    return -1;
}

static int selector_item_count(void)
{
    return entry_count + 4; /* create, rename, diagnostics, language */
}

static int source_entry_index(void)
{
    return find_entry_by_id(profile_source_id);
}

static int source_supports_profiles(void)
{
    int i = source_entry_index();
    return i >= 0 &&
           (strcmp(entries[i].kind, "managed") == 0 ||
            strcmp(entries[i].kind, "compat") == 0);
}

static int load_catalog(void)
{
    char prior_selected[64] = "";
    int old_entry_count = entry_count;
    int prior_action = -1;
    if (selected >= 0 && selected < old_entry_count)
        snprintf(prior_selected, sizeof(prior_selected), "%s", entries[selected].id);
    else if (old_entry_count > 0 && selected >= old_entry_count)
        prior_action = selected - old_entry_count;

    load_current_default();
    FILE *fp = fopen(CATALOG, "r");
    char line[1800];
    entry_count = 0;
    if (fp) {
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
    }

    /* Stable partition: enabled entries remain in discovery order, followed
     * by disabled entries. Disabled frontends remain manageable but cannot
     * crowd active choices at the top of the selector. */
    Frontend ordered[MAX_ENTRIES];
    int out = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < entry_count; ++i) {
            int disabled = strcmp(entries[i].status, "disabled") == 0;
            if (disabled == pass) ordered[out++] = entries[i];
        }
    }
    if (out == entry_count)
        memcpy(entries, ordered, (size_t)entry_count * sizeof(entries[0]));

    int old_selected_index = find_entry_by_id(prior_selected);
    if (old_selected_index >= 0) selected = old_selected_index;
    else if (prior_action >= 0) selected = entry_count + prior_action;
    else if (selected < 0) selected = 0;
    if (entry_count == 0) selected = 2; /* open diagnostics when no frontend exists */
    if (selected >= selector_item_count()) selected = selector_item_count() - 1;
    if (selected < 0) selected = 0;

    if (!profile_source_id[0] && entry_count > 0)
        snprintf(profile_source_id, sizeof(profile_source_id), "%s", entries[0].id);
    if (source_entry_index() < 0 && entry_count > 0)
        snprintf(profile_source_id, sizeof(profile_source_id), "%s", entries[0].id);
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

static int run_manager_args(const char *action, const char *arg1,
                              const char *arg2, const char *arg3)
{
    pid_t pid = fork();
    int status = 0;
    if (pid < 0) return -1;
    if (pid == 0) {
        if (arg3)
            execl(MANAGER, "baseos-frontend-manager", action, arg1, arg2, arg3,
                  (char *)NULL);
        else if (arg2)
            execl(MANAGER, "baseos-frontend-manager", action, arg1, arg2,
                  (char *)NULL);
        else if (arg1)
            execl(MANAGER, "baseos-frontend-manager", action, arg1, (char *)NULL);
        else
            execl(MANAGER, "baseos-frontend-manager", action, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int read_config_value(const char *path, const char *key,
                             char *value, size_t capacity)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[256];
    size_t key_len = strlen(key);
    int found = 0;
    value[0] = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') continue;
        char *p = line + key_len + 1;
        p[strcspn(p, "\r\n")] = 0;
        snprintf(value, capacity, "%s", p);
        found = 1;
    }
    fclose(fp);
    return found ? 0 : -1;
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


typedef struct { const char *en; const char *zh; } UiTranslation;

static const UiTranslation ui_translations[] = {
    {"Frontend selector", "前端选择"},
    {"Installed frontend profiles", "已安装的前端配置"},
    {"Create profile...", "新建配置..."},
    {"Rename selected profile...", "重命名所选配置..."},
    {"BaseOS diagnostics", "BaseOS 系统诊断"},
    {"Create another independent configuration from the selected profile", "基于所选配置新建独立配置"},
    {"Select a managed or compatible profile first", "请先选择受管理或兼容前端配置"},
    {"Change the selected profile's display name", "修改所选配置的显示名称"},
    {"System, storage, battery and physical button tests", "系统、存储、电池和实体按键测试"},
    {"[DISABLED]", "[已禁用]"},
    {"[DEFAULT]", "[默认]"},
    {"[MANAGED]", "[受管理]"},
    {"[COMPAT]", "[兼容]"},
    {"[LEGACY]", "[旧式]"},
    {"No compatible frontend found. Copy one into Frontends/.", "未发现兼容前端，请将前端放入 Frontends/。"},
    {"A Open/Launch   START Default   SELECT Enable/disable   L1 Remove profile   Y Delete frontend", "A 打开/启动  START 设为默认  SELECT 启用/禁用  L1 删除配置  Y 删除前端"},
    {"Confirm removal", "确认删除"},
    {"This cannot be undone", "此操作无法撤销"},
    {"Remove this configuration profile?", "确定删除此配置？"},
    {"Delete this frontend package?", "确定删除此前端程序？"},
    {"The shared program and other profiles will remain.", "共享程序和其他配置将保留。"},
    {"All profiles and this program folder will be removed.", "所有配置及此前端程序目录都会删除。"},
    {"Games, Saves and private instance data are preserved.", "Games、Saves 和各配置的独立数据会保留。"},
    {"A Confirm   B Cancel", "A 确认  B 取消"},
    {"System diagnostics", "系统诊断"},
    {"BaseOS / RG34XX", "BaseOS / RG34XX"},
    {"Device", "设备"},
    {"Anbernic RG34XX / Allwinner H700", "Anbernic RG34XX / 全志 H700"},
    {"Kernel", "内核"},
    {"Frontend catalog", "前端列表"},
    {"Storage", "存储"},
    {"/mnt/sdcard mounted", "/mnt/sdcard 已挂载"},
    {"No user-data card mounted", "未挂载用户数据卡"},
    {"Battery", "电量"},
    {"USB Mass Storage", "USB 大容量存储"},
    {"Hold MENU while powering on", "开机时按住 MENU"},
    {"Boot log", "启动日志"},
    {"BUTTON TEST", "按键测试"},
    {"Press A to test every button, trigger and analog stick", "按 A 测试所有按键、扳机和摇杆"},
    {"Button test", "按键测试"},
    {"Handheld control map", "掌机按键布局"},
    {"Press each control; the matching key lights up.", "按下实体按键，对应位置会高亮显示。"},
    {"Waiting for input", "等待输入"},
    {"Other input", "其他输入"},
    {"L3 / Left stick click", "L3 / 左摇杆按下"},
    {"R3 / Right stick click", "R3 / 右摇杆按下"},
    {"Left stick X axis", "左摇杆 X 轴"},
    {"Left stick Y axis", "左摇杆 Y 轴"},
    {"Right stick X axis", "右摇杆 X 轴"},
    {"Right stick Y axis", "右摇杆 Y 轴"},
    {"D-pad Up", "十字键 上"},
    {"D-pad Down", "十字键 下"},
    {"D-pad Left", "十字键 左"},
    {"D-pad Right", "十字键 右"},
    {"X / West", "X / 左侧"},
    {"L1 / Select", "L1 / 选择"},
    {"R1 / Start", "R1 / 开始"},
    {"L2 / Menu", "L2 / 菜单"},
    {"Left stick X axis", "左摇杆 X 轴"},
    {"Left stick Y axis", "左摇杆 Y 轴"},
    {"Right stick X axis", "右摇杆 X 轴"},
    {"Right stick Y axis", "右摇杆 Y 轴"},
    {"Hold MENU + START together to return to diagnostics", "同时按住 MENU + START 返回诊断页面"},
    {"Create frontend profile", "新建前端配置"},
    {"Rename frontend profile", "重命名前端配置"},
    {"On-device keyboard", "设备键盘"},
    {"Profile name", "配置名称"},
    {"D-pad Move   A Select   B Backspace   START Save   CANCEL Exit", "十字键移动  A 输入  B 退格  START 保存  CANCEL 取消"},
    {"Press buttons and move sticks; MENU+START exits", "按键或移动摇杆；同时按 MENU+START 退出"},
    {"A Open button test   B Frontend selector   START Refresh", "A 打开按键测试  B 返回前端选择  START 刷新"},
    {"A Open/Launch", "A 打开/启动"},
    {"Cancelled", "已取消"},
    {"Profile removed successfully", "配置已删除"},
    {"Frontend removed successfully", "前端已删除"},
    {"Removal failed; manager rejected the operation", "删除失败：管理器拒绝了操作"},
    {"Unable to write one-boot selection", "无法写入本次启动选择"},
    {"Enable this frontend before launching", "请先启用此前端再启动"},
    {"Diagnostics", "系统诊断"},
    {"Default saved", "默认配置已保存"},
    {"Could not save default profile", "无法保存默认配置"},
    {"Frontend status updated", "前端状态已更新"},
    {"Could not change frontend status", "无法更改前端状态"},
    {"Profile name cannot be empty", "配置名称不能为空"},
    {"Could not generate a unique profile ID", "无法生成唯一的配置 ID"},
    {"Profile creation failed; check the profile name", "创建配置失败，请检查名称"},
    {"Profile created", "配置已创建"},
    {"Profile rename failed; check the profile name", "重命名失败，请检查名称"},
    {"Profile renamed", "配置已重命名"},
    {"Selected profile is no longer available", "所选配置已不可用"},
    {"Enter a name for the new profile", "请输入新配置名称"},
    {"Enter the new profile name", "请输入新的配置名称"},
    {"Rebooting into USB storage mode...", "正在重启并进入 USB 存储模式..."},
    {"Could not request USB storage mode", "无法请求 USB 存储模式"},
    {"USB storage request saved; rebooting", "USB 存储请求已保存，正在重启"},
    {"L1 English", "L1 English"},
    {"L1 中文", "L1 中文"}
};

static const char *ui_translate(const char *s)
{
    if (!s || !ui_chinese) return s;
    for (size_t i = 0; i < sizeof(ui_translations) / sizeof(ui_translations[0]); ++i)
        if (strcmp(s, ui_translations[i].en) == 0)
            return ui_translations[i].zh;
    return s;
}

static void load_ui_language(void)
{
    FILE *fp = fopen("/data/baseos-ui-language", "r");
    char value[16] = "";
    ui_chinese = 1;
    if (!fp) return;
    if (fgets(value, sizeof(value), fp) && strncmp(value, "en", 2) == 0)
        ui_chinese = 0;
    fclose(fp);
}

static void toggle_ui_language(void)
{
    const char *value;
    FILE *fp;
    ui_chinese = !ui_chinese;
    value = ui_chinese ? "zh\n" : "en\n";
    fp = fopen("/data/baseos-ui-language.tmp", "w");
    if (fp) {
        fputs(value, fp);
        if (fclose(fp) == 0)
            (void)rename("/data/baseos-ui-language.tmp", "/data/baseos-ui-language");
        else
            (void)unlink("/data/baseos-ui-language.tmp");
    }
    set_status(ui_chinese ? "Language changed to Chinese" : "Language changed to English");
}

static void request_usb_storage_mode(void)
{
    FILE *fp = fopen("/data/usb-storage-next-boot", "w");
    if (!fp) {
        set_status("Could not request USB storage mode");
        return;
    }
    fputs("1\n", fp);
    if (fclose(fp) != 0) {
        (void)unlink("/data/usb-storage-next-boot");
        set_status("Could not request USB storage mode");
        return;
    }
    sync();
    set_status("USB storage request saved; rebooting");
    (void)system("/usr/sbin/reboot");
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
    /* Chinese defaults to on; keep the Latin face for the existing visual style
     * and use WenQuanYi only for non-ASCII glyphs. The font is optional at runtime
     * so an older system still opens the selector in English-compatible mode. */
    (void)FT_New_Face(ft_lib, CJK_FONT, 0, &ft_cjk_face);
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
    draw_text(w - 142, 61, ui_chinese ? "L1 English" : "L1 中文", 11,
              colour(173,190,205));
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
    int row_h = 38;
    int total = selector_item_count();
    char default_label[96];
    snprintf(default_label, sizeof(default_label), ui_chinese ? "默认启动：%s" : "Default: %s", current_default_id);
    draw_header("Frontend selector", default_label);
    draw_text(28, 101, "Installed frontend profiles", 16, colour(157, 177, 194));
    char count[80];
    snprintf(count, sizeof(count), ui_chinese ? "%d 个配置" : "%d profile(s)", entry_count);
    draw_text(w - 150, 101, count, 13, colour(157, 177, 194));

    int visible = ((int)fb_var.yres - 150) / row_h;
    if (visible < 3) visible = 3;
    if (visible > 8) visible = 8;
    if (selected < offset) offset = selected;
    if (selected >= offset + visible) offset = selected - visible + 1;
    if (offset > total - visible) offset = total - visible;
    if (offset < 0) offset = 0;

    for (int i = 0; i < visible && offset + i < total; ++i) {
        int index = offset + i, y = 112 + i * row_h;
        int is_selected = index == selected;
        fill_rect(20, y, w - 40, 34,
                  is_selected ? colour(31, 69, 78) : colour(17, 27, 39));
        if (is_selected) fill_rect(20, y, 5, 34, colour(63, 220, 183));
        if (index < entry_count) {
            Frontend *e = &entries[index];
            char meta[220];
            snprintf(meta, sizeof(meta), "%s / %s", e->package, e->profile);
            draw_line(38, y + 17, e->label, 15,
                      strcmp(e->status,"disabled")==0 ? colour(145,155,165) :
                      is_selected ? colour(255,255,255) : colour(216,226,235), w - 260);
            draw_line(38, y + 30, meta, 10, colour(145, 165, 180), w - 250);
            const char *tag = strcmp(e->status,"disabled")==0 ? "[DISABLED]" :
                              entry_is_default(e) ? "[DEFAULT]" :
                              (strcmp(e->kind,"managed")==0 ? "[MANAGED]" :
                               strcmp(e->kind,"compat")==0 ? "[COMPAT]" : "[LEGACY]");
            draw_text(w - 158, y + 23, tag, 10,
                      strcmp(e->status,"disabled")==0 ? colour(236,164,108) :
                      entry_is_default(e) ? colour(243,194,104) : colour(104,195,173));
        } else {
            int action = index - entry_count;
            const char *label = action == 0 ? "Create profile..." :
                                action == 1 ? "Rename selected profile..." :
                                action == 2 ? "BaseOS diagnostics" :
                                (ui_chinese ? "语言：中文（按 A 切换 English）" :
                                              "Language: English (press A for 中文)");
            const char *detail = action == 0 ?
                (source_supports_profiles() ? "Create another independent configuration from the selected profile" :
                                               "Select a managed or compatible profile first") :
                action == 1 ?
                (source_supports_profiles() ? "Change the selected profile's display name" :
                                               "Select a managed or compatible profile first") :
                action == 2 ? "System, storage, battery and physical button tests" :
                (ui_chinese ? "切换诊断页和前端选择的显示语言" :
                              "Change the language on selector and diagnostics pages");
            draw_text(38, y + 16, label, 15,
                      is_selected ? colour(255,255,255) :
                      action >= 2 ? colour(192,208,220) : colour(123,220,194));
            draw_line(38, y + 29, detail, 10, colour(145,165,180), w - 75);
        }
    }
    if (entry_count == 0) {
        draw_text(30, 426 - 20, "No compatible frontend found. Copy one into Frontends/.",
                  12, colour(240,190,112));
    }
    draw_footer("A Open/Launch   START Default   SELECT Enable/disable   L1 Remove profile   Y Delete frontend");
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
    draw_text(28, 108, "Device", 14, colour(123,192,180));
    draw_text(190, 108, "Anbernic RG34XX / Allwinner H700", 15, colour(236,242,247));
    if (uname(&u) == 0) {
        draw_text(28, 143, "Kernel", 14, colour(123,192,180));
        draw_line(190, 143, u.release, 14, colour(236,242,247), w - 215);
    }
    draw_text(28, 178, "Frontend catalog", 14, colour(123,192,180));
    snprintf(line, sizeof(line), ui_chinese ? "%d 个兼容配置" : "%d compatible profile(s)", entry_count);
    draw_text(190, 178, line, 14, colour(236,242,247));

    int mounted = 0;
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char row[512];
        while (fgets(row, sizeof(row), fp))
            if (strstr(row, " /mnt/sdcard ")) { mounted = 1; break; }
        fclose(fp);
    }
    draw_text(28, 213, "Storage", 14, colour(123,192,180));
    draw_text(190, 213, mounted ? "/mnt/sdcard mounted" : "No user-data card mounted",
              14, colour(236,242,247));

    fp = fopen("/sys/class/power_supply/axp2202-battery/capacity", "r");
    char battery[32] = "unknown";
    if (fp) {
        if (fgets(battery, sizeof(battery), fp)) battery[strcspn(battery, "\r\n")] = 0;
        fclose(fp);
    }
    draw_text(28, 248, "Battery", 14, colour(123,192,180));
    snprintf(line, sizeof(line), "%s%%", battery);
    draw_text(190, 248, line, 14, colour(236,242,247));
    draw_text(28, 283, "USB Mass Storage", 14, colour(123,192,180));
    draw_line(190, 283, ui_chinese ?
              "选择下方入口，重启后进入 USB 存储模式" :
              "Select below; device restarts into USB storage mode",
              13, colour(236,242,247), w - 215);
    draw_text(28, 318, "Boot log", 14, colour(123,192,180));
    draw_line(190, 318, "/mnt/sdcard/baseos-boot.log", 14,
              colour(236,242,247), w - 215);

    const char *actions[2] = {"BUTTON TEST", "USB STORAGE MODE"};
    const char *details[2] = {
        ui_chinese ? "测试实体按键、扳机和左右摇杆" : "Test buttons, triggers and both analog sticks",
        ui_chinese ? "重启进入 USB 大容量存储；传输完成后可直接拔线" :
                     "Reboot into USB mass storage; unplug after transfers finish"
    };
    for (int i = 0; i < 2; ++i) {
        int y = 342 + i * 39;
        int active = diagnostics_selection == i;
        fill_rect(22, y, w - 44, 35,
                  active ? colour(24,77,77) : colour(17,27,39));
        if (active) fill_rect(22, y, 6, 35, colour(54,220,185));
        draw_text(42, y + 15, actions[i], 15, colour(255,255,255));
        draw_line(230, y + 23, details[i], 11, colour(195,215,225), w - 260);
    }
    draw_footer(ui_chinese ?
        "上下选择  A 确认  B 返回前端选择  L1 切换 English  START 刷新" :
        "UP/DOWN Select  A Open  B Selector  L1 Switch to 中文  START Refresh");
}

static const char *input_key_name(int key)
{
    if (h700_keys) {
        switch (key) {
        case BTN_WEST: return "L1";
        case BTN_Z: return "R1";
        case BTN_SELECT: return "L2";
        case BTN_START: return "R2";
        case BTN_TL: return "Select";
        case BTN_TR: return "Start";
        case BTN_TL2: return "Menu";
        case BTN_TR2: return "Extra trigger";
        case BTN_SOUTH: return "A";
        case BTN_EAST: return "B";
        case BTN_C: return "X";
        case BTN_NORTH: return "Y";
        case BTN_MODE: return "Mode";
        case BTN_THUMBL: return "L3 / Left stick click";
        case BTN_THUMBR: return "R3 / Right stick click";
        case TEST_AXIS_LX: return "Left stick X axis";
        case TEST_AXIS_LY: return "Left stick Y axis";
        case TEST_AXIS_RX: return "Right stick X axis";
        case TEST_AXIS_RY: return "Right stick Y axis";
        case KEY_UP: case BTN_DPAD_UP: return "D-pad Up";
        case KEY_DOWN: case BTN_DPAD_DOWN: return "D-pad Down";
        case KEY_LEFT: case BTN_DPAD_LEFT: return "D-pad Left";
        case KEY_RIGHT: case BTN_DPAD_RIGHT: return "D-pad Right";
        default: return "Other input";
        }
    }
    switch (key) {
    case BTN_WEST: return "X / West";
    case BTN_Z: return "R1";
    case BTN_TL: return "L1 / Select";
    case BTN_TR: return "R1 / Start";
    case BTN_TL2: return "L2 / Menu";
    case BTN_TR2: return "R2";
    case BTN_SOUTH: return "A";
    case BTN_EAST: return "B";
    case BTN_C: return "C";
    case BTN_NORTH: return "Y";
    case BTN_START: return "Start";
    case BTN_SELECT: return "Select";
    case BTN_MODE: return "Mode";
    case BTN_THUMBL: return "L3 / Left stick click";
    case BTN_THUMBR: return "R3 / Right stick click";
    case TEST_AXIS_LX: return "Left stick X axis";
    case TEST_AXIS_LY: return "Left stick Y axis";
    case TEST_AXIS_RX: return "Right stick X axis";
    case TEST_AXIS_RY: return "Right stick Y axis";
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

static const char *const text_keyboard_keys[46] = {
    "1","2","3","4","5","6","7","8","9","0",
    "q","w","e","r","t","y","u","i","o","p",
    "a","s","d","f","g","h","j","k","l","-",
    "z","x","c","v","b","n","m","_",".","'",
    "CAP","SPACE","BKSP","CLEAR","DONE","CANCEL"
};

static void begin_profile_text_entry(int mode)
{
    int index = source_entry_index();
    if (index < 0 || !source_supports_profiles()) {
        set_status("Select a managed or compatible profile first");
        return;
    }
    text_entry_mode = mode;
    text_entry_cursor = 0;
    text_entry_caps = 0;
    text_entry[0] = 0;
    if (mode == TEXT_ENTRY_RENAME_PROFILE) {
        if (read_config_value(entries[index].profile_config, "display_name",
                              text_entry, sizeof(text_entry)) != 0)
            snprintf(text_entry, sizeof(text_entry), "%s", entries[index].profile);
    }
    page_diagnostics = 3;
    set_status(mode == TEXT_ENTRY_CREATE_PROFILE ?
               "Enter a name for the new profile" : "Enter the new profile name");
}

static int make_profile_id(const Frontend *entry, const char *name,
                           char *id, size_t capacity)
{
    char base[23];
    size_t used = 0;
    int previous_dash = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (isalnum(*p)) {
            if (used < sizeof(base) - 1)
                base[used++] = (char)tolower(*p);
            previous_dash = 0;
        } else if (used && !previous_dash && used < sizeof(base) - 1) {
            base[used++] = '-';
            previous_dash = 1;
        }
    }
    while (used && base[used - 1] == '-') --used;
    if (!used) {
        memcpy(base, "profile", 7);
        used = 7;
    }
    base[used] = 0;

    for (int serial = 1; serial < 1000; ++serial) {
        char candidate[23], suffix[8] = "";
        size_t base_len = used;
        if (serial > 1) {
            snprintf(suffix, sizeof(suffix), "-%d", serial);
            size_t suffix_len = strlen(suffix);
            if (base_len + suffix_len > 22) base_len = 22 - suffix_len;
        }
        memcpy(candidate, base, base_len);
        candidate[base_len] = 0;
        strncat(candidate, suffix, sizeof(candidate) - strlen(candidate) - 1);
        char path[FIELD * 2];
        char profile_directory[FIELD * 2];
        if (strcmp(entry->kind, "compat") == 0) {
            if (snprintf(profile_directory, sizeof(profile_directory),
                         "%s/.baseos/compat/%s", entry->root, entry->package) >=
                (int)sizeof(profile_directory))
                return -1;
        } else {
            snprintf(profile_directory, sizeof(profile_directory), "%s",
                     entry->package_dir);
        }
        if (snprintf(path, sizeof(path), "%s/profiles/%s.conf",
                     profile_directory, candidate) >= (int)sizeof(path))
            return -1;
        if (access(path, F_OK) != 0) {
            snprintf(id, capacity, "%s", candidate);
            return 0;
        }
    }
    return -1;
}

static void finish_profile_text_entry(void)
{
    int index = source_entry_index();
    if (index < 0 || !source_supports_profiles()) {
        set_status("Selected profile is no longer available");
        return;
    }
    size_t length = strlen(text_entry);
    while (length && text_entry[length - 1] == ' ')
        text_entry[--length] = 0;
    size_t first = 0;
    while (text_entry[first] == ' ') ++first;
    if (first) memmove(text_entry, text_entry + first, strlen(text_entry + first) + 1);
    if (!text_entry[0]) {
        set_status("Profile name cannot be empty");
        return;
    }

    Frontend *entry = &entries[index];
    selected = index;
    if (text_entry_mode == TEXT_ENTRY_CREATE_PROFILE) {
        char profile_id[23], new_id[64];
        if (make_profile_id(entry, text_entry, profile_id, sizeof(profile_id)) != 0) {
            set_status("Could not generate a unique profile ID");
            return;
        }
        if (run_manager_args("create-profile", entry->id, profile_id, text_entry) != 0) {
            set_status("Profile creation failed; check the profile name");
            return;
        }
        snprintf(new_id, sizeof(new_id), "%s--%s", entry->package, profile_id);
        text_entry_mode = TEXT_ENTRY_NONE;
        page_diagnostics = 0;
        snprintf(profile_source_id, sizeof(profile_source_id), "%s", new_id);
        reload_catalog();
        int created = find_entry_by_id(new_id);
        if (created >= 0) selected = created;
        set_status("Profile created");
    } else if (text_entry_mode == TEXT_ENTRY_RENAME_PROFILE) {
        if (run_manager_args("rename-profile", entry->id, text_entry, NULL) != 0) {
            set_status("Profile rename failed; check the profile name");
            return;
        }
        text_entry_mode = TEXT_ENTRY_NONE;
        page_diagnostics = 0;
        reload_catalog();
        int renamed = find_entry_by_id(profile_source_id);
        if (renamed >= 0) selected = renamed;
        set_status("Profile renamed");
    }
}

static void handle_text_entry_key(int key)
{
    int row = text_entry_cursor / 10;
    int col = text_entry_cursor % 10;
    if (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT) {
        if (key == KEY_UP && row > 0) --row;
        else if (key == KEY_DOWN && row < 4) ++row;
        else if (key == KEY_LEFT && col > 0) --col;
        else if (key == KEY_RIGHT && col < (row == 4 ? 5 : 9)) ++col;
        if (row == 4 && col > 5) col = 5;
        text_entry_cursor = row * 10 + col;
        return;
    }
    if (key == BTN_EAST) {
        size_t len = strlen(text_entry);
        if (len) text_entry[len - 1] = 0;
        return;
    }
    if (key == BTN_TL2) {
        text_entry_mode = TEXT_ENTRY_NONE;
        page_diagnostics = 0;
        set_status("Cancelled");
        return;
    }
    if (baseos_is_start_key(key)) {
        finish_profile_text_entry();
        return;
    }
    if (key != BTN_SOUTH) return;

    const char *selected_key = text_keyboard_keys[text_entry_cursor];
    if (strcmp(selected_key, "CAP") == 0) {
        text_entry_caps = !text_entry_caps;
    } else if (strcmp(selected_key, "SPACE") == 0) {
        size_t len = strlen(text_entry);
        if (len < sizeof(text_entry) - 1) {
            text_entry[len] = ' ';
            text_entry[len + 1] = 0;
        }
    } else if (strcmp(selected_key, "BKSP") == 0) {
        size_t len = strlen(text_entry);
        if (len) text_entry[len - 1] = 0;
    } else if (strcmp(selected_key, "CLEAR") == 0) {
        text_entry[0] = 0;
    } else if (strcmp(selected_key, "DONE") == 0) {
        finish_profile_text_entry();
    } else if (strcmp(selected_key, "CANCEL") == 0) {
        text_entry_mode = TEXT_ENTRY_NONE;
        page_diagnostics = 0;
        set_status("Cancelled");
    } else {
        size_t len = strlen(text_entry);
        if (len < sizeof(text_entry) - 1) {
            char ch = selected_key[0];
            if (text_entry_caps && ch >= 'a' && ch <= 'z')
                ch = (char)toupper((unsigned char)ch);
            text_entry[len] = ch;
            text_entry[len + 1] = 0;
        }
    }
}

static void render_text_entry(void)
{
    int w = (int)fb_var.xres;
    draw_header(text_entry_mode == TEXT_ENTRY_CREATE_PROFILE ?
                "Create frontend profile" : "Rename frontend profile",
                "On-device keyboard");
    draw_text(28, 112, "Profile name", 14, colour(123,192,180));
    fill_rect(24, 126, w - 48, 42, colour(23, 39, 54));
    draw_line(38, 153, text_entry, 19, colour(255,255,255), w - 76);
    int cell_w = (w - 60) / 10;
    for (int i = 0; i < 46; ++i) {
        int row = i / 10;
        int col = i % 10;
        int cx = 30 + col * cell_w + cell_w / 2;
        int cy = 204 + row * 42;
        int width = cell_w - 4;
        if (row == 4 && col >= 4) width = cell_w - 4;
        draw_button_tile(cx, cy, width, 31, text_keyboard_keys[i],
                         i == text_entry_cursor);
    }
    draw_footer("D-pad Move   A Select   B Backspace   START Save   CANCEL Exit");
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

static void render_text_entry(void);

static void render(void)
{
    if (page_diagnostics == 1) render_diagnostics();
    else if (page_diagnostics == 2) render_button_test();
    else if (page_diagnostics == 3) render_text_entry();
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

static const char *frontend_delete_action(const Frontend *entry)
{
    if (strcmp(entry->kind, "managed") == 0) return "uninstall-package";
    if (strcmp(entry->kind, "compat") == 0) return "delete-compat";
    if (strcmp(entry->kind, "legacy-adapter") == 0) return "delete-legacy-adapter";
    return NULL;
}

static void remember_profile_source(int index)
{
    if (index >= 0 && index < entry_count)
        snprintf(profile_source_id, sizeof(profile_source_id), "%s", entries[index].id);
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
            const char *action = confirm_action == CONFIRM_PROFILE ?
                                 "uninstall" : frontend_delete_action(e);
            if (action && run_manager(action, e->id) == 0)
                set_status(confirm_action == CONFIRM_PROFILE ?
                           "Profile removed successfully" : "Frontend removed successfully");
            else
                set_status("Removal failed; manager rejected the operation");
            confirm_mode = CONFIRM_NONE;
            reload_catalog();
        }
        return;
    }

    if (key == KEY_UP || key == BTN_DPAD_UP) {
        remember_profile_source(selected);
        if (selected > 0) --selected;
        remember_profile_source(selected);
    } else if (key == KEY_DOWN || key == BTN_DPAD_DOWN) {
        remember_profile_source(selected);
        if (selected < selector_item_count() - 1) ++selected;
        remember_profile_source(selected);
    } else if (key == BTN_EAST) {
        running = 0;
    } else if (key == BTN_SOUTH) {
        if (selected < entry_count) {
            if (strcmp(entries[selected].status, "disabled") != 0) {
                if (write_selection(entries[selected].id) == 0)
                    running = 0;
                else
                    set_status("Unable to write one-boot selection");
            } else {
                set_status("Enable this frontend before launching");
            }
        } else if (selected == entry_count) {
            begin_profile_text_entry(TEXT_ENTRY_CREATE_PROFILE);
        } else if (selected == entry_count + 1) {
            begin_profile_text_entry(TEXT_ENTRY_RENAME_PROFILE);
        } else if (selected == entry_count + 2) {
            page_diagnostics = 1;
            diagnostics_selection = 0;
            set_status("Diagnostics");
        } else {
            toggle_ui_language();
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
        if (selected < entry_count && (strcmp(entries[selected].kind,"managed") == 0 ||
            strcmp(entries[selected].kind,"compat") == 0)) {
            remember_profile_source(selected);
            confirm_action = CONFIRM_PROFILE;
            confirm_mode = 1;
        }
    } else if (key == BTN_NORTH) {
        if (selected < entry_count && frontend_delete_action(&entries[selected])) {
            remember_profile_source(selected);
            confirm_action = CONFIRM_PACKAGE;
            confirm_mode = 1;
        }
    }
}

static void handle_text_entry_key(int key);

static void handle_diagnostics_key(int key)
{
    if (page_diagnostics == 1) {
        if (key == KEY_UP || key == BTN_DPAD_UP || key == KEY_DOWN || key == BTN_DPAD_DOWN) {
            diagnostics_selection = 1 - diagnostics_selection;
        } else if (key == BTN_WEST) {
            toggle_ui_language();
        } else if (key == BTN_SOUTH) {
            if (diagnostics_selection == 0) {
                page_diagnostics = 2;
                last_tested_key = -1;
                set_status("Press buttons and move sticks; MENU+START exits");
            } else {
                request_usb_storage_mode();
            }
        } else if (key == BTN_EAST) {
            page_diagnostics = 0;
            set_status("Frontend selector");
        } else if (baseos_is_start_key(key)) {
            reload_catalog();
            set_status("Diagnostics refreshed");
        }
    } else if (page_diagnostics == 2) {
        /* A and B are testable inputs. Exit only through the deliberate
         * MENU + START chord so neither face button is consumed as Back. */
        if (baseos_button_test_exit_chord(key_is_down(BTN_TL2),
                                          key_is_down(h700_keys ? BTN_TR : BTN_START))) {
            page_diagnostics = 1;
            last_tested_key = -1;
            set_status("Diagnostics");
        } else {
            last_tested_key = key;
        }
    } else if (page_diagnostics == 3) {
        handle_text_entry_key(key);
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
    load_ui_language();
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
    if (ft_cjk_face) FT_Done_Face(ft_cjk_face);
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
        diagnostics_selection = 0;
        return run_ui();
    }
    if (argc == 2 && strcmp(argv[1], "--selector") == 0)
        return run_ui();
    fprintf(stderr, "usage: %s [--selector|--diagnostics|--boot-held]\n", argv[0]);
    return 2;
}

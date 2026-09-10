#define _GNU_SOURCE
#include <cairo/cairo.h>
#include <cjson/cJSON.h>
#include <math.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <dirent.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <linux/input-event-codes.h>
#include <libudev.h>
#include <pixman.h>
#include <poll.h>
#include <pthread.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-util.h>

#include "menu.h"
#include "defaults.h"
#include "rtconfig.h"
#include "style.h"
#include "tray.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

#ifndef MANGOBAR_VERSION
#define MANGOBAR_VERSION "unknown"
#endif

static void print_usage(const char *prog) {
  printf("Usage: %s [options]\n", prog);
  printf("\n");
  printf("A Wayland status bar for mangowm.\n");
  printf("\n");
  printf("Options:\n");
  printf("  -h            Show this help message and exit.\n");
  printf("  -v            Show version information and exit.\n");
  printf("  -c <path>     Use the given configuration file (JSONC).\n");
  printf("  -s <path>     Use the given CSS style file.\n");
}

// Default colors (overridable by style.css)
static const uint32_t active_fg_color_hex = 0x000000FF;
static const uint32_t active_bg_color_hex = 0x8BAA9BFF;
static const uint32_t occupied_fg_color_hex = 0xc3b695FF;
static const uint32_t occupied_bg_color_hex = 0x201B14FF;
static const uint32_t inactive_fg_color_hex = 0xC68A93FF;
static const uint32_t inactive_bg_color_hex = 0x201B14FF;
static const uint32_t urgent_fg_color_hex = 0x201B14FF;
static const uint32_t urgent_bg_color_hex = 0xDBD0C6FF;
static const uint32_t empty_fg_color_hex = 0xC68A93FF;
static const uint32_t empty_bg_color_hex = 0x201B14FF;
static const uint32_t layout_fg_color_hex = 0xC68A93FF;
static const uint32_t layout_bg_color_hex = 0x201B14FF;
static const uint32_t title_fg_color_hex = 0xC68A93FF;
static const uint32_t title_bg_color_hex = 0x201B14FF;
static const uint32_t cpu_fg_color_hex = 0xC68A93FF;
static const uint32_t cpu_bg_color_hex = 0x201B14FF;
static const uint32_t mem_fg_color_hex = 0xC68A93FF;
static const uint32_t mem_bg_color_hex = 0x201B14FF;
static const uint32_t brightness_fg_color_hex = 0xC68A93FF;
static const uint32_t brightness_bg_color_hex = 0x201B14FF;
static const uint32_t volume_fg_color_hex = 0xC68A93FF;
static const uint32_t volume_bg_color_hex = 0x201B14FF;
static const uint32_t clock_fg_color_hex = 0xC68A93FF;
static const uint32_t clock_bg_color_hex = 0x201B14FF;
static const uint32_t keymode_fg_color_hex = 0xC68A93FF;
static const uint32_t keymode_bg_color_hex = 0x201B14FF;
static const uint32_t keyboardlayout_fg_color_hex = 0xC68A93FF;
static const uint32_t keyboardlayout_bg_color_hex = 0x201B14FF;
static const uint32_t hide_clients_fg_color_hex = 0xC68A93FF;
static const uint32_t hide_clients_bg_color_hex = 0x201B14FF;
static const uint32_t battery_fg_color_hex = 0xC68A93FF;
static const uint32_t battery_bg_color_hex = 0x201B14FF;
static const uint32_t tray_fg_color_hex = 0xFFFFFFFF;
static const uint32_t tray_bg_color_hex = 0x201B14FF;
static const uint32_t overview_fg_color_hex = 0x111012FF;
static const uint32_t overview_bg_color_hex = 0x718b80FF;
static const uint32_t separator_fg_color_hex = 0xC68A93FF;
static const uint32_t separator_bg_color_hex = 0x201B14FF;
static const uint32_t middle_bg_color_hex = 0x201B14FF;
static const uint32_t middle_bg_sel_color_hex = 0x201B14FF;

static uint32_t utf8_decode(uint32_t *state, uint32_t *codep, uint8_t byte) {
  static const uint8_t len_tab[] = {0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 1, 1, 2, 3};
  if (*state == 0) {
    if (byte < 0x80) {
      *codep = byte;
      return 0;
    }
    int len = len_tab[byte >> 4];
    if (len < 1) {
      *state = 1;
      return 1;
    }
    *codep = byte & (0x7F >> len);
    *state = len;
    return 1;
  }
  if ((byte & 0xC0) != 0x80) {
    *state = 1;
    return 1;
  }
  *codep = (*codep << 6) | (byte & 0x3F);
  if (--*state == 0)
    return 0;
  return 1;
}

static void truncate_utf8_string(char *dest, const char *src, size_t dest_size,
                                 int max_chars) {
  if (!src || !dest || dest_size == 0)
    return;
  if (max_chars <= 0) {
    dest[0] = '\0';
    return;
  }
  uint32_t state = 0, codepoint = 0;
  int chars = 0;
  size_t i = 0, last_valid_len = 0;
  while (src[i] && i < dest_size - 4) {
    dest[i] = src[i];
    if (utf8_decode(&state, &codepoint, (uint8_t)src[i]) == 0) {
      chars++;
      last_valid_len = i + 1;
      if (chars == max_chars && src[i + 1] != '\0') {
        strcpy(dest + last_valid_len, "...");
        return;
      }
    }
    i++;
  }
  dest[last_valid_len] = '\0';
}

// ---------- Color helpers ----------
static void hex_to_pixman(uint32_t hex, pixman_color_t *c) {
  c->red = ((hex >> 24) & 0xff) * 0x101;
  c->green = ((hex >> 16) & 0xff) * 0x101;
  c->blue = ((hex >> 8) & 0xff) * 0x101;
  c->alpha = (hex & 0xff) * 0x101;
}

static void pixman_color_to_doubles(const pixman_color_t *c, double *r,
                                    double *g, double *b, double *a) {
  *r = c->red / 65535.0;
  *g = c->green / 65535.0;
  *b = c->blue / 65535.0;
  *a = c->alpha / 65535.0;
}

static void cairo_rounded_rect(cairo_t *cr, double x, double y, double w,
                               double h, double r) {
  if (r > h / 2)
    r = h / 2;
  if (r > w / 2)
    r = w / 2;
  if (r < 0)
    r = 0;
  cairo_new_path(cr);
  cairo_move_to(cr, x + r, y);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI_2);
  cairo_close_path(cr);
}

// ---------- Module style ----------
typedef struct {
  pixman_color_t fg, bg;
  pixman_color_t gradient_end;
  bool bg_gradient;
  int gradient_dir; // 0=to bottom 1=to top 2=to left 3=to right
  int pad_l, pad_r;
  int margin_l, margin_r;
  int radius; // -1=pill, >=0 explicit radius (0=square)
  int min_width; // minimum module width
  bool center; // center text (tag buttons)
} ModuleStyle;

static void apply_css_resolved(ModuleStyle *ms, Style s);

// Tag states: 0=active 1=occupied 2=urgent 3=empty 4=inactive
// Per-profile module styles and bar geometry. The StyleSheet itself is shared
// across all outputs, but the resolved ModuleStyle values depend on each
// profile's bar_height and custom module set.
typedef struct {
  ModuleStyle st_tags[5];
  ModuleStyle st_tags_hover_scratch;
  Style st_tag_hover_css;
  ModuleStyle st_layout, st_title, st_clock, st_clock_date, st_cpu, st_mem;
  ModuleStyle st_network, st_hide_clients, st_battery;
  ModuleStyle st_brightness, st_volume;
  ModuleStyle st_keymode, st_keyboardlayout;
  ModuleStyle st_custom[MANGOBAR_MAX_CUSTOM];
  ModuleStyle st_overview, st_separator, st_tray, st_bar, st_bar_sel;
  int bar_top;
  uint32_t bar_h;
  uint32_t bar_left, bar_right;
  char brightness_dev[64];
  bool brightness_dev_init;
  MangoConfigProfile *profile;
} ProfileRuntime;

static StyleSheet g_style_sheet;
static cairo_t *bar_bg_cr; // cairo context for module backgrounds
static ProfileRuntime *g_rt; // profile runtime for the bar being drawn
static MangoConfigSet g_config_set;
static ProfileRuntime *g_runtimes; // one per profile, indexed by pointer diff

// ---------- Format helpers ----------
static uint32_t text_metrics(const char *text, int32_t *min_x, int32_t *max_x);

static void format_value_full(const char *fmt, const char *value,
                              const char *load, const char *icon, char *out,
                              size_t outsz) {
  if (!fmt || !*fmt) {
    snprintf(out, outsz, "%s", value ? value : "");
    return;
  }
  size_t o = 0;
  const char *p = fmt;
  while (*p && o + 1 < outsz) {
    if (p[0] == '{' && p[1] == '}') {
      if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 2;
    } else if (strncmp(p, "{percent}", 9) == 0) {
      if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 9;
    } else if (strncmp(p, "{load}", 6) == 0) {
      if (load)
        o += snprintf(out + o, outsz - o, "%s", load);
      else if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 6;
    } else if (strncmp(p, "{usage}", 7) == 0) {
      if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 7;
    } else if (strncmp(p, "{volume}", 8) == 0) {
      if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 8;
    } else if (strncmp(p, "{title}", 7) == 0) {
      if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 7;
    } else if (strncmp(p, "{layout}", 8) == 0) {
      if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 8;
    } else if (strncmp(p, "{ifname}", 8) == 0) {
      if (value)
        o += snprintf(out + o, outsz - o, "%s", value);
      p += 8;
    } else if (strncmp(p, "{icon}", 6) == 0) {
      if (icon)
        o += snprintf(out + o, outsz - o, "%s", icon);
      p += 6;
    } else {
      out[o++] = *p++;
    }
  }
  out[o] = '\0';
}

static void format_value(const char *fmt, const char *value, const char *icon,
                         char *out, size_t outsz) {
  format_value_full(fmt, value, NULL, icon, out, outsz);
}

static void format_int(const char *fmt, int value, const char *icon, char *out,
                       size_t outsz) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%d", value);
  format_value(fmt, tmp, icon, out, outsz);
}

// Battery format: {percent}, {icon}, {status} and {ac} (plug icon)
static void format_battery(const char *fmt, int percent, const char *status,
                           const char *icon, const char *ac, char *out,
                           size_t outsz) {
  char pct[16];
  snprintf(pct, sizeof(pct), "%d", percent);
  size_t o = 0;
  const char *p = fmt ? fmt : "";
  while (*p && o + 1 < outsz) {
    if (p[0] == '{' && p[1] == '}') {
      o += snprintf(out + o, outsz - o, "%s", pct);
      p += 2;
    } else if (strncmp(p, "{percent}", 9) == 0) {
      o += snprintf(out + o, outsz - o, "%s", pct);
      p += 9;
    } else if (strncmp(p, "{icon}", 6) == 0) {
      o += snprintf(out + o, outsz - o, "%s", icon ? icon : "");
      p += 6;
    } else if (strncmp(p, "{status}", 8) == 0) {
      o += snprintf(out + o, outsz - o, "%s", status ? status : "");
      p += 8;
    } else if (strncmp(p, "{ac}", 4) == 0) {
      o += snprintf(out + o, outsz - o, "%s", ac ? ac : "");
      p += 4;
    } else {
      out[o++] = *p++;
    }
  }
  out[o] = '\0';
}

// Pick an icon from a level array based on percent (0..100)
static const char *level_icon(const char icons[][16], int count, int pct) {
  if (count <= 0)
    return "";
  int idx = pct * count / 101;
  if (idx >= count)
    idx = count - 1;
  if (idx < 0)
    idx = 0;
  return icons[idx];
}

// Volume format: {volume}/{percent}/{icon} plus {bt} (bluetooth sink)
static void format_volume(const char *fmt, int pct, const char *icon,
                          const char *bt, char *out, size_t outsz) {
  char p[16];
  snprintf(p, sizeof(p), "%d", pct);
  size_t o = 0;
  const char *q = fmt ? fmt : "";
  while (*q && o + 1 < outsz) {
    if (q[0] == '{' && q[1] == '}') {
      o += snprintf(out + o, outsz - o, "%s", p);
      q += 2;
    } else if (strncmp(q, "{volume}", 8) == 0 ||
               strncmp(q, "{percent}", 9) == 0) {
      int n = strncmp(q, "{volume}", 8) == 0 ? 8 : 9;
      o += snprintf(out + o, outsz - o, "%s", p);
      q += n;
    } else if (strncmp(q, "{icon}", 6) == 0) {
      o += snprintf(out + o, outsz - o, "%s", icon ? icon : "");
      q += 6;
    } else if (strncmp(q, "{bluetooth}", 11) == 0 ||
               strncmp(q, "{bt}", 4) == 0) {
      int n = strncmp(q, "{bluetooth}", 11) == 0 ? 11 : 4;
      if (bt && *bt)
        o += snprintf(out + o, outsz - o, "%s", bt);
      else
        while (o > 0 && out[o - 1] == ' ')
          o--;
      q += n;
    } else {
      out[o++] = *q++;
    }
  }
  out[o] = '\0';
}

// Speed units: KB/s under 1MB/s, otherwise MB/s
static void format_speed(double kbps, char *out, size_t outsz) {
  if (kbps >= 1024.0)
    snprintf(out, outsz, "%.1fMB/s", kbps / 1024.0);
  else
    snprintf(out, outsz, "%.0fKB/s", kbps);
}

// Replace {down}/{up} in a network format and fill the rest with format_value
static void format_network_alt(const char *fmt, const char *ifname,
                               const char *down, const char *up, char *out,
                               size_t outsz) {
  char tmp[512];
  size_t o = 0;
  const char *p = fmt ? fmt : "";
  while (*p && o + 1 < sizeof(tmp)) {
    if (strncmp(p, "{down}", 6) == 0) {
      o += snprintf(tmp + o, sizeof(tmp) - o, "%s", down);
      p += 6;
    } else if (strncmp(p, "{up}", 4) == 0) {
      o += snprintf(tmp + o, sizeof(tmp) - o, "%s", up);
      p += 4;
    } else {
      tmp[o++] = *p++;
    }
  }
  tmp[o] = '\0';
  format_value(tmp, ifname, "", out, outsz);
}

// ---------- Bar ----------
#define MAX_HOTSPOTS 256
#define MAX_TRAY_HOTSPOTS 16

typedef struct {
  char module[64];
  int tag; // -1 = not a tag
  uint32_t x1, x2; // logical coords
} Hotspot;

typedef struct {
  MangobarTrayItem *item;
  uint32_t x1, x2; // logical coords
} TrayHotspot;

typedef struct {
  struct wl_output *wl_output;
  struct wl_surface *wl_surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct zxdg_output_v1 *xdg_output;
  uint32_t registry_name;
  char *name;
  int out_x, out_y; // output position in global coords
  int out_w, out_h; // output logical size
  bool configured;
  int scale;          // effective buffer scale for this output
  int wl_scale;       // physical scale factor reported by wl_output.scale
  uint32_t logical_w, logical_h; // logical surface size
  int tag_count;      // max tag number reported by the WM for this output
  uint32_t width, height;
  uint32_t stride, bufsize;
  uint32_t mtags, ctags, urg, sel;
  uint32_t atags;
  char layout[32];
  char title[256];
  char appid[128];
  char keymode[32];
  char kb_layout[16];
  int cpu_pct, mem_pct;
  double cpu_load;
  int hideclients;
  int hover_tag; // hovered tag index, -1 = none
  int battery_pct[MANGOBAR_MAX_BATTERIES];
  bool battery_present[MANGOBAR_MAX_BATTERIES];
  bool battery_on_ac[MANGOBAR_MAX_BATTERIES];
  char battery_status[MANGOBAR_MAX_BATTERIES][16];
  bool volume_bt;
  int brightness_pct, volume_pct;
  bool volume_muted;
  char time_str[16];
  uint8_t alt_on[MANGOBAR_MAX_ALTS];
  char net_ifname[64];
  double net_rx_kbps, net_tx_kbps;
  bool redraw;
  bool overview_mode; // true when active_tags == [0]
  MangoConfigProfile *profile; // matched profile (NULL when disabled/unmatched)
  ProfileRuntime *rt;          // profile runtime (NULL before surface creation)
  bool active;                 // layer surface created for this output
  Hotspot hotspots[MAX_HOTSPOTS];
  int hotspot_count;
  TrayHotspot tray_hotspots[MAX_TRAY_HOTSPOTS];
  int tray_hotspot_count;
  struct wl_list link;
} Bar;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;
static struct wl_list bar_list;
static struct fcft_font *font;
static struct fcft_font *g_draw_font; // scale-adjusted font for drawing
static uint32_t g_draw_scale = 1;
static char g_font_base[256]; // effective font string used for scaling
static bool running = true;

static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_cursor_theme *cursor_theme;
static struct wl_surface *cursor_surface;
static Bar *pointer_bar;
static double pointer_x, pointer_y;
static int axis_steps[2];
static wl_fixed_t axis_value[2];
// Smooth pointer-axis motion is reported in small fractional values. Keep the
// unconsumed distance per axis so it can cross the threshold in a later frame.
static double axis_smooth_remainder[2];
static bool frame_has_axis;            // axis events arrived since last frame
static uint8_t axis_stop_mask;         // axes that reported gesture end
static bool sys_refresh; // force immediate system info refresh

// Persistent PulseAudio context: volume changes arrive via subscription
static pa_mainloop *pa_ml;
static pa_context *pa_ctx;
static atomic_int pa_pct = -1;
static atomic_int pa_muted = -1;
static atomic_int pa_sink_bt;
static atomic_int pa_source_bt;
static atomic_bool pa_dirty;
static int pulse_event_fd = -1;
static pthread_t pulse_thread;
static atomic_bool pa_running;

// udev backlight monitor: external brightness changes
static struct udev *g_udev;
static struct udev_monitor *g_udev_mon;
static int g_udev_fd = -1;
static bool brightness_dirty;

// Custom module realtime-signal triggers (SIGRTMIN+N, like Waybar's custom
// module "signal" option). The handler only sets flags and pokes an eventfd;
// the event loop wakes up and re-runs the affected modules.
static volatile sig_atomic_t
    custom_signal_dirty[MANGOBAR_MAX_CONFIGS][MANGOBAR_MAX_CUSTOM];
static struct {
  int signo;
  int pi; // profile index
  int ci; // custom index within the profile
} custom_signal_map[MANGOBAR_MAX_CONFIGS * MANGOBAR_MAX_CUSTOM];
static int custom_signal_count;
static int signal_fd = -1;

static const struct wl_pointer_listener pointer_listener;

static MangobarTray *tray;

static int ipc_fd = -1;
static char ipc_buf[65536];
static size_t ipc_buf_len = 0;

// IPC debug log: enabled with MANGOBAR_LOG_IPC=1
#define IPC_LOG(...)                                                           \
  do {                                                                         \
    if (getenv("MANGOBAR_LOG_IPC")) {                                          \
      struct timespec ts;                                                      \
      clock_gettime(CLOCK_MONOTONIC, &ts);                                     \
      fprintf(stderr, "[%ld.%03ld] ", (long)ts.tv_sec,                        \
              ts.tv_nsec / 1000000);                                           \
      fprintf(stderr, __VA_ARGS__);                                            \
    }                                                                          \
  } while (0)

// ========== Wayland buffers ==========
static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
  static unsigned long released;
  IPC_LOG("[buf] release #%lu\n", ++released);
  wl_buffer_destroy(wl_buffer);
}
static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static int allocate_shm_file(size_t size) {
  int fd = memfd_create("mangobar", MFD_CLOEXEC);
  if (fd < 0)
    return -1;
  if (ftruncate(fd, size) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// Create a fully transparent wl_buffer (for the grab layer)
static struct wl_buffer *create_transparent_buffer(uint32_t w, uint32_t h,
                                                   uint32_t *stride_out,
                                                   uint32_t *bufsize_out) {
  uint32_t stride = w * 4;
  uint32_t bufsize = stride * h;
  int fd = allocate_shm_file(bufsize);
  if (fd < 0)
    return NULL;
  uint8_t *data = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return NULL;
  }
  memset(data, 0, bufsize);
  munmap(data, bufsize);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bufsize);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  if (stride_out)
    *stride_out = stride;
  if (bufsize_out)
    *bufsize_out = bufsize;
  return buf;
}

// ========== Text drawing ==========
// Measure rendered text width and bounds (incl. glyph bearings) in px
static uint32_t text_metrics_font(const struct fcft_font *f, const char *text,
                                  int32_t *min_x, int32_t *max_x) {
  int32_t mn = INT32_MAX, mx = INT32_MIN;
  int32_t pen = 0;
  uint32_t state = 0, codepoint = 0, last_cp = 0;
  for (const char *p = text; *p; p++) {
    if (utf8_decode(&state, &codepoint, (uint8_t)*p))
      continue;
    const struct fcft_glyph *g =
        fcft_rasterize_char_utf32((struct fcft_font *)f, codepoint,
                                  FCFT_SUBPIXEL_NONE);
    if (!g)
      continue;
    long kern = 0;
    if (last_cp)
      fcft_kerning((struct fcft_font *)f, last_cp, codepoint, &kern, NULL);
    pen += (int32_t)kern;
    int32_t gx = pen + g->x;
    if (gx < mn)
      mn = gx;
    int32_t gx2 = gx + g->width;
    if (gx2 > mx)
      mx = gx2;
    pen += g->advance.x;
    last_cp = codepoint;
  }
  if (min_x)
    *min_x = mn == INT32_MAX ? 0 : mn;
  if (max_x)
    *max_x = mx == INT32_MIN ? 0 : mx;
  return mn == INT32_MAX ? 0 : (uint32_t)(mx - mn);
}

static uint32_t text_metrics(const char *text, int32_t *min_x, int32_t *max_x) {
  return text_metrics_font(font, text, min_x, max_x);
}

// Create or fetch a font sized for `scale` (HiDPI rendering).
static struct fcft_font *font_for_scale(int scale) {
  static struct fcft_font *cache[8];
  static int cached_scale[8];
  if (scale < 1)
    scale = 1;
  if (scale > 7)
    scale = 7;
  if (cache[scale] && cached_scale[scale] == scale)
    return cache[scale];
  const char *sp = strstr(g_font_base, ":size=");
  if (!sp || !g_font_base[0]) {
    cache[scale] = font;
    cached_scale[scale] = scale;
    return font;
  }
  int logical = atoi(sp + 6);
  int px = logical > 0 ? logical * scale : 0;
  char buf[256];
  snprintf(buf, sizeof(buf), "%.*s:size=%d", (int)(sp - g_font_base),
           g_font_base, px);
  struct fcft_font *f = fcft_from_name(1, (const char *[]){buf}, NULL);
  cache[scale] = f ? f : font;
  cached_scale[scale] = scale;
  return cache[scale];
}

// Reserve width for two digits so 3% / 30% keep the module size.
static void ensure_numeric_min_width(ModuleStyle *st, const char *fmt,
                                     const char *icon) {
  char tmp[256];
  format_value(fmt, "88", icon ? icon : "", tmp, sizeof(tmp));
  int32_t mn, mx;
  uint32_t tw = text_metrics(tmp, &mn, &mx);
  int need = (int)tw + st->pad_l + st->pad_r + st->margin_l + st->margin_r;
  if (need > st->min_width)
    st->min_width = need;
}

// Reserve min width from an already-formatted sample string.
static void ensure_module_min_width(ModuleStyle *st, const char *text) {
  if (!text || !*text)
    return;
  int32_t mn, mx;
  uint32_t tw = text_metrics(text, &mn, &mx);
  int need = (int)tw + st->pad_l + st->pad_r + st->margin_l + st->margin_r;
  if (need > st->min_width)
    st->min_width = need;
}

static uint32_t draw_text(const char *text, uint32_t x, uint32_t y,
                          pixman_image_t *fg, pixman_image_t *fg_mask,
                          pixman_image_t *bg, pixman_color_t *fg_color,
                          pixman_color_t *bg_color, uint32_t max_x,
                          uint32_t buf_h) {
  if (!text || !*text || x >= max_x)
    return x;
  uint32_t scale = g_draw_scale > 0 ? g_draw_scale : 1;
  struct fcft_font *df = g_draw_font ? g_draw_font : font;
  uint32_t dx = x * scale;
  uint32_t dy = y * scale;
  uint32_t dmax = max_x * scale;
  uint32_t dbuf = buf_h * scale;
  // x is the glyph-box left; pen starts minus the first glyph's left bearing
  int32_t mn = 0;
  text_metrics_font(df, text, &mn, NULL);
  int32_t pen = (int32_t)dx - mn;
  uint32_t state = 0, codepoint = 0, last_cp = 0;
  for (const char *p = text; *p; p++) {
    if (utf8_decode(&state, &codepoint, (uint8_t)*p))
      continue;
    const struct fcft_glyph *g =
        fcft_rasterize_char_utf32(df, codepoint, FCFT_SUBPIXEL_NONE);
    if (!g)
      continue;
    long kern = 0;
    if (last_cp)
      fcft_kerning(df, last_cp, codepoint, &kern, NULL);
    pen += (int32_t)kern;
    int32_t gx = pen + g->x;
    int32_t gx2 = gx + g->width;
    if (gx >= (int32_t)dmax)
      break;
    last_cp = codepoint;

    if (fg && fg_color) {
      if (pixman_image_get_format(g->pix) == PIXMAN_a8r8g8b8) {
        pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, fg, 0, 0, 0, 0,
                                 gx, (int32_t)dy - g->y, g->width, g->height);
      } else {
        pixman_image_fill_boxes(
            PIXMAN_OP_OVER, fg, fg_color, 1,
            &(pixman_box32_t){.x1 = gx, .x2 = gx2, .y1 = 0, .y2 = (int32_t)dbuf});
      }
      pixman_image_t *mask = pixman_image_create_solid_fill(
          &(pixman_color_t){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF});
      pixman_image_composite32(PIXMAN_OP_OVER, g->pix, mask, fg_mask, 0, 0, 0,
                               0, gx, (int32_t)dy - g->y, g->width, g->height);
      pixman_image_unref(mask);
    }
    if (bg && bg_color)
      pixman_image_fill_boxes(
          PIXMAN_OP_OVER, bg, bg_color, 1,
          &(pixman_box32_t){.x1 = gx, .x2 = gx2, .y1 = 0, .y2 = (int32_t)dbuf});
    pen += g->advance.x;
  }
  return (uint32_t)(pen - mn);
}

// ---------- Hotspots ----------
static void record_hotspot(Bar *bar, const char *module, int tag, uint32_t x1,
                           uint32_t x2) {
  if (bar->hotspot_count >= MAX_HOTSPOTS)
    return;
  Hotspot *h = &bar->hotspots[bar->hotspot_count++];
  snprintf(h->module, sizeof(h->module), "%s", module ? module : "");
  h->tag = tag;
  h->x1 = x1;
  h->x2 = x2;
}

static void record_tray_hotspot(Bar *bar, MangobarTrayItem *item, uint32_t x1,
                                uint32_t x2) {
  if (bar->tray_hotspot_count >= MAX_TRAY_HOTSPOTS)
    return;
  bar->tray_hotspots[bar->tray_hotspot_count++] =
      (TrayHotspot){item, x1, x2};
}

// Draw a module (background + text + record hotspot)
static int module_radius(const ModuleStyle *st, uint32_t h) {
  int r = st->radius;
  if (st->radius < 0)
    r = (int)h / 2;
  if (r > (int)h / 2)
    r = (int)h / 2;
  return r;
}

static uint32_t draw_module(Bar *bar, const char *module, int tag,
                            const char *text, ModuleStyle *st, uint32_t x,
                            uint32_t y, pixman_image_t *fg,
                            pixman_image_t *fg_mask, pixman_image_t *bg,
                            uint32_t max_x, uint32_t buf_h) {
  if (!text || !*text)
    return x;
  int32_t mn = 0, mx = 0;
  uint32_t tw = text_metrics(text, &mn, &mx);
  uint32_t body = tw + st->pad_l + st->pad_r;
  uint32_t mw = body + st->margin_l + st->margin_r;
  if ((int)mw < st->min_width) {
    mw = (uint32_t)st->min_width;
    body = mw - st->margin_l - st->margin_r;
  }
  uint32_t x0 = x;
  uint32_t x1 = x + st->margin_l;
  uint32_t x2 = x1 + body;
  if (x2 > max_x)
    x2 = max_x;
  if (x2 > x1) {
    int rad = module_radius(st, buf_h);
    // Rounded background
    if (bar_bg_cr) {
      cairo_pattern_t *pat = NULL;
      if (st->bg_gradient) {
        double x1d = (double)x1, y1d = (double)g_rt->bar_top;
        double x2d = (double)x2, y2d = (double)(g_rt->bar_top + buf_h);
        if (st->gradient_dir == 1) { // to top: vertical, bottom -> top
          x2d = x1d;
          y1d = (double)(g_rt->bar_top + buf_h);
          y2d = (double)g_rt->bar_top;
        } else if (st->gradient_dir == 2) { // to left: horizontal, right -> left
          y2d = y1d;
          x1d = (double)x2;
          x2d = (double)x1;
        } else if (st->gradient_dir == 3) { // to right: horizontal, left -> right
          y2d = y1d;
        } else { // to bottom (default): vertical, top -> bottom
          x2d = x1d;
        }
        double r0, g0, b0, a0, r1, g1, b1, a1;
        pixman_color_to_doubles(&st->bg, &r0, &g0, &b0, &a0);
        pixman_color_to_doubles(&st->gradient_end, &r1, &g1, &b1, &a1);
        pat = cairo_pattern_create_linear(x1d, y1d, x2d, y2d);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, r0, g0, b0, a0);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, r1, g1, b1, a1);
        cairo_set_source(bar_bg_cr, pat);
      } else {
        double r, g, b, a;
        pixman_color_to_doubles(&st->bg, &r, &g, &b, &a);
        cairo_set_source_rgba(bar_bg_cr, r, g, b, a);
      }
      cairo_rounded_rect(bar_bg_cr, x1, g_rt->bar_top, (double)(x2 - x1),
                                (double)buf_h, rad);
      cairo_fill(bar_bg_cr);
      if (pat)
        cairo_pattern_destroy(pat);
    } else {
      uint32_t s = g_draw_scale > 0 ? g_draw_scale : 1;
      pixman_image_fill_boxes(
          PIXMAN_OP_OVER, bg, &st->bg, 1,
          &(pixman_box32_t){.x1 = x1 * s, .x2 = x2 * s, .y1 = g_rt->bar_top * s,
                            .y2 = (g_rt->bar_top + buf_h) * s});
    }
    uint32_t text_x = x1 + st->pad_l;
    if (st->center) {
      uint32_t body_w = x2 - x1;
      uint32_t inner = body_w - st->pad_l - st->pad_r;
      if (tw < inner)
        text_x = x1 + st->pad_l + (inner - tw) / 2;
    }
    draw_text(text, text_x, y, fg, fg_mask, NULL, &st->fg, NULL, max_x, buf_h);
  }
  uint32_t end = x + mw;
  if (end > max_x)
    end = max_x;
  record_hotspot(bar, module, tag, x0, end);
  return end;
}

static int alt_index(const char *module) {
  for (int i = 0; i < g_cfg.alt_count; i++)
    if (strcmp(g_cfg.alts[i].module, module) == 0)
      return i;
  return -1;
}

// Format string with format-alt applied when toggled.
static const char *module_fmt(const char *module, const char *fallback,
                              const Bar *bar) {
  int ai = alt_index(module);
  return (ai >= 0 && bar->alt_on[ai]) ? g_cfg.alts[ai].fmt : fallback;
}

// Fill custom module text and CSS/action name; false when disabled
static bool custom_module_text(const Bar *bar, int ci, char *dst, size_t dstsz,
                               char *name, size_t namesz) {
  if (ci < 0 || ci >= g_cfg.custom_count || !g_cfg.customs[ci].enabled)
    return false;
  const MangoCustomModule *cm = &g_cfg.customs[ci];
  snprintf(name, namesz, "custom-%s", cm->name);
  format_value(module_fmt(name, cm->format, bar), cm->output, "", dst, dstsz);
  return true;
}

// Scale and draw a tray icon (nearest neighbor)
static void draw_tray_icon(pixman_image_t *dst, pixman_image_t *icon,
                           int dst_size, int dx, int dy) {
  if (!icon || dst_size <= 0)
    return;
  int scale = g_draw_scale > 0 ? (int)g_draw_scale : 1;
  int ds = dst_size * scale;
  int sw = pixman_image_get_width(icon);
  int sh = pixman_image_get_height(icon);
  if (sw <= 0 || sh <= 0)
    return;
  const uint32_t *src = (const uint32_t *)pixman_image_get_data(icon);
  pixman_image_t *scaled = pixman_image_create_bits(
      PIXMAN_a8r8g8b8, ds, ds, NULL, ds * 4);
  if (!scaled)
    return;
  uint32_t *dstbuf = (uint32_t *)pixman_image_get_data(scaled);
  if (ds >= sw && ds >= sh) {
    // Upscale: bilinear.
    for (int yy = 0; yy < ds; yy++) {
      double sy = ((double)yy + 0.5) * sh / ds - 0.5;
      if (sy < 0)
        sy = 0;
      int y0 = (int)sy;
      int y1 = y0 + 1 < sh ? y0 + 1 : y0;
      double fy = sy - y0;
      const uint32_t *row0 = src + (size_t)y0 * sw;
      const uint32_t *row1 = src + (size_t)y1 * sw;
      for (int xx = 0; xx < ds; xx++) {
        double sx = ((double)xx + 0.5) * sw / ds - 0.5;
        if (sx < 0)
          sx = 0;
        int x0 = (int)sx;
        int x1 = x0 + 1 < sw ? x0 + 1 : x0;
        double fx = sx - x0;
        uint32_t p00 = row0[x0], p01 = row0[x1];
        uint32_t p10 = row1[x0], p11 = row1[x1];
        uint32_t out[4];
        for (int c = 0; c < 4; c++) {
          int sh_ = (3 - c) * 8; // high byte first: A, R, G, B
          double t =
              ((p00 >> sh_) & 0xff) * (1 - fx) * (1 - fy) +
              ((p01 >> sh_) & 0xff) * fx * (1 - fy) +
              ((p10 >> sh_) & 0xff) * (1 - fx) * fy +
              ((p11 >> sh_) & 0xff) * fx * fy;
          out[c] = (uint32_t)(t + 0.5);
          if (out[c] > 255)
            out[c] = 255;
        }
        dstbuf[yy * ds + xx] =
            (out[0] << 24) | (out[1] << 16) | (out[2] << 8) | out[3];
      }
    }
  } else {
    // Downscale: exact area average, avoids aliasing on edges.
    for (int yy = 0; yy < ds; yy++) {
      double y0f = (double)yy * sh / ds;
      double y1f = (double)(yy + 1) * sh / ds;
      int y0 = (int)y0f;
      int y1 = (int)ceil(y1f);
      if (y1 > sh)
        y1 = sh;
      if (y1 <= y0)
        y1 = y0 + 1;
      for (int xx = 0; xx < ds; xx++) {
        double x0f = (double)xx * sw / ds;
        double x1f = (double)(xx + 1) * sw / ds;
        int x0 = (int)x0f;
        int x1 = (int)ceil(x1f);
        if (x1 > sw)
          x1 = sw;
        if (x1 <= x0)
          x1 = x0 + 1;
        double area = (x1f - x0f) * (y1f - y0f);
        double acc[4] = {0, 0, 0, 0};
        for (int sy = y0; sy < y1; sy++) {
          double wy = fmin((double)(sy + 1), y1f) - fmax((double)sy, y0f);
          if (wy <= 0)
            continue;
          const uint32_t *row = src + (size_t)sy * sw;
          for (int sx = x0; sx < x1; sx++) {
            double wx = fmin((double)(sx + 1), x1f) - fmax((double)sx, x0f);
            if (wx <= 0)
              continue;
            double w = wx * wy / area;
            uint32_t p = row[sx];
            acc[0] += ((p >> 24) & 0xff) * w;
            acc[1] += ((p >> 16) & 0xff) * w;
            acc[2] += ((p >> 8) & 0xff) * w;
            acc[3] += (p & 0xff) * w;
          }
        }
        uint32_t out[4];
        for (int c = 0; c < 4; c++) {
          out[c] = (uint32_t)(acc[c] + 0.5);
          if (out[c] > 255)
            out[c] = 255;
        }
        dstbuf[yy * ds + xx] =
            (out[0] << 24) | (out[1] << 16) | (out[2] << 8) | out[3];
      }
    }
  }
  pixman_image_composite32(PIXMAN_OP_OVER, scaled, NULL, dst, 0, 0, 0, 0,
                           dx * scale, dy * scale, ds, ds);
  pixman_image_unref(scaled);
}

#define MAX_MODULE_ENTRIES 128

typedef struct {
  const char *text;
  ModuleStyle *st;
  const char *module;
  int tag;
  bool is_tray;
  uint32_t width;
  int tray_icon_size;
} ModuleEntry;

// Per-module max display width in pixels; 0 = unlimited.
static int module_max_length(const char *module) {
  for (int i = 0; i < g_cfg.max_len_count; i++)
    if (strcmp(g_cfg.max_lens[i].module, module) == 0)
      return g_cfg.max_lens[i].max_length;
  return 0;
}

// Truncate text so its rendered width (with "...") is at most max_px.
static void truncate_text_to_width(char *dest, const char *src,
                                   size_t dest_size, uint32_t max_px) {
  char tmp[256];
  int maxc = 256;
  while (maxc > 0) {
    truncate_utf8_string(tmp, src, sizeof(tmp), maxc);
    int32_t mn, mx;
    uint32_t tw = text_metrics(tmp, &mn, &mx);
    if (tw <= max_px || maxc <= 1) {
      snprintf(dest, dest_size, "%s", tmp);
      return;
    }
    // Step down by 8, but never skip the one-character fallback.
    maxc = maxc > 8 ? maxc - 8 : 1;
  }
  dest[0] = '\0';
}

// Fill entries for one module id; returns the number of entries added.
static int append_module_entries(Bar *bar, int id, ModuleEntry *ents, int max,
                                 char (*texts)[256], char (*names)[96],
                                 int *text_n, int *name_n) {
  int n = 0;
  char *dst;
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);

  // Custom modules use ids M_CUSTOM + index, so they can't be matched by a
  // single switch case; handle them before the fixed-module switch.
  if (id >= M_CUSTOM && id < M_CUSTOM + MANGOBAR_MAX_CUSTOM) {
    int ci = id - M_CUSTOM;
    if (!custom_module_text(bar, ci, texts[(*text_n)], 256,
                            names[(*name_n)], 96))
      return 0;
    ModuleEntry *me = &ents[n++];
    *me = (ModuleEntry){.text = texts[(*text_n)++],
                        .st = &g_rt->st_custom[ci],
                        .module = names[(*name_n)++],
                        .tag = -1};
    int ml = module_max_length(me->module);
    if (ml > 0)
      truncate_text_to_width((char *)me->text, me->text, 256, (uint32_t)ml);
    return n;
  }

  // Battery instances use ids M_BATTERY + index into g_cfg.batteries, so
  // "battery" and "battery#BAT0"/"battery#BAT1" can be shown separately.
  if (id >= M_BATTERY && id < M_BATTERY + MANGOBAR_MAX_BATTERIES) {
    int bi = id - M_BATTERY;
    if (bi >= g_cfg.battery_count)
      return 0;
    MangoBatteryCfg *bc = &g_cfg.batteries[bi];
    if (!bc->enabled || !bar->battery_present[bi])
      return 0;
    if (bc->hide_on_ac && bar->battery_on_ac[bi])
      return 0;
    const char *icon;
    if (strncmp(bar->battery_status[bi], "Charging", 8) == 0)
      icon = bc->icon_charging;
    else if (strncmp(bar->battery_status[bi], "Full", 4) == 0)
      icon = bc->icon_full;
    else
      icon = level_icon(bc->icons, bc->icon_count, bar->battery_pct[bi]);
    dst = texts[(*text_n)++];
    format_battery(module_fmt(bc->name, bc->fmt, bar), bar->battery_pct[bi],
                   bar->battery_status[bi], icon,
                   bar->battery_on_ac[bi] ? bc->icon_ac : "", dst, 256);
    ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_battery,
                              .module = bc->name, .tag = -1};
    return n;
  }

  switch (id) {
  case M_TAGS:
    if (bar->overview_mode) {
      if (max > 0)
        ents[n++] = (ModuleEntry){.text = g_cfg.overview_label,
                                  .st = &g_rt->st_overview, .module = "tags",
                                  .tag = -1};
    } else {
      for (int i = 0; i < bar->tag_count && n < max; i++) {
        if (g_cfg.only_occupied && !(bar->ctags & (1 << i)) &&
            !(bar->atags & (1 << i)) && !(g_cfg.pinned_tags & (1 << i)))
          continue;
        ModuleStyle *st;
        if (bar->urg & (1 << i))
          st = &g_rt->st_tags[2];
        else if (bar->mtags & (1 << i))
          st = &g_rt->st_tags[0];
        else if (bar->ctags & (1 << i))
          st = &g_rt->st_tags[1];
        else
          st = &g_rt->st_tags[3];
        if (bar->hover_tag == i) {
          g_rt->st_tags_hover_scratch = *st;
          apply_css_resolved(&g_rt->st_tags_hover_scratch, g_rt->st_tag_hover_css);
          st = &g_rt->st_tags_hover_scratch;
        }
        ents[n++] = (ModuleEntry){.text = g_cfg.tag_names[i], .st = st,
                                  .module = "tags", .tag = i};
      }
    }
    break;
  case M_LAYOUT:
    dst = texts[(*text_n)++];
    format_value(g_cfg.layout_format, bar->layout, "", dst, 256);
    ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_layout,
                              .module = "layout", .tag = -1};
    break;
  case M_TITLE:
    if (bar->title[0]) {
      dst = texts[(*text_n)++];
      format_value(g_cfg.title_format, bar->title, "", dst, 256);
      ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_title,
                                .module = "title", .tag = -1};
    }
    break;
  case M_CPU:
    {
      char usagestr[16];
      char loadstr[32];
      snprintf(usagestr, sizeof(usagestr), "%d", bar->cpu_pct);
      snprintf(loadstr, sizeof(loadstr), "%.2f", bar->cpu_load);
      dst = texts[(*text_n)++];
      format_value_full(module_fmt("cpu", g_cfg.cpu_format, bar), usagestr,
                        loadstr, "", dst, 256);
      ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_cpu,
                                .module = "cpu", .tag = -1};
      ensure_numeric_min_width(&g_rt->st_cpu, module_fmt("cpu", g_cfg.cpu_format, bar),
                               "");
    }
    break;
  case M_MEM:
    dst = texts[(*text_n)++];
    format_int(module_fmt("mem", g_cfg.mem_format, bar), bar->mem_pct, "", dst,
               256);
    ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_mem, .module = "mem",
                              .tag = -1};
    ensure_numeric_min_width(&g_rt->st_mem, module_fmt("mem", g_cfg.mem_format, bar),
                             "");
    break;
  case M_BRIGHTNESS:
    {
      const char *bicon = level_icon(g_cfg.brightness_icons,
                                     g_cfg.brightness_icon_count,
                                     bar->brightness_pct);
      dst = texts[(*text_n)++];
      format_int(module_fmt("brightness", g_cfg.brightness_fmt, bar),
                 bar->brightness_pct, bicon, dst, 256);
      ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_brightness,
                                .module = "brightness", .tag = -1};
      ensure_numeric_min_width(&g_rt->st_brightness,
                               module_fmt("brightness",
                                          g_cfg.brightness_fmt, bar),
                               bicon);
    }
    break;
  case M_VOLUME:
    {
      int ai = alt_index("volume");
      const char *fmt;
      const char *icon;
      if (ai >= 0 && bar->alt_on[ai]) {
        fmt = g_cfg.alts[ai].fmt;
        icon = level_icon(g_cfg.volume_icons, g_cfg.volume_icon_count,
                          bar->volume_pct);
      } else if (bar->volume_muted) {
        fmt = g_cfg.volume_fmt_muted;
        icon = g_cfg.volume_muted_icon;
      } else {
        fmt = g_cfg.volume_fmt;
        icon = level_icon(g_cfg.volume_icons, g_cfg.volume_icon_count,
                          bar->volume_pct);
      }
      dst = texts[(*text_n)++];
      format_volume(fmt, bar->volume_pct, icon,
                    bar->volume_bt ? g_cfg.volume_bt_icon : "", dst, 256);
      ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_volume,
                                .module = "volume", .tag = -1};
      // Reserve room for two-digit values, but never for the {bt} mark.
      char wt[256];
      format_volume(g_cfg.volume_fmt, 88,
                    level_icon(g_cfg.volume_icons, g_cfg.volume_icon_count,
                               bar->volume_pct),
                    "", wt, sizeof(wt));
      ensure_module_min_width(&g_rt->st_volume, wt);
      format_volume(g_cfg.volume_fmt_muted, 88, g_cfg.volume_muted_icon, "",
                    wt, sizeof(wt));
      ensure_module_min_width(&g_rt->st_volume, wt);
    }
    break;
  case M_CLOCK_TIME:
    dst = texts[(*text_n)++];
    strftime(dst, 256, module_fmt("clock", g_cfg.clock_time_format, bar),
             tm);
    ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_clock, .module = "clock",
                              .tag = -1};
    break;
  case M_CLOCK_DATE:
    dst = texts[(*text_n)++];
    strftime(dst, 256, module_fmt("clock.date", g_cfg.clock_date_format, bar),
             tm);
    ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_clock_date,
                              .module = "clock.date", .tag = -1};
    break;
  case M_KEYMODE:
    dst = texts[(*text_n)++];
    format_value(module_fmt("keymode", g_cfg.keymode_format, bar), bar->keymode,
                 "", dst, 256);
    ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_keymode,
                              .module = "keymode", .tag = -1};
    break;
  case M_KBLAYOUT:
    dst = texts[(*text_n)++];
    format_value(module_fmt("keyboardlayout", g_cfg.keyboardlayout_format, bar),
                 bar->kb_layout, "", dst, 256);
    ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_keyboardlayout,
                              .module = "keyboardlayout", .tag = -1};
    break;
  case M_NETWORK:
    {
      int ai = alt_index("network");
      dst = texts[(*text_n)++];
      if (ai >= 0 && bar->alt_on[ai]) {
        char down[32], up[32];
        format_speed(bar->net_rx_kbps, down, sizeof(down));
        format_speed(bar->net_tx_kbps, up, sizeof(up));
        format_network_alt(g_cfg.alts[ai].fmt, bar->net_ifname, down, up, dst,
                           256);
      } else {
        format_value(g_cfg.network_format, bar->net_ifname, "", dst, 256);
      }
      ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_network,
                                .module = "network", .tag = -1};
    }
    break;
  case M_HIDE_CLIENTS:
    if (bar->hideclients > 0) {
      dst = texts[(*text_n)++];
      format_int(module_fmt("hideclients", g_cfg.hide_clients_format, bar),
                 bar->hideclients, "", dst, 256);
      ents[n++] = (ModuleEntry){.text = dst, .st = &g_rt->st_hide_clients,
                                .module = "hideclients", .tag = -1};
    }
    break;
  case M_TRAY:
    {
      int tvis = 0;
      if (tray && tray_visible_items(tray, &tvis) && tvis > 0) {
        int tsize = g_cfg.tray_icon_size > 0
                        ? g_cfg.tray_icon_size
                        : (int)g_rt->bar_h - 2 * g_cfg.tray_pad;
        if (tsize < 8)
          tsize = 8;
        if (tsize > (int)g_rt->bar_h - 2 * g_cfg.tray_pad)
          tsize = (int)g_rt->bar_h - 2 * g_cfg.tray_pad;
        uint32_t tw = (uint32_t)(tvis * tsize + (tvis - 1) * g_cfg.tray_gap +
                                 g_rt->st_tray.pad_l + g_rt->st_tray.pad_r +
                                 g_rt->st_tray.margin_l + g_rt->st_tray.margin_r);
        ents[n++] = (ModuleEntry){.module = "tray",
                                  .st = &g_rt->st_tray,
                                  .tag = -1,
                                  .is_tray = true,
                                  .width = tw,
                                  .tray_icon_size = tsize};
      }
    }
    break;
  default:
    break;
  }
  // Apply per-module max width to the formatted text (tags keep their short
  // labels untouched).
  for (int i = 0; i < n; i++) {
    if (strcmp(ents[i].module, "tags") == 0)
      continue;
    int ml = module_max_length(ents[i].module);
    if (ml > 0)
      truncate_text_to_width((char *)ents[i].text, ents[i].text, 256,
                             (uint32_t)ml);
  }
  return n;
}

static int build_module_entries(Bar *bar, const int *order, int count,
                                ModuleEntry *ents, int max,
                                char (*texts)[256], char (*names)[96],
                                int *text_n, int *name_n) {
  int n = 0;
  for (int i = 0; i < count && n < max; i++)
    n += append_module_entries(bar, order[i], ents + n, max - n, texts, names,
                               text_n, name_n);
  return n;
}

// Truncate text to available width (with "..."), for the window module.
// `extra` is the module's horizontal padding + margins: the text plus that
// amount must fit inside `avail` so the whole module box stays within bounds.
static void fit_text_width(const char *text, char *out, size_t outsz,
                           uint32_t avail, uint32_t extra) {
  char tmp[256];
  int maxc = 256;
  while (maxc > 0) {
    truncate_utf8_string(tmp, text, sizeof(tmp), maxc);
    int32_t mn, mx;
    uint32_t tw = text_metrics(tmp, &mn, &mx);
    if (tw + extra <= avail || maxc <= 1) {
      snprintf(out, outsz, "%s", tmp);
      return;
    }
    // Step down by 8, but never skip the one-character fallback: otherwise
    // the loop would exit at maxc == 0 and return an empty string, making
    // the window module disappear entirely when space is tight.
    maxc = maxc > 8 ? maxc - 8 : 1;
  }
  out[0] = '\0';
}

static uint32_t module_entry_width(const ModuleEntry *e) {
  if (e->is_tray)
    return e->width;
  int32_t mn, mx;
  uint32_t tw = text_metrics(e->text, &mn, &mx);
  uint32_t body = tw + e->st->pad_l + e->st->pad_r;
  uint32_t mw = body + e->st->margin_l + e->st->margin_r;
  if ((int)mw < e->st->min_width)
    mw = (uint32_t)e->st->min_width;
  return mw;
}

static uint32_t draw_tray_entry(Bar *bar, const ModuleEntry *e, uint32_t x,
                                uint32_t y, pixman_image_t *fg,
                                pixman_image_t *fg_mask, pixman_image_t *bg,
                                uint32_t max_x, uint32_t buf_h) {
  int count = 0;
  MangobarTrayItem **items = tray_visible_items(tray, &count);
  uint32_t tray_x1 = x + g_rt->st_tray.margin_l;
  uint32_t tray_body_w = e->width - g_rt->st_tray.margin_l - g_rt->st_tray.margin_r;
  if (tray_x1 + tray_body_w > max_x)
    tray_body_w = max_x > tray_x1 ? max_x - tray_x1 : 0;
  if (bar_bg_cr && tray_body_w > 0) {
    cairo_pattern_t *pat = NULL;
    if (g_rt->st_tray.bg_gradient) {
      double x1d = (double)tray_x1, y1d = (double)g_rt->bar_top;
      double x2d = (double)(tray_x1 + tray_body_w), y2d = (double)g_rt->bar_top;
      if (g_rt->st_tray.gradient_dir == 1) { // to top: vertical, bottom -> top
        x2d = x1d;
        y1d = (double)(g_rt->bar_top + buf_h);
        y2d = (double)g_rt->bar_top;
      } else if (g_rt->st_tray.gradient_dir == 2) { // to left: right -> left
        y2d = y1d;
        x1d = (double)(tray_x1 + tray_body_w);
        x2d = (double)tray_x1;
      } else if (g_rt->st_tray.gradient_dir == 3) { // to right: left -> right
        y2d = y1d;
      } else { // to bottom: vertical, top -> bottom
        x2d = x1d;
        y2d = (double)(g_rt->bar_top + buf_h);
      }
      double r0, g0, b0, a0, r1, g1, b1, a1;
      pixman_color_to_doubles(&g_rt->st_tray.bg, &r0, &g0, &b0, &a0);
      pixman_color_to_doubles(&g_rt->st_tray.gradient_end, &r1, &g1, &b1, &a1);
      pat = cairo_pattern_create_linear(x1d, y1d, x2d, y2d);
      cairo_pattern_add_color_stop_rgba(pat, 0.0, r0, g0, b0, a0);
      cairo_pattern_add_color_stop_rgba(pat, 1.0, r1, g1, b1, a1);
      cairo_set_source(bar_bg_cr, pat);
    } else {
      double tr, tg, tb, ta;
      pixman_color_to_doubles(&g_rt->st_tray.bg, &tr, &tg, &tb, &ta);
      cairo_set_source_rgba(bar_bg_cr, tr, tg, tb, ta);
    }
    cairo_rounded_rect(bar_bg_cr, tray_x1, g_rt->bar_top, (double)tray_body_w,
                       (double)buf_h, module_radius(&g_rt->st_tray, buf_h));
    cairo_fill(bar_bg_cr);
    if (pat)
      cairo_pattern_destroy(pat);
  }
  uint32_t cur = tray_x1;
  int tpad = g_rt->st_tray.pad_l > 0 ? g_rt->st_tray.pad_l : g_cfg.tray_pad;
  int tsize = e->tray_icon_size;
  for (int i = 0; i < count; i++) {
    int dx = (int)cur + tpad;
    int dy = g_rt->bar_top + ((int)buf_h - tsize) / 2;
    draw_tray_icon(bg, tray_item_icon(items[i]), tsize, dx, dy);
    record_tray_hotspot(bar, items[i], (uint32_t)cur,
                        (uint32_t)(cur + (uint32_t)tsize + (uint32_t)tpad +
                                   g_rt->st_tray.pad_r));
    cur += (uint32_t)(tsize + g_cfg.tray_gap);
  }
  return x + e->width;
}

static uint32_t draw_module_entry(Bar *bar, const ModuleEntry *e, uint32_t x,
                                  uint32_t y, pixman_image_t *fg,
                                  pixman_image_t *fg_mask, pixman_image_t *bg,
                                  uint32_t max_x, uint32_t buf_h) {
  if (e->is_tray)
    return draw_tray_entry(bar, e, x, y, fg, fg_mask, bg, max_x, buf_h);
  return draw_module(bar, e->module, e->tag, e->text, e->st, x, y, fg, fg_mask,
                     bg, max_x, buf_h);
}

static void draw_bar(Bar *bar) {
  IPC_LOG("[draw] %s enter\n", bar->name);
  if (!bar->profile || !bar->rt)
    return; // pending output without a matched profile
  g_rt = bar->rt;
  g_cfg_ptr = &bar->profile->config;
  g_draw_scale = bar->scale > 0 ? (uint32_t)bar->scale : 1;
  g_draw_font = font_for_scale(bar->scale);
  int fd = allocate_shm_file(bar->bufsize);
  if (fd < 0) {
    IPC_LOG("[draw] %s shm alloc FAILED\n", bar->name);
    return;
  }
  uint32_t *data =
      mmap(NULL, bar->bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, bar->bufsize);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      pool, 0, bar->width, bar->height, bar->stride, WL_SHM_FORMAT_ARGB8888);
  wl_buffer_add_listener(buf, &wl_buffer_listener, NULL);
  wl_shm_pool_destroy(pool);
  close(fd);

  pixman_image_t *final = pixman_image_create_bits(
      PIXMAN_a8r8g8b8, bar->width, bar->height, data, bar->width * 4);
  pixman_image_t *fg = pixman_image_create_bits(
      PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);
  pixman_image_t *fg_mask = pixman_image_create_bits(
      PIXMAN_a8, bar->width, bar->height, NULL, bar->width * 4);
  pixman_image_t *bg = pixman_image_create_bits(
      PIXMAN_a8r8g8b8, bar->width, bar->height, NULL, bar->width * 4);

  pixman_color_t transparent = {0, 0, 0, 0};
  pixman_image_fill_boxes(
      PIXMAN_OP_SRC, final, &transparent, 1,
      &(pixman_box32_t){.x1 = 0, .x2 = bar->width, .y1 = 0, .y2 = bar->height});
  pixman_image_fill_boxes(
      PIXMAN_OP_SRC, bg, &transparent, 1,
      &(pixman_box32_t){.x1 = 0, .x2 = bar->width, .y1 = 0, .y2 = bar->height});

  // Wrap bg in cairo for rounded module backgrounds
  bar_bg_cr = NULL;
  {
    uint32_t *bgdata = (uint32_t *)pixman_image_get_data(bg);
    if (bgdata) {
      cairo_surface_t *cs = cairo_image_surface_create_for_data(
          (unsigned char *)bgdata, CAIRO_FORMAT_ARGB32, bar->width,
          bar->height, bar->width * 4);
      if (cairo_surface_status(cs) == CAIRO_STATUS_SUCCESS) {
        bar_bg_cr = cairo_create(cs);
        cairo_scale(bar_bg_cr, bar->scale, bar->scale);
        cairo_set_antialias(bar_bg_cr, CAIRO_ANTIALIAS_BEST);
      }
      cairo_surface_destroy(cs);
    }
  }

  bar->hotspot_count = 0;
  bar->tray_hotspot_count = 0;

  uint32_t y = (uint32_t)g_rt->bar_top + (g_rt->bar_h + font->ascent - font->descent) / 2;

  // --- Build right modules (width known before left/center layout) ---
  ModuleEntry right_ents[MAX_MODULE_ENTRIES];
  char right_texts[MAX_MODULE_ENTRIES][256];
  char right_names[MAX_MODULE_ENTRIES][96];
  int rtext_n = 0, rname_n = 0;
  int right_n = build_module_entries(bar, g_cfg.right_order, g_cfg.right_count,
                                     right_ents, MAX_MODULE_ENTRIES,
                                     right_texts, right_names, &rtext_n,
                                     &rname_n);

  // Total right width (no separators between modules)
  uint32_t right_total_w = 0;
  for (int i = 0; i < right_n; i++) {
    right_total_w += module_entry_width(&right_ents[i]);
  }

  uint32_t logical_w = bar->width / (bar->scale > 0 ? (uint32_t)bar->scale : 1);
  uint32_t right_edge = logical_w > g_rt->bar_right ? logical_w - g_rt->bar_right : 0;
  // Right group right-aligned
  uint32_t right_group_left =
      right_edge > right_total_w ? right_edge - right_total_w : 0;

  // --- Center modules (built first: their width decides how much space the
  // left/right groups get) ---
  ModuleEntry center_ents[MAX_MODULE_ENTRIES];
  char center_texts[MAX_MODULE_ENTRIES][256];
  char center_names[MAX_MODULE_ENTRIES][96];
  int ctext_n = 0, cname_n = 0;
  int center_n =
      build_module_entries(bar, g_cfg.center_order, g_cfg.center_count,
                           center_ents, MAX_MODULE_ENTRIES, center_texts,
                           center_names, &ctext_n, &cname_n);
  uint32_t center_cw = 0;
  uint32_t center_title_w = 0;
  bool center_has_title = false;
  for (int i = 0; i < center_n; i++) {
    uint32_t ew = module_entry_width(&center_ents[i]);
    center_cw += ew;
    if (strcmp(center_ents[i].module, "title") == 0) {
      center_title_w = ew;
      center_has_title = true;
    }
  }
  // When center modules exist they are forced to the true screen center; the
  // left/right groups only get the remaining space on their own side. Without
  // center modules the old squeeze behavior is kept unchanged.
  uint32_t center_left = 0, center_right = 0, center_max = 0;
  uint32_t center_title_avail = 0;
  if (center_n > 0) {
    uint32_t usable = right_edge > g_rt->bar_left ? right_edge - g_rt->bar_left : 0;
    center_left = center_cw < usable ? g_rt->bar_left + (usable - center_cw) / 2
                                     : g_rt->bar_left;
    center_right = center_left + center_cw;
    center_max = center_right < right_edge ? center_right : right_edge;
    if (center_has_title) {
      uint32_t fixed_w = center_cw - center_title_w;
      uint32_t space = center_max > center_left &&
                               center_max - center_left > fixed_w
                           ? center_max - center_left - fixed_w
                           : 0;
      uint32_t title_min = center_title_w < 48 ? center_title_w : 48;
      center_title_avail = space > title_min ? space : title_min;
    }
  }

  // --- Left modules (limited by center group or right group) ---
  ModuleEntry scratch[MAX_MODULE_ENTRIES];
  char scratch_texts[MAX_MODULE_ENTRIES][256];
  char scratch_names[MAX_MODULE_ENTRIES][96];
  int stext_n = 0, sname_n = 0;
  int left_n = build_module_entries(bar, g_cfg.left_order, g_cfg.left_count,
                                    scratch, MAX_MODULE_ENTRIES,
                                    scratch_texts, scratch_names, &stext_n,
                                    &sname_n);
  // Fixed left modules keep their natural width; the window is flexible but
  // gets a small floor so it is squeezed instead of vanishing entirely.
  uint32_t fixed_left_w = 0;
  uint32_t title_w = 0;
  bool has_title = false;
  for (int i = 0; i < left_n; i++) {
    if (strcmp(scratch[i].module, "title") == 0) {
      title_w = module_entry_width(&scratch[i]);
      has_title = true;
    } else {
      fixed_left_w += module_entry_width(&scratch[i]);
    }
  }
  // The window is the only flexible module and absorbs the squeeze, so fixed
  // modules (which may come before or after the window) keep their natural
  // width whenever possible.
  uint32_t left_max = center_n > 0 ? center_left : right_group_left;
  uint32_t title_avail = 0;
  if (has_title) {
    uint32_t space = 0;
    if (left_max > g_rt->bar_left && left_max - g_rt->bar_left > fixed_left_w)
      space = left_max - g_rt->bar_left - fixed_left_w;
    uint32_t title_min = title_w < 48 ? title_w : 48;
    title_avail = space > title_min ? space : title_min;
  }

  uint32_t x = g_rt->bar_left;
  for (int i = 0; i < left_n && x < left_max; i++) {
    const char *text = scratch[i].text;
    char fit[256];
    if (strcmp(scratch[i].module, "title") == 0 && title_avail > 0 &&
        left_max > x) {
      uint32_t avail = left_max - x;
      if (avail > title_avail)
        avail = title_avail;
      uint32_t extra = scratch[i].st->pad_l + scratch[i].st->pad_r +
                       scratch[i].st->margin_l + scratch[i].st->margin_r;
      int tml = module_max_length("title");
      if (tml > 0 && avail > (uint32_t)tml + extra)
        avail = (uint32_t)tml + extra;
      fit_text_width(text, fit, sizeof(fit), avail, extra);
      text = fit;
    }
    ModuleEntry me = scratch[i];
    me.text = text;
    x = draw_module_entry(bar, &me, x, y, fg, fg_mask, bg, left_max, g_rt->bar_h);
  }
  uint32_t left_end = x;

  // Right group start: tray first, then right modules
  uint32_t right_start;
  if (center_n > 0) {
    // Right-aligned within the space to the right of the center group.
    right_start = center_right;
    uint32_t rg_left =
        right_edge > right_total_w ? right_edge - right_total_w : 0;
    if (rg_left > right_start)
      right_start = rg_left;
    if (right_start > right_edge)
      right_start = right_edge;
  } else {
    right_start = right_group_left;
    if (right_start < left_end)
      right_start = left_end;
    if (right_start > right_edge)
      right_start = right_edge;
  }

  // Middle background
  if (right_start > left_end) {
    uint32_t s = bar->scale > 0 ? (uint32_t)bar->scale : 1;
    pixman_image_fill_boxes(
        PIXMAN_OP_SRC, bg, bar->sel ? &g_rt->st_bar_sel.bg : &g_rt->st_bar.bg, 1,
        &(pixman_box32_t){.x1 = left_end * s, .x2 = right_start * s,
                          .y1 = g_rt->bar_top * s, .y2 = (g_rt->bar_top + g_rt->bar_h) * s});
  }

  // --- Center modules (drawn at the true screen center) ---
  if (center_n > 0) {
    uint32_t cx = center_left;
    for (int i = 0; i < center_n && cx < center_max; i++) {
      const char *text = center_ents[i].text;
      char fit[256];
      if (strcmp(center_ents[i].module, "title") == 0 &&
          center_title_avail > 0 && center_max > cx) {
        uint32_t ta = center_max - cx;
        if (ta > center_title_avail)
          ta = center_title_avail;
        uint32_t extra = center_ents[i].st->pad_l + center_ents[i].st->pad_r +
                         center_ents[i].st->margin_l +
                         center_ents[i].st->margin_r;
        int tml = module_max_length("title");
        if (tml > 0 && ta > (uint32_t)tml + extra)
          ta = (uint32_t)tml + extra;
        fit_text_width(text, fit, sizeof(fit), ta, extra);
        text = fit;
      }
      ModuleEntry me = center_ents[i];
      me.text = text;
      cx = draw_module_entry(bar, &me, cx, y, fg, fg_mask, bg, center_max,
                             g_rt->bar_h);
    }
  }

  // Draw right group
  uint32_t cur_x = right_start;
  for (int i = 0; i < right_n; i++) {
    if (cur_x >= right_edge)
      break;
    cur_x = draw_module_entry(bar, &right_ents[i], cur_x, y, fg, fg_mask, bg,
                              right_edge, g_rt->bar_h);
  }

  if (bar_bg_cr) {
    cairo_destroy(bar_bg_cr);
    bar_bg_cr = NULL;
  }

  // Composite final image
  pixman_image_composite32(PIXMAN_OP_OVER, bg, NULL, final, 0, 0, 0, 0, 0, 0,
                           bar->width, bar->height);
  pixman_image_set_alpha_map(fg, fg_mask, 0, 0);
  pixman_image_composite32(PIXMAN_OP_OVER, fg, fg_mask, final, 0, 0, 0, 0, 0, 0,
                           bar->width, bar->height);

  if (getenv("MANGOBAR_DUMPPNG") && (bar->atags || bar->ctags)) {
    cairo_surface_t *dumps = cairo_image_surface_create_for_data(
        (unsigned char *)data, CAIRO_FORMAT_ARGB32, bar->width, bar->height,
        bar->stride);
    cairo_surface_write_to_png(dumps, "/tmp/mangobar_bar.png");
    cairo_surface_destroy(dumps);
    fprintf(stderr, "DBG dumped atags=%x ctags=%x mtags=%x title='%s'\n",
            bar->atags, bar->ctags, bar->mtags, bar->title);
  }

  pixman_image_unref(fg);
  pixman_image_unref(fg_mask);
  pixman_image_unref(bg);
  pixman_image_unref(final);
  munmap(data, bar->bufsize);

  wl_surface_set_buffer_scale(bar->wl_surface, bar->scale);
  wl_surface_attach(bar->wl_surface, buf, 0, 0);
  wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->width, bar->height);
  wl_surface_commit(bar->wl_surface);
  IPC_LOG("[draw] %s committed\n", bar->name);
}

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t w, uint32_t h) {
  Bar *bar = data;
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  if (bar->configured && w == bar->logical_w && h == bar->logical_h)
    return;
  bar->logical_w = w;
  bar->logical_h = h;
  bar->width = w * (uint32_t)bar->scale;
  bar->height = h * (uint32_t)bar->scale;
  bar->stride = bar->width * 4;
  bar->bufsize = bar->stride * bar->height;
  bar->configured = true;
  draw_bar(bar);
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
  // The surface can be closed on TTY switches without the compositor
  // exiting, so only socket failures end the event loop.
  IPC_LOG("[wl] layer surface closed\n");
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void wl_output_geometry(void *data, struct wl_output *output, int32_t x,
                               int32_t y, int32_t phys_w, int32_t phys_h,
                               int32_t subpixel, const char *make,
                               const char *model, int32_t transform) {
}

static void wl_output_mode(void *data, struct wl_output *output,
                           uint32_t flags, int32_t width, int32_t height,
                           int32_t refresh) {
}

static void wl_output_done(void *data, struct wl_output *output) {
}

static void wl_output_scale(void *data, struct wl_output *output,
                            int32_t factor) {
  Bar *bar = data;
  bar->wl_scale = factor > 0 ? factor : 1;
  int bscale = bar->profile ? bar->profile->config.buffer_scale : 1;
  if (bscale < 1)
    bscale = 1;
  int scale = bar->wl_scale * bscale;
  if (scale == bar->scale)
    return;
  bar->scale = scale;
  if (bar->configured) {
    bar->width = bar->logical_w * (uint32_t)scale;
    bar->height = bar->logical_h * (uint32_t)scale;
    bar->stride = bar->width * 4;
    bar->bufsize = bar->stride * bar->height;
    draw_bar(bar);
  }
}

static const struct wl_output_listener wl_output_listener = {
    .geometry = wl_output_geometry,
    .mode = wl_output_mode,
    .done = wl_output_done,
    .scale = wl_output_scale,
};

// Force a profile's custom modules to run on the next refresh.
static void profile_custom_force_refresh(MangoConfigProfile *profile);

// Destroy the layer surface for a bar, if any.
static void bar_destroy_surface(Bar *bar) {
  if (bar->layer_surface) {
    zwlr_layer_surface_v1_destroy(bar->layer_surface);
    bar->layer_surface = NULL;
  }
  if (bar->wl_surface) {
    wl_surface_destroy(bar->wl_surface);
    bar->wl_surface = NULL;
  }
  bar->configured = false;
  bar->active = false;
}

// Create the layer surface for a bar after its profile has been matched.
static void bar_create_surface(Bar *bar) {
  const MangoConfig *cfg = &bar->profile->config;
  ProfileRuntime *rt = bar->rt;
  int bscale = cfg->buffer_scale > 0 ? cfg->buffer_scale : 1;
  // The compositor may have reported wl_output.scale before the xdg-output
  // name arrived; keep that factor instead of starting over from 1.
  bar->scale = bar->wl_scale * bscale;
  bar->tag_count = cfg->tag_count;
  bar->height = (rt->bar_h + rt->bar_top) * (uint32_t)bar->scale;
  bar->wl_surface = wl_compositor_create_surface(compositor);
  bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, bar->wl_surface, bar->wl_output, cfg->layer, "mangobar");
  zwlr_layer_surface_v1_add_listener(bar->layer_surface,
                                     &layer_surface_listener, bar);
  zwlr_layer_surface_v1_set_size(bar->layer_surface, 0,
                                 rt->bar_h + rt->bar_top);
  zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface,
                                           rt->bar_h + rt->bar_top);
  wl_surface_commit(bar->wl_surface);
  bar->active = true;
}

static void output_name_handler(void *data, struct zxdg_output_v1 *xdg_output,
                                const char *name) {
  Bar *bar = data;
  free(bar->name);
  bar->name = strdup(name);

  // Match a profile now that the stable output name is known.
  MangoConfigProfile *profile = NULL;
  MangoConfigMatch m = mango_config_set_match(&g_config_set, name, &profile);
  if (m == MANGO_CONFIG_MATCH_ENABLED && profile) {
    if (bar->active && bar->profile == profile)
      return; // repeated/unchanged name event; keep the running bar
    if (bar->active)
      bar_destroy_surface(bar); // output changed to another profile
    bar->profile = profile;
    bar->rt = &g_runtimes[profile - g_config_set.profiles];
    bar_create_surface(bar);
    profile_custom_force_refresh(profile);
  } else if (bar->active) {
    // The output stopped matching any profile: remove its bar.
    bar_destroy_surface(bar);
    bar->profile = NULL;
    bar->rt = NULL;
  } else {
    // Unmatched output: keep the listener record but no surface.
    bar->profile = NULL;
    bar->rt = NULL;
  }
}

static void output_logical_position(void *data,
                                    struct zxdg_output_v1 *xdg_output,
                                    int32_t x, int32_t y) {
  Bar *bar = data;
  bar->out_x = x;
  bar->out_y = y;
}

static void output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                                int32_t width, int32_t height) {
  Bar *bar = data;
  bar->out_w = width;
  bar->out_h = height;
}

static void output_done(void *data, struct zxdg_output_v1 *xdg_output) {
}

static void output_description(void *data, struct zxdg_output_v1 *xdg_output,
                               const char *description) {
}

static const struct zxdg_output_v1_listener output_listener = {
    .name = output_name_handler,
    .logical_position = output_logical_position,
    .logical_size = output_logical_size,
    .done = output_done,
    .description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  if (strcmp(interface, wl_compositor_interface.name) == 0)
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  else if (strcmp(interface, wl_shm_interface.name) == 0)
    shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
    layer_shell =
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
  else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
    output_manager =
        wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
    // wl_output may have been announced before the xdg-output manager.
    Bar *b;
    wl_list_for_each(b, &bar_list, link) {
      if (!b->xdg_output) {
        b->xdg_output =
            zxdg_output_manager_v1_get_xdg_output(output_manager, b->wl_output);
        zxdg_output_v1_add_listener(b->xdg_output, &output_listener, b);
      }
    }
  }
  else if (strcmp(interface, wl_seat_interface.name) == 0) {
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
    pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    // Defer surface creation until the xdg-output name arrives so the profile
    // can be matched first.
    Bar *bar = calloc(1, sizeof(Bar));
    bar->registry_name = name;
    bar->hover_tag = -1;
    bar->active = false;
    bar->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 2);
    wl_output_add_listener(bar->wl_output, &wl_output_listener, bar);
    if (output_manager) {
      bar->xdg_output =
          zxdg_output_manager_v1_get_xdg_output(output_manager, bar->wl_output);
      zxdg_output_v1_add_listener(bar->xdg_output, &output_listener, bar);
    }
    bar->scale = 1;
    bar->wl_scale = 1;
    bar->tag_count = MANGOBAR_MAX_TAGS;
    wl_list_insert(&bar_list, &bar->link);
  }
}

// Called when the menu/popup state references an output that is going away.
static void menu_output_removed(struct wl_output *output);

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
  Bar *bar, *tmp;
  wl_list_for_each_safe(bar, tmp, &bar_list, link) {
    if (bar->registry_name != name)
      continue;
    // Clear pointer state referencing this bar.
    if (pointer_bar == bar)
      pointer_bar = NULL;
    menu_output_removed(bar->wl_output);
    // Destroy Wayland resources.
    bar_destroy_surface(bar);
    if (bar->xdg_output)
      zxdg_output_v1_destroy(bar->xdg_output);
    if (bar->wl_output)
      wl_output_destroy(bar->wl_output);
    wl_list_remove(&bar->link);
    free(bar->name);
    free(bar);
    return;
  }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static Bar *find_bar(const char *name) {
  Bar *b;
  wl_list_for_each(b, &bar_list,
                   link) if (b->name && strcmp(b->name, name) == 0) return b;
  return NULL;
}

static void update_bar_json(Bar *bar, cJSON *json) {
  cJSON *item;
  IPC_LOG("[ipc] update %s start\n", bar->name);
  if ((item = cJSON_GetObjectItem(json, "hide_clients")) && cJSON_IsNumber(item))
    bar->hideclients = item->valueint;
  if ((item = cJSON_GetObjectItem(json, "active")))
    bar->sel = cJSON_IsTrue(item);
  if ((item = cJSON_GetObjectItem(json, "layout_symbol")))
    strncpy(bar->layout, item->valuestring, sizeof(bar->layout) - 1);
  if ((item = cJSON_GetObjectItem(json, "tag_num")) && cJSON_IsNumber(item)) {
    int n = item->valueint;
    if (n >= 1 && n <= MANGOBAR_MAX_TAGS)
      bar->tag_count = n;
  }

  cJSON *client = cJSON_GetObjectItem(json, "active_client");
  if (client && !cJSON_IsNull(client)) {
    cJSON *t = cJSON_GetObjectItem(client, "title");
    cJSON *a = cJSON_GetObjectItem(client, "appid");
    const char *title_str = (t && cJSON_IsString(t)) ? t->valuestring : "";
    const char *appid_str = (a && cJSON_IsString(a)) ? a->valuestring : "";
    // Keep the full title; pixel limits are applied when the module text is
    // built and drawn (truncate_utf8_string only guards the buffer boundary).
    truncate_utf8_string(bar->title, title_str, sizeof(bar->title), 250);
    snprintf(bar->appid, sizeof(bar->appid), "%s", appid_str);
  } else {
    bar->title[0] = '\0';
    bar->appid[0] = '\0';
  }

  cJSON *tags = cJSON_GetObjectItem(json, "tags");
  if (tags && cJSON_IsArray(tags)) {
    bar->mtags = bar->ctags = bar->urg = 0;
    cJSON *tobj;
    cJSON_ArrayForEach(tobj, tags) {
      int idx = cJSON_GetObjectItem(tobj, "index")->valueint - 1;
      if (idx < 0 || idx >= bar->tag_count)
        continue;
      if (cJSON_IsTrue(cJSON_GetObjectItem(tobj, "is_active")))
        bar->mtags |= 1 << idx;
      if (cJSON_IsTrue(cJSON_GetObjectItem(tobj, "is_urgent")))
        bar->urg |= 1 << idx;
      if (cJSON_GetObjectItem(tobj, "client_count")->valueint > 0)
        bar->ctags |= 1 << idx;
    }
  }

  // Parse active_tags into atags and overview_mode
  bar->atags = 0;
  bar->overview_mode = false; // default
  cJSON *active_tags = cJSON_GetObjectItem(json, "active_tags");
  if (active_tags && cJSON_IsArray(active_tags)) {
    int len = cJSON_GetArraySize(active_tags);
    if (len == 1) {
      cJSON *item0 = cJSON_GetArrayItem(active_tags, 0);
      if (cJSON_IsNumber(item0) && item0->valueint == 0) {
        bar->overview_mode = true;
      } else {
        int idx = item0->valueint - 1;
        if (idx >= 0 && idx < bar->tag_count)
          bar->atags |= (1 << idx);
      }
    } else {
      cJSON *elem;
      cJSON_ArrayForEach(elem, active_tags) {
        if (cJSON_IsNumber(elem)) {
          int idx = elem->valueint - 1;
          if (idx >= 0 && idx < bar->tag_count)
            bar->atags |= (1 << idx);
        }
      }
    }
  }

  bar->redraw = true;
  IPC_LOG("[ipc] update %s atags=%x ctags=%x mtags=%x title='%s'\n", bar->name,
          bar->atags, bar->ctags, bar->mtags, bar->title);
}

static void process_ipc_msg(const char *msg) {
  cJSON *json = cJSON_Parse(msg);
  if (!json) {
    IPC_LOG("[ipc] parse FAIL: %.100s\n", msg);
    return;
  }

  cJSON *monitors = cJSON_GetObjectItem(json, "monitors");
  if (monitors && cJSON_IsArray(monitors)) {
    IPC_LOG("[ipc] msg monitors=%d\n", cJSON_GetArraySize(monitors));
    cJSON *monitor;
    cJSON_ArrayForEach(monitor, monitors) {
      cJSON *name = cJSON_GetObjectItem(monitor, "name");
      if (cJSON_IsString(name)) {
        Bar *bar = find_bar(name->valuestring);
        if (bar)
          update_bar_json(bar, monitor);
      }
      cJSON *km = cJSON_GetObjectItem(monitor, "keymode");
      cJSON *kl = cJSON_GetObjectItem(monitor, "keyboardlayout");
      Bar *b;
      wl_list_for_each(b, &bar_list, link) {
        if (km)
          strncpy(b->keymode, km->valuestring, sizeof(b->keymode) - 1);
        if (kl)
          strncpy(b->kb_layout, kl->valuestring, sizeof(b->kb_layout) - 1);
        b->redraw = true;
      }
    }
  } else {
    cJSON *name = cJSON_GetObjectItem(json, "name");
    if (cJSON_IsString(name)) {
      Bar *bar = find_bar(name->valuestring);
      if (bar)
        update_bar_json(bar, json);
    }
    cJSON *km = cJSON_GetObjectItem(json, "keymode");
    cJSON *kl = cJSON_GetObjectItem(json, "keyboardlayout");
    if (km || kl) {
      Bar *b;
      wl_list_for_each(b, &bar_list, link) {
        if (km)
          strncpy(b->keymode, km->valuestring, sizeof(b->keymode) - 1);
        if (kl)
          strncpy(b->kb_layout, kl->valuestring, sizeof(b->kb_layout) - 1);
        b->redraw = true;
      }
    }
  }
  cJSON_Delete(json);
}

static void process_ipc_data() {
  char *line = ipc_buf, *end;
  while ((end = memchr(line, '\n', ipc_buf_len - (line - ipc_buf)))) {
    *end = '\0';
    process_ipc_msg(line);
    line = end + 1;
  }
  size_t rem = ipc_buf_len - (line - ipc_buf);
  memmove(ipc_buf, line, rem);
  ipc_buf_len = rem;
}

static void ipc_connect() {
  const char *path = getenv("MANGO_INSTANCE_SIGNATURE");
  if (!path) {
    IPC_LOG("[ipc] connect: MANGO_INSTANCE_SIGNATURE not set\n");
    return;
  }
  ipc_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (ipc_fd < 0) {
    IPC_LOG("[ipc] connect: socket failed\n");
    return;
  }
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  if (connect(ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    IPC_LOG("[ipc] connect: connect %s failed\n", path);
    close(ipc_fd);
    ipc_fd = -1;
    return;
  }
  fcntl(ipc_fd, F_SETFL, fcntl(ipc_fd, F_GETFL) | O_NONBLOCK);
  IPC_LOG("[ipc] connect ok fd=%d path=%s\n", ipc_fd, path);
}

static void ipc_subscribe(void) {
  if (ipc_fd < 0)
    return;
  const char *msg = "watch all-monitors\n";
  ssize_t n = send(ipc_fd, msg, strlen(msg), MSG_NOSIGNAL);
  IPC_LOG("[ipc] subscribe sent=%zd\n", n);
}

static void ipc_send_command(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // Use a fresh connection for commands.
  const char *path = getenv("MANGO_INSTANCE_SIGNATURE");
  if (!path) {
    IPC_LOG("[cmd] no socket path, cmd='%s'\n", buf);
    return;
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    IPC_LOG("[cmd] socket failed, cmd='%s'\n", buf);
    return;
  }
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    IPC_LOG("[cmd] connect failed, cmd='%s'\n", buf);
    close(fd);
    return;
  }
  // Don't block the event loop: wait at most 200ms for the response
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ssize_t sn = send(fd, buf, strlen(buf), MSG_NOSIGNAL);
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, 200);
  char tmp[256];
  ssize_t rn, total = 0;
  if (pr > 0)
    while ((rn = recv(fd, tmp, sizeof(tmp), 0)) > 0)
      total += rn;
  else
    rn = pr;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
              (t1.tv_nsec - t0.tv_nsec) / 1e6;
  IPC_LOG("[cmd] '%s' sent=%zd poll=%d recv=%zd total=%d %.2fms\n", buf, sn,
          pr, rn, (int)total, ms);
  close(fd);
}

static void run_command(const char *cmd) {
  if (!cmd || !*cmd)
    return;
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 0);
      dup2(devnull, 1);
      dup2(devnull, 2);
      if (devnull > 2)
        close(devnull);
    }
    // A Wayland bar must not force X11 toolkits onto spawned GUI apps
    // (e.g. GDK_BACKEND=x11 makes wlogout/satty fall back to XWayland).
    if (getenv("WAYLAND_DISPLAY")) {
      const char *gdk = getenv("GDK_BACKEND");
      if (gdk && strstr(gdk, "x11") && !strstr(gdk, "wayland"))
        unsetenv("GDK_BACKEND");
    }
    // Don't leak mangobar's sockets/pipes into GUI children.
#ifdef SYS_close_range
    syscall(SYS_close_range, 3, ~0U, 0);
#else
    for (int fd = 3; fd < 1024; fd++)
      close(fd);
#endif
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
}

// ---------- Module actions ----------
static const MangoAction *find_action(const char *module) {
  for (int i = 0; i < g_cfg.action_count; i++)
    if (strcmp(g_cfg.actions[i].module, module) == 0)
      return &g_cfg.actions[i];
  return NULL;
}

// Per-module scroll debounce: scrolls inside the interval reset the timer,
// so a continuous scroll only triggers the first event.
static uint64_t now_ms(void);
#define MAX_SCROLL_TRACK 128
static char scroll_track_module[MAX_SCROLL_TRACK][32];
static uint64_t scroll_track_last[MAX_SCROLL_TRACK];
static int scroll_track_count;

static bool scroll_debounced(const char *module) {
  int interval = g_cfg.scroll_interval;
  const MangoAction *ma = find_action(module);
  if (ma && ma->scroll_interval >= 0)
    interval = ma->scroll_interval;
  if (interval <= 0)
    return false;
  uint64_t now = now_ms();
  for (int i = 0; i < scroll_track_count; i++) {
    if (strcmp(scroll_track_module[i], module) == 0) {
      bool blocked = now - scroll_track_last[i] < (uint64_t)interval;
      scroll_track_last[i] = now; // keep resetting while scrolling
      return blocked;
    }
  }
  if (scroll_track_count < MAX_SCROLL_TRACK) {
    snprintf(scroll_track_module[scroll_track_count], 32, "%s", module);
    scroll_track_last[scroll_track_count] = now;
    scroll_track_count++;
  }
  return false;
}

static void handle_module_action(Bar *bar, const char *module, int tag,
                                 uint32_t button) {
  if (bar->profile)
    g_cfg_ptr = &bar->profile->config;
  // Left click toggles format-alt when the module defines one.
  int ai = alt_index(module);
  if (ai >= 0 && button == BTN_LEFT) {
    bar->alt_on[ai] = !bar->alt_on[ai];
    bar->redraw = true;
    IPC_LOG("[click] %s alt=%d\n", module, bar->alt_on[ai]);
    if (strcmp(module, "network") == 0)
      sys_refresh = true; // start/refresh speed sampling promptly
  }
  const MangoAction *ma = find_action(module);
  if (!ma)
    return;
  const char *cmd = NULL;
  if (button == 4)
    cmd = ma->scroll_up;
  else if (button == 5)
    cmd = ma->scroll_down;
  else if (button == BTN_LEFT)
    cmd = ma->left;
  else if (button == BTN_RIGHT)
    cmd = ma->right;
  else if (button == BTN_MIDDLE)
    cmd = ma->middle;
  if (!cmd)
    return;
  IPC_LOG("[click] module=%s tag=%d button=%u cmd='%s'\n", module, tag, button,
          cmd);
  if ((button == 4 || button == 5) && scroll_debounced(module))
    return;

  if (strcmp(module, "tags") == 0 && tag < 0) {
    // Overview button
    if (strcmp(cmd, "@view") == 0 || strcmp(cmd, "@toggle") == 0) {
      ipc_send_command("dispatch toggleoverview\n");
      return;
    }
  }

  if (tag >= 0 && strcmp(module, "tags") == 0) {
    if (strcmp(cmd, "@view") == 0) {
      ipc_send_command("dispatch view,%d\n", tag + 1);
      return;
    }
    if (strcmp(cmd, "@toggle") == 0) {
      ipc_send_command("dispatch toggleview,%d\n", tag + 1);
      return;
    }
    if (strcmp(cmd, "@tag") == 0) {
      ipc_send_command("dispatch tag,%d\n", tag + 1);
      return;
    }
    if (strcmp(cmd, "@toggletag") == 0) {
      ipc_send_command("dispatch toggletag,%d\n", tag + 1);
      return;
    }
  }
  if (strncmp(cmd, "@ipc:", 5) == 0) {
    ipc_send_command("%s\n", cmd + 5);
    return;
  }
  run_command(cmd);
  // Refresh immediately after brightness/volume changes
  if (strcmp(module, "brightness") == 0 || strcmp(module, "volume") == 0)
    sys_refresh = true;
}

static void menu_open(Bar *bar, MangobarTrayItem *item, double lx, double ly);
static void tray_right_click(Bar *bar, double x, double y);

static double smooth_scroll_threshold_at(const Bar *bar, double x) {
  if (bar->profile)
    g_cfg_ptr = &bar->profile->config;
  for (int i = 0; i < bar->hotspot_count; i++) {
    const Hotspot *h = &bar->hotspots[i];
    if (x >= h->x1 && x < h->x2) {
      const MangoAction *ma = find_action(h->module);
      if (ma && ma->smooth_scroll_threshold > 0.0)
        return ma->smooth_scroll_threshold;
      break;
    }
  }
  return g_cfg.smooth_scroll_threshold;
}

static void dispatch_pointer_event(Bar *bar, double x, double y,
                                   uint32_t button) {
  if (!bar)
    return;
  if (bar->profile)
    g_cfg_ptr = &bar->profile->config;
  for (int i = 0; i < bar->tray_hotspot_count; i++) {
    TrayHotspot *h = &bar->tray_hotspots[i];
    if (x >= h->x1 && x < h->x2) {
      if (button == BTN_RIGHT) {
        tray_right_click(bar, x, y);
      } else {
        // Other clicks get global coordinates
        tray_handle_click(tray, h->item, bar->out_x + x, bar->out_y + y,
                          button);
      }
      return;
    }
  }
  for (int i = 0; i < bar->hotspot_count; i++) {
    Hotspot *h = &bar->hotspots[i];
    if (x >= h->x1 && x < h->x2) {
      handle_module_action(bar, h->module, h->tag, button);
      return;
    }
  }
  handle_module_action(bar, "bar", -1, button);
}

// ---------- Tray DBusMenu popup ----------
#define MENU_PAD_H 10
#define MENU_PAD_V 4

typedef struct {
  MangobarMenu *menu;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  int scale; // buffer scale of the output the menu is on
  uint32_t width, height; // logical size
  uint32_t buf_width, buf_height, stride, bufsize;
  int item_h;
  int hover; // hovered row (incl. back row), -1 = none
  uint64_t hover_ms; // hover start time (ms) for delayed submenu open
  uint64_t outside_since; // time pointer left our surfaces; 0 = inside
  int lx, ly; // trigger position (output-local coords)
  int bar_h; // bar height (logical)
  int out_w, out_h; // output size for bounds clamping
  MangobarTrayItem *item; // associated tray item
  struct wl_output *output;
  // Side submenu
  struct wl_surface *sub_surface;
  struct zwlr_layer_surface_v1 *sub_layer_surface;
  uint32_t sub_width, sub_height; // logical size
  uint32_t sub_buf_width, sub_buf_height, sub_stride, sub_bufsize;
  int sub_hover;
  int margin_left, margin_top; // main menu position in the output
  const MangobarMenuNode *sub_node;
  bool sub_open;
  bool sub_configured;
  // Fullscreen transparent grab layer: catches outside clicks
  struct wl_surface *grab_surface;
  struct zwlr_layer_surface_v1 *grab_layer_surface;
  struct wl_buffer *grab_buffer;
  uint32_t grab_width, grab_height, grab_stride, grab_bufsize;
  // Pending open when the Menu property isn't loaded yet
  MangobarTrayItem *pending_item;
  Bar *pending_bar;
  int pending_x, pending_y;
  uint64_t pending_ms;
  bool configured;
  bool open;
} MenuPopup;
static MenuPopup popup;
static bool popup_pointer; // whether pointer is on the popup
static bool pointer_on_sub; // whether pointer is on the submenu

static void draw_menu_popup(void);
static void menu_layout_cb(void *data);
static void submenu_destroy_surface(void);
static void submenu_close(void);
static void submenu_open(const MangobarMenuNode *node);
static void grab_create(Bar *bar);
static void grab_destroy(void);

static void menu_close(void) {
  if (!popup.open)
    return;
  grab_destroy();
  submenu_destroy_surface();
  if (popup.layer_surface)
    zwlr_layer_surface_v1_destroy(popup.layer_surface);
  if (popup.surface)
    wl_surface_destroy(popup.surface);
  menu_destroy(popup.menu);
  memset(&popup, 0, sizeof(popup));
  popup_pointer = false;
  pointer_on_sub = false;
}

static void menu_output_removed(struct wl_output *output) {
  if (popup.pending_bar && popup.pending_bar->wl_output == output) {
    popup.pending_item = NULL;
    popup.pending_bar = NULL;
  }
  if (popup.open && popup.output == output)
    menu_close();
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// Right-click a tray item: open menu, or queue a retry if not ready
static void tray_right_click(Bar *bar, double x, double y) {
  for (int i = 0; i < bar->tray_hotspot_count; i++) {
    TrayHotspot *h = &bar->tray_hotspots[i];
    if (x >= h->x1 && x < h->x2) {
      if (tray_item_has_menu(h->item)) {
        menu_open(bar, h->item, x, y);
      } else {
        popup.pending_item = h->item;
        popup.pending_bar = bar;
        popup.pending_x = (int)x;
        popup.pending_y = (int)y;
        popup.pending_ms = now_ms();
        tray_refresh(tray);
      }
      return;
    }
  }
}

static void update_menu_hover(double y) {
  int row = popup.item_h > 0 ? (int)(y / popup.item_h) : -1;
  int rows = menu_visible_count(popup.menu) +
             (menu_in_submenu(popup.menu) ? 1 : 0);
  if (row >= rows)
    row = -1;
  if (row != popup.hover) {
    popup.hover = row;
    popup.hover_ms = now_ms();
    if (row < 0) {
      submenu_close();
    } else {
      const MangobarMenuNode *n = menu_visible_node(popup.menu, row);
      if (!n || n->child_count == 0)
        submenu_close();
    }
    if (popup.configured)
      draw_menu_popup();
  }
}

// Auto-open a submenu after hovering ~300ms
static void menu_hover_tick(void) {
  if (!popup.open || !popup_pointer || pointer_on_sub || popup.hover < 0)
    return;
  const MangobarMenuNode *n = menu_visible_node(popup.menu, popup.hover);
  if (popup.sub_open && popup.sub_node != n)
    submenu_close();
  if (!n || n->child_count == 0)
    return;
  if (popup.sub_open && popup.sub_node == n)
    return;
  if (now_ms() - popup.hover_ms < 300)
    return;
  submenu_open(n);
}

static void menu_activate_row(int row) {
  const MangobarMenuNode *n = menu_visible_node(popup.menu, row);
  if (!n)
    return;
  if (n->child_count > 0) {
    submenu_open(n);
    return;
  }
  if (!n->enabled)
    return;
  menu_activate(popup.menu, n, (uint32_t)now_ms());
  menu_close();
}

// Build a Pango font from the fcft font string
static PangoFontDescription *build_menu_font(void) {
  PangoFontDescription *fd = pango_font_description_new();
  char fam[256] = "Sans";
  int size = 12;
  bool bold = false;
  const char *p = g_style_sheet.font_family[0] ? g_style_sheet.font_family
                                               : g_config_set.profiles[0].config.font;
  const char *colon = strchr(p, ':');
  if (colon) {
    size_t n = (size_t)(colon - p);
    if (n > 0 && n < sizeof(fam)) {
      memcpy(fam, p, n);
      fam[n] = '\0';
    }
    p = colon;
  } else {
    snprintf(fam, sizeof(fam), "%s", p);
    p = p + strlen(p);
  }
  while (*p) {
    if (*p == ':')
      p++;
    const char *eq = strchr(p, ':');
    size_t tlen = eq ? (size_t)(eq - p) : strlen(p);
    char tok[64];
    size_t tl = tlen < 63 ? tlen : 63;
    memcpy(tok, p, tl);
    tok[tl] = '\0';
    p += tlen;
    char *eqc = strchr(tok, '=');
    if (eqc) {
      *eqc = '\0';
      const char *k = tok;
      const char *v = eqc + 1;
      if (strcmp(k, "size") == 0)
        size = atoi(v);
      else if (strcmp(k, "style") == 0 || strcmp(k, "weight") == 0) {
        if (strcasestr(v, "bold"))
          bold = true;
      }
    }
  }
  if (g_style_sheet.menu_font_size > 0)
    size = g_style_sheet.menu_font_size;
  if (g_style_sheet.font_weight[0] &&
      strcasestr(g_style_sheet.font_weight, "bold"))
    bold = true;
  if (size <= 0)
    size = 12;
  pango_font_description_set_family(fd, fam);
  pango_font_description_set_size(fd, size * PANGO_SCALE);
  if (bold)
    pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
  return fd;
}

static void menu_draw_text(cairo_t *cr, const char *text, double x, double y,
                           double r, double g, double b) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = build_menu_font();
  pango_layout_set_font_description(layout, fd);
  pango_font_description_free(fd);
  pango_layout_set_text(layout, text, -1);
  cairo_set_source_rgb(cr, r, g, b);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
}

static int pango_text_width(const char *text);

// Width reserved for a state indicator column (checkmark/radio), cached.
static int menu_indicator_col(void) {
  static int w = -1;
  if (w < 0) {
    int a = pango_text_width("✓");
    int b = pango_text_width("●");
    w = (a > b ? a : b) + 6;
  }
  return w;
}

// State indicator glyph for toggle/radio menu items, or NULL if none.
static const char *menu_state_glyph(const MangobarMenuNode *n) {
  if (!n)
    return NULL;
  const char *tt = n->toggle_type ? n->toggle_type : n->type;
  if (!tt)
    return NULL;
  if (strcmp(tt, "checkmark") == 0 || strcmp(tt, "toggle") == 0)
    return n->toggle_state > 0 ? "✓" : NULL;
  if (strcmp(tt, "radio") == 0)
    return n->toggle_state > 0 ? "●" : "○";
  return NULL;
}

// Whether a menu item reserves an indicator column (stable layout when the
// state changes between checked/unchecked).
static int menu_indicator_gap(const MangobarMenuNode *n) {
  if (!n)
    return 0;
  const char *tt = n->toggle_type ? n->toggle_type : n->type;
  if (!tt)
    return 0;
  if (strcmp(tt, "checkmark") == 0 || strcmp(tt, "toggle") == 0 ||
      strcmp(tt, "radio") == 0)
    return menu_indicator_col();
  return 0;
}

static void argb_to_rgb(uint32_t c, double *r, double *g, double *b) {
  // Colors are stored as 0xRRGGBBAA (see hex_to_pixman)
  *r = ((c >> 24) & 0xff) / 255.0;
  *g = ((c >> 16) & 0xff) / 255.0;
  *b = ((c >> 8) & 0xff) / 255.0;
}

static void draw_menu_popup(void) {
  if (!popup.open || !popup.configured)
    return;
  int fd = allocate_shm_file(popup.bufsize);
  if (fd < 0)
    return;
  uint32_t *data = mmap(NULL, popup.bufsize, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, popup.bufsize);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      pool, 0, popup.buf_width, popup.buf_height, popup.stride,
      WL_SHM_FORMAT_ARGB8888);
  wl_buffer_add_listener(buf, &wl_buffer_listener, NULL);
  wl_shm_pool_destroy(pool);
  close(fd);

  cairo_surface_t *cs = cairo_image_surface_create_for_data(
      (unsigned char *)data, CAIRO_FORMAT_ARGB32, popup.buf_width,
      popup.buf_height, popup.stride);
  cairo_t *cr = cairo_create(cs);
  cairo_scale(cr, popup.scale, popup.scale);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

  double rad = g_style_sheet.menu_radius > 0 ? g_style_sheet.menu_radius : 10;
  double bg_r, bg_g, bg_b, bd_r, bd_g, bd_b;
  double it_r, it_g, it_b, hov_r, hov_g, hov_b;
  double hovbg_r, hovbg_g, hovbg_b;
  if (g_style_sheet.menu.background_set)
    argb_to_rgb(g_style_sheet.menu.background, &bg_r, &bg_g, &bg_b);
  else {
    bg_r = 0.125;
    bg_g = 0.106;
    bg_b = 0.078;
  }
  if (g_style_sheet.menu.border_color_set)
    argb_to_rgb(g_style_sheet.menu.border_color, &bd_r, &bd_g, &bd_b);
  else {
    bd_r = 0.27;
    bd_g = 0.27;
    bd_b = 0.27;
  }
  if (g_style_sheet.menuitem.color_set)
    argb_to_rgb(g_style_sheet.menuitem.color, &it_r, &it_g, &it_b);
  else {
    it_r = 0.78;
    it_g = 0.54;
    it_b = 0.58;
  }
  if (g_style_sheet.menuitem_hover.color_set)
    argb_to_rgb(g_style_sheet.menuitem_hover.color, &hov_r, &hov_g, &hov_b);
  else {
    hov_r = it_r;
    hov_g = it_g;
    hov_b = it_b;
  }
  if (g_style_sheet.menuitem_hover.background_set)
    argb_to_rgb(g_style_sheet.menuitem_hover.background, &hovbg_r, &hovbg_g,
                &hovbg_b);
  else {
    hovbg_r = 0.23;
    hovbg_g = 0.17;
    hovbg_b = 0.16;
  }
  // Background (rounded)
  cairo_rounded_rect(cr, 0, 0, popup.width, popup.height, rad);
  cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
  cairo_fill(cr);
  // Border (rounded stroke)
  cairo_rounded_rect(cr, 0.5, 0.5, popup.width - 1, popup.height - 1, rad);
  cairo_set_source_rgb(cr, bd_r, bd_g, bd_b);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  double y = MENU_PAD_V;
  int vis = menu_visible_count(popup.menu);
  bool in_sub = menu_in_submenu(popup.menu);

  if (in_sub) {
    bool hov = popup.hover == 0;
    if (hov) {
      cairo_rounded_rect(cr, 1, y, popup.width - 2, popup.item_h, rad - 2);
      cairo_set_source_rgb(cr, hovbg_r, hovbg_g, hovbg_b);
      cairo_fill(cr);
    }
    menu_draw_text(cr, "← 返回", MENU_PAD_H, y + 2, hov ? hov_r : it_r,
                   hov ? hov_g : it_g, hov ? hov_b : it_b);
    y += popup.item_h;
  }
  for (int i = 0; i < vis; i++) {
    const MangobarMenuNode *n = menu_visible_node(popup.menu, i);
    int row = in_sub ? i + 1 : i;
    bool hov = popup.hover == row;
    if (hov) {
      cairo_rounded_rect(cr, 1, y, popup.width - 2, popup.item_h, rad - 2);
      cairo_set_source_rgb(cr, hovbg_r, hovbg_g, hovbg_b);
      cairo_fill(cr);
    }
    const char *label = n->label ? n->label : "";
    double lr, lg, lb;
    if (n->enabled) {
      lr = hov ? hov_r : it_r;
      lg = hov ? hov_g : it_g;
      lb = hov ? hov_b : it_b;
    } else {
      lr = lg = lb = 0.5;
    }
    int ig = menu_indicator_gap(n);
    double tx = MENU_PAD_H + ig;
    const char *glyph = menu_state_glyph(n);
    if (glyph)
      menu_draw_text(cr, glyph, MENU_PAD_H, y + 2, lr, lg, lb);
    menu_draw_text(cr, label, tx, y + 2, lr, lg, lb);
    if (n->child_count > 0) {
      PangoLayout *l = pango_cairo_create_layout(cr);
      PangoFontDescription *fd = build_menu_font();
      pango_layout_set_font_description(l, fd);
      pango_font_description_free(fd);
      pango_layout_set_text(l, label, -1);
      int tw, th;
      pango_layout_get_pixel_size(l, &tw, &th);
      g_object_unref(l);
      menu_draw_text(cr, "▶", tx + tw + 8, y + 2, it_r, it_g, it_b);
    }
    y += popup.item_h;
  }

  cairo_destroy(cr);
  cairo_surface_destroy(cs);
  munmap(data, popup.bufsize);

  wl_surface_attach(popup.surface, buf, 0, 0);
  wl_surface_damage_buffer(popup.surface, 0, 0, popup.buf_width,
                           popup.buf_height);
  wl_surface_commit(popup.surface);
}

static void popup_layer_configure(void *data,
                                  struct zwlr_layer_surface_v1 *surface,
                                  uint32_t serial, uint32_t w, uint32_t h) {
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  popup.width = w;
  popup.height = h;
  uint32_t s = popup.scale > 0 ? (uint32_t)popup.scale : 1;
  popup.buf_width = w * s;
  popup.buf_height = h * s;
  popup.stride = popup.buf_width * 4;
  popup.bufsize = popup.stride * popup.buf_height;
  popup.configured = true;
  draw_menu_popup();
}

static void popup_layer_closed(void *data,
                               struct zwlr_layer_surface_v1 *surface) {
  menu_close();
}

static const struct zwlr_layer_surface_v1_listener popup_layer_listener = {
    .configure = popup_layer_configure,
    .closed = popup_layer_closed,
};

// Measure text width with pango (matches menu drawing)
static int pango_text_width(const char *text) {
  if (!text || !*text)
    return 0;
  cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *cr = cairo_create(cs);
  PangoLayout *l = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = build_menu_font();
  pango_layout_set_font_description(l, fd);
  pango_font_description_free(fd);
  pango_layout_set_text(l, text, -1);
  int w, h;
  pango_layout_get_pixel_size(l, &w, &h);
  g_object_unref(l);
  cairo_destroy(cr);
  cairo_surface_destroy(cs);
  return w;
}

// Called after GetLayout: compute size, clamp position, redraw
static void menu_layout_cb(void *data) {
  if (!popup.open || !popup.menu)
    return;
  int vis = menu_visible_count(popup.menu);
  if (vis <= 0) {
    // Menu not ready; refresh and close.
    if (tray)
      tray_refresh(tray);
    menu_close();
    return;
  }
  int item_h = font->ascent + font->descent + 6;
  if (item_h < 20)
    item_h = 20;
  popup.item_h = item_h;
  uint32_t w = MENU_PAD_H * 2 + 8;
  for (int i = 0; i < vis; i++) {
    const MangobarMenuNode *n = menu_visible_node(popup.menu, i);
    if (n->label) {
      int tw = pango_text_width(n->label);
      int need = tw + menu_indicator_gap(n) + MENU_PAD_H * 2 +
                 (n->child_count > 0 ? 20 : 8);
      if (need > (int)w)
        w = (uint32_t)need;
    }
  }
  int rows = vis + (menu_in_submenu(popup.menu) ? 1 : 0);
  uint32_t h = (uint32_t)rows * item_h + MENU_PAD_V * 2;

  // Menu hugs below the systray (no overlap), right-aligned to the click
  int ml = popup.lx - (int)w;
  // Position below the systray with the desired gap.
  int mt = 10;
  if (mt < 0)
    mt = 0;
  if (ml < 0)
    ml = 0;
  if (popup.out_w > 0 && ml + (int)w > popup.out_w)
    ml = popup.out_w - (int)w;
  if (mt < 0)
    mt = 0;
  if (popup.out_h > 0 && mt + (int)h > popup.out_h)
    mt = popup.out_h - (int)h;
  zwlr_layer_surface_v1_set_margin(popup.layer_surface, mt, 0, 0, ml);
  zwlr_layer_surface_v1_set_size(popup.layer_surface, w, h);
  wl_surface_commit(popup.surface);
  popup.margin_left = ml;
  popup.margin_top = mt;
  IPC_LOG("[menu] layout bar_h=%d mt=%d ml=%d w=%u h=%u\n", popup.bar_h, mt,
          ml, w, h);
}

// ---------- Submenu (side popup) ----------
static void submenu_destroy_surface(void) {
  if (popup.sub_layer_surface)
    zwlr_layer_surface_v1_destroy(popup.sub_layer_surface);
  if (popup.sub_surface)
    wl_surface_destroy(popup.sub_surface);
  popup.sub_layer_surface = NULL;
  popup.sub_surface = NULL;
  popup.sub_open = false;
  popup.sub_configured = false;
  popup.sub_node = NULL;
  popup.sub_hover = -1;
}

static void submenu_close(void) {
  if (!popup.open || !popup.sub_open)
    return;
  submenu_destroy_surface();
}

static void draw_submenu(void) {
  if (!popup.sub_open || !popup.sub_configured || !popup.sub_node)
    return;
  int fd = allocate_shm_file(popup.sub_bufsize);
  if (fd < 0)
    return;
  uint32_t *data = mmap(NULL, popup.sub_bufsize, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    close(fd);
    return;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, popup.sub_bufsize);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(
      pool, 0, popup.sub_buf_width, popup.sub_buf_height, popup.sub_stride,
      WL_SHM_FORMAT_ARGB8888);
  wl_buffer_add_listener(buf, &wl_buffer_listener, NULL);
  wl_shm_pool_destroy(pool);
  close(fd);

  cairo_surface_t *cs = cairo_image_surface_create_for_data(
      (unsigned char *)data, CAIRO_FORMAT_ARGB32, popup.sub_buf_width,
      popup.sub_buf_height, popup.sub_stride);
  cairo_t *cr = cairo_create(cs);
  cairo_scale(cr, popup.scale, popup.scale);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

  double rad = g_style_sheet.menu_radius > 0 ? g_style_sheet.menu_radius : 10;
  double bg_r, bg_g, bg_b, bd_r, bd_g, bd_b;
  double it_r, it_g, it_b, hov_r, hov_g, hov_b, hovbg_r, hovbg_g, hovbg_b;
  if (g_style_sheet.menu.background_set)
    argb_to_rgb(g_style_sheet.menu.background, &bg_r, &bg_g, &bg_b);
  else {
    bg_r = 0.125;
    bg_g = 0.106;
    bg_b = 0.078;
  }
  if (g_style_sheet.menu.border_color_set)
    argb_to_rgb(g_style_sheet.menu.border_color, &bd_r, &bd_g, &bd_b);
  else {
    bd_r = 0.27;
    bd_g = 0.27;
    bd_b = 0.27;
  }
  if (g_style_sheet.menuitem.color_set)
    argb_to_rgb(g_style_sheet.menuitem.color, &it_r, &it_g, &it_b);
  else {
    it_r = 0.78;
    it_g = 0.54;
    it_b = 0.58;
  }
  if (g_style_sheet.menuitem_hover.color_set)
    argb_to_rgb(g_style_sheet.menuitem_hover.color, &hov_r, &hov_g, &hov_b);
  else {
    hov_r = it_r;
    hov_g = it_g;
    hov_b = it_b;
  }
  if (g_style_sheet.menuitem_hover.background_set)
    argb_to_rgb(g_style_sheet.menuitem_hover.background, &hovbg_r, &hovbg_g,
                &hovbg_b);
  else {
    hovbg_r = 0.23;
    hovbg_g = 0.17;
    hovbg_b = 0.16;
  }

  cairo_rounded_rect(cr, 0, 0, popup.sub_width, popup.sub_height, rad);
  cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
  cairo_fill(cr);
  cairo_rounded_rect(cr, 0.5, 0.5, popup.sub_width - 1, popup.sub_height - 1,
                     rad);
  cairo_set_source_rgb(cr, bd_r, bd_g, bd_b);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  int vis = menu_node_visible_count(popup.sub_node);
  double y = MENU_PAD_V;
  for (int i = 0; i < vis; i++) {
    const MangobarMenuNode *n = menu_node_visible_node(popup.sub_node, i);
    bool hov = popup.sub_hover == i;
    if (hov) {
      cairo_rounded_rect(cr, 1, y, popup.sub_width - 2, popup.item_h, rad - 2);
      cairo_set_source_rgb(cr, hovbg_r, hovbg_g, hovbg_b);
      cairo_fill(cr);
    }
    const char *label = n ? n->label : "";
    double lr, lg, lb;
    if (n && n->enabled) {
      lr = hov ? hov_r : it_r;
      lg = hov ? hov_g : it_g;
      lb = hov ? hov_b : it_b;
    } else {
      lr = lg = lb = 0.5;
    }
    int ig = menu_indicator_gap(n);
    double tx = MENU_PAD_H + ig;
    const char *glyph = menu_state_glyph(n);
    if (glyph)
      menu_draw_text(cr, glyph, MENU_PAD_H, y + 2, lr, lg, lb);
    menu_draw_text(cr, label, tx, y + 2, lr, lg, lb);
    y += popup.item_h;
  }

  cairo_destroy(cr);
  cairo_surface_destroy(cs);
  munmap(data, popup.sub_bufsize);

  wl_surface_attach(popup.sub_surface, buf, 0, 0);
  wl_surface_damage_buffer(popup.sub_surface, 0, 0, popup.sub_buf_width,
                           popup.sub_buf_height);
  wl_surface_commit(popup.sub_surface);
}

static void submenu_layer_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t w, uint32_t h) {
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  popup.sub_width = w;
  popup.sub_height = h;
  uint32_t s = popup.scale > 0 ? (uint32_t)popup.scale : 1;
  popup.sub_buf_width = w * s;
  popup.sub_buf_height = h * s;
  popup.sub_stride = popup.sub_buf_width * 4;
  popup.sub_bufsize = popup.sub_stride * popup.sub_buf_height;
  popup.sub_configured = true;
  draw_submenu();
}

static void submenu_layer_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
  submenu_destroy_surface();
}

static const struct zwlr_layer_surface_v1_listener submenu_layer_listener = {
    .configure = submenu_layer_configure,
    .closed = submenu_layer_closed,
};

static void submenu_open(const MangobarMenuNode *node) {
  if (!popup.open || !node || node->child_count == 0)
    return;
  if (popup.sub_open && popup.sub_node == node)
    return;
  submenu_destroy_surface();

  int vis = menu_node_visible_count(node);
  if (vis <= 0)
    return;
  uint32_t w = MENU_PAD_H * 2 + 8;
  for (int i = 0; i < vis; i++) {
    const MangobarMenuNode *n = menu_node_visible_node(node, i);
    if (n && n->label) {
      int tw = pango_text_width(n->label);
      int need = tw + menu_indicator_gap(n) + MENU_PAD_H * 2 + 8;
      if (need > (int)w)
        w = (uint32_t)need;
    }
  }
  uint32_t h = (uint32_t)vis * popup.item_h + MENU_PAD_V * 2;

  int ml = popup.margin_left + (int)popup.width + 4;
  int mt = popup.margin_top + MENU_PAD_V +
           (popup.hover >= 0 ? popup.hover : 0) * popup.item_h;
  if (popup.out_w > 0 && ml + (int)w > popup.out_w) {
    ml = popup.margin_left - (int)w - 4;
    if (ml < 0)
      ml = 0;
  }
  if (popup.out_h > 0 && mt + (int)h > popup.out_h) {
    mt = popup.out_h - (int)h - 8;
    if (mt < 0)
      mt = 0;
  }

  popup.sub_node = node;
  popup.sub_hover = -1;
  popup.sub_surface = wl_compositor_create_surface(compositor);
  wl_surface_set_buffer_scale(popup.sub_surface, popup.scale);
  popup.sub_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, popup.sub_surface, popup.output,
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mangobar-menu-sub");
  zwlr_layer_surface_v1_add_listener(popup.sub_layer_surface,
                                     &submenu_layer_listener, NULL);
  zwlr_layer_surface_v1_set_anchor(
      popup.sub_layer_surface,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  zwlr_layer_surface_v1_set_margin(popup.sub_layer_surface, mt, 0, 0, ml);
  zwlr_layer_surface_v1_set_size(popup.sub_layer_surface, w, h);
  zwlr_layer_surface_v1_set_exclusive_zone(popup.sub_layer_surface, 0);
  wl_surface_commit(popup.sub_surface);
  popup.sub_open = true;
  popup.sub_configured = false;
}

// ---------- Fullscreen transparent grab layer ----------
static void grab_layer_configure(void *data,
                                 struct zwlr_layer_surface_v1 *surface,
                                 uint32_t serial, uint32_t w, uint32_t h) {
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  uint32_t s = popup.scale > 0 ? (uint32_t)popup.scale : 1;
  uint32_t bw = w * s, bh = h * s;
  if (popup.grab_width == bw && popup.grab_height == bh && popup.grab_buffer)
    return;
  if (popup.grab_buffer) {
    wl_buffer_destroy(popup.grab_buffer);
    popup.grab_buffer = NULL;
  }
  popup.grab_width = bw;
  popup.grab_height = bh;
  popup.grab_buffer = create_transparent_buffer(
      bw, bh, &popup.grab_stride, &popup.grab_bufsize);
  if (popup.grab_buffer) {
    wl_surface_attach(popup.grab_surface, popup.grab_buffer, 0, 0);
    wl_surface_commit(popup.grab_surface);
  }
}

static void grab_layer_closed(void *data,
                              struct zwlr_layer_surface_v1 *surface) {
  if (popup.open)
    menu_close();
}

static const struct zwlr_layer_surface_v1_listener grab_layer_listener = {
    .configure = grab_layer_configure,
    .closed = grab_layer_closed,
};

static void grab_create(Bar *bar) {
  popup.grab_surface = wl_compositor_create_surface(compositor);
  wl_surface_set_buffer_scale(popup.grab_surface, bar->scale);
  popup.grab_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, popup.grab_surface, bar->wl_output,
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "mangobar-menu-grab");
  zwlr_layer_surface_v1_add_listener(popup.grab_layer_surface,
                                     &grab_layer_listener, NULL);
  zwlr_layer_surface_v1_set_anchor(
      popup.grab_layer_surface,
      ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
          ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
          ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
  zwlr_layer_surface_v1_set_exclusive_zone(popup.grab_layer_surface, 0);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      popup.grab_layer_surface,
      ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(popup.grab_surface);
}

static void grab_destroy(void) {
  if (popup.grab_layer_surface)
    zwlr_layer_surface_v1_destroy(popup.grab_layer_surface);
  if (popup.grab_surface)
    wl_surface_destroy(popup.grab_surface);
  if (popup.grab_buffer)
    wl_buffer_destroy(popup.grab_buffer);
  popup.grab_layer_surface = NULL;
  popup.grab_surface = NULL;
  popup.grab_buffer = NULL;
  popup.grab_width = popup.grab_height = 0;
  popup.grab_stride = popup.grab_bufsize = 0;
}

static void menu_open(Bar *bar, MangobarTrayItem *item, double lx, double ly) {
  menu_close();
  const char *svc = tray_item_service(item);
  const char *mp = tray_item_menu_path(item);
  if (!svc || !mp || !*mp)
    return;
  sd_bus *bus = (sd_bus *)tray_get_bus(tray);
  if (!bus)
    return;
  popup.menu = menu_init(bus, svc, mp, menu_layout_cb, NULL);
  if (!popup.menu)
    return;
  popup.output = bar->wl_output;
  popup.scale = bar->scale > 0 ? bar->scale : 1;
  // Create the grab layer first (below the menu) to catch outside clicks
  grab_create(bar);
  popup.surface = wl_compositor_create_surface(compositor);
  wl_surface_set_buffer_scale(popup.surface, popup.scale);
  popup.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      layer_shell, popup.surface, popup.output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      "mangobar-menu");
  zwlr_layer_surface_v1_add_listener(popup.layer_surface, &popup_layer_listener,
                                     NULL);
  zwlr_layer_surface_v1_set_anchor(
      popup.layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                               ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  // Record position / output size for layout clamping
  popup.lx = (int)lx;
  popup.ly = (int)ly;
  popup.bar_h = bar->height / (popup.scale > 0 ? popup.scale : 1);
  popup.out_w = bar->out_w;
  popup.out_h = bar->out_h;
  IPC_LOG("[menu] open bar_height=%u bar_h=%d lx=%d ly=%d out=%dx%d\n",
          bar->height, popup.bar_h, popup.lx, popup.ly, popup.out_w,
          popup.out_h);
  // Initial position: below the systray, no overlap
  int top = 10;
  int left = popup.lx > 10 ? popup.lx - 10 : 0;
  zwlr_layer_surface_v1_set_margin(popup.layer_surface, top, 0, 0, left);
  zwlr_layer_surface_v1_set_size(popup.layer_surface, 10, 10);
  zwlr_layer_surface_v1_set_exclusive_zone(popup.layer_surface, 0);
  wl_surface_commit(popup.surface);
  popup.open = true;
  popup.hover = -1;
  popup.configured = false;
  popup.item = item;
  menu_refresh(popup.menu);
}

// ---------- Pointer input ----------
static void setup_cursor() {
  if (cursor_surface || !compositor)
    return;
  cursor_surface = wl_compositor_create_surface(compositor);
  const char *theme_name = getenv("XCURSOR_THEME");
  int size = 24;
  const char *es = getenv("XCURSOR_SIZE");
  if (es && *es) {
    int v = atoi(es);
    if (v > 0)
      size = v;
  }
  cursor_theme = wl_cursor_theme_load(theme_name, size, shm);
}

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                             uint32_t serial, struct wl_surface *surface,
                             wl_fixed_t surface_x, wl_fixed_t surface_y) {
  pointer_x = wl_fixed_to_double(surface_x);
  pointer_y = wl_fixed_to_double(surface_y);
  if (popup.open) {
    if (surface == popup.surface) {
      popup_pointer = true;
      pointer_on_sub = false;
      popup.outside_since = 0;
      update_menu_hover(pointer_y);
    } else if (popup.sub_open && surface == popup.sub_surface) {
      popup_pointer = true;
      pointer_on_sub = true;
      popup.outside_since = 0;
      int row = popup.item_h > 0 ? (int)(pointer_y / popup.item_h) : -1;
      int vis = menu_node_visible_count(popup.sub_node);
      if (row >= vis)
        row = -1;
      if (row != popup.sub_hover) {
        popup.sub_hover = row;
        draw_submenu();
      }
    } else if (popup.grab_surface && surface == popup.grab_surface) {
      // Grab layer: keep the menu open; button handler closes it
      popup_pointer = false;
      pointer_on_sub = false;
      popup.outside_since = 0;
      // Grab coords match the bar (same output origin) for tray hit-testing
      Bar *gb;
      wl_list_for_each(gb, &bar_list, link)
          if (gb->wl_output == popup.output) {
            pointer_bar = gb;
            break;
          }
    } else {
      // Moving to the bar keeps it open; other windows count as outside
      bool is_bar = false;
      Bar *b;
      wl_list_for_each(b, &bar_list, link) if (b->wl_surface == surface) {
        is_bar = true;
        break;
      }
      if (is_bar) {
        popup_pointer = false;
        pointer_on_sub = false;
        popup.outside_since = 0;
      } else {
        menu_close();
      }
    }
  } else {
    popup_pointer = false;
    pointer_on_sub = false;
  }
  Bar *b;
  wl_list_for_each(b, &bar_list, link) if (b->wl_surface == surface) {
    pointer_bar = b;
    break;
  }
  if (cursor_theme) {
    struct wl_cursor *cur = wl_cursor_theme_get_cursor(cursor_theme, "default");
    if (cur) {
      struct wl_cursor_image *img = cur->images[0];
      wl_surface_attach(cursor_surface, wl_cursor_image_get_buffer(img), 0, 0);
      wl_pointer_set_cursor(pointer, serial, cursor_surface, img->hotspot_x,
                            img->hotspot_y);
      wl_surface_damage_buffer(cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
      wl_surface_commit(cursor_surface);
    }
  }
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                             uint32_t serial, struct wl_surface *surface) {
  if (popup.open) {
    // Pointer left our surfaces; close after the outside threshold.
    popup.outside_since = now_ms();
    popup_pointer = false;
    pointer_on_sub = false;
  }
  // Scrolling state only makes sense while the pointer is on the bar. Drop
  // any unconsumed distance so re-entering cannot fire stale scroll actions.
  memset(axis_smooth_remainder, 0, sizeof(axis_smooth_remainder));
  frame_has_axis = false;
  axis_stop_mask = 0;
  if (pointer_bar) {
    pointer_bar->hover_tag = -1;
    pointer_bar->redraw = true;
  }
  pointer_bar = NULL;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                              uint32_t time, wl_fixed_t surface_x,
                              wl_fixed_t surface_y) {
  pointer_x = wl_fixed_to_double(surface_x);
  pointer_y = wl_fixed_to_double(surface_y);
  if (pointer_bar) {
    int hover = -1;
    for (int i = 0; i < pointer_bar->hotspot_count; i++) {
      const Hotspot *h = &pointer_bar->hotspots[i];
      if (pointer_x >= h->x1 && pointer_x < h->x2 &&
          strcmp(h->module, "tags") == 0 && h->tag >= 0) {
        hover = h->tag;
        break;
      }
    }
    if (hover != pointer_bar->hover_tag) {
      pointer_bar->hover_tag = hover;
      pointer_bar->redraw = true;
    }
  }
  if (popup_pointer && pointer_on_sub) {
    int row = popup.item_h > 0 ? (int)(pointer_y / popup.item_h) : -1;
    int vis = menu_node_visible_count(popup.sub_node);
    if (row >= vis)
      row = -1;
    if (row != popup.sub_hover) {
      popup.sub_hover = row;
      draw_submenu();
    }
  } else if (popup_pointer) {
    update_menu_hover(pointer_y);
  }
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                              uint32_t serial, uint32_t time, uint32_t button,
                              uint32_t state) {
  if (state != WL_POINTER_BUTTON_STATE_PRESSED)
    return;
  IPC_LOG("[pointer] button=%u x=%.1f y=%.1f bar=%s popup=%d\n", button,
          pointer_x, pointer_y, pointer_bar ? pointer_bar->name : "-",
          popup.open);
  if (popup.open) {
    if (popup_pointer && pointer_on_sub && popup.sub_open) {
      const MangobarMenuNode *n =
          menu_node_visible_node(popup.sub_node, popup.sub_hover);
      if (n && n->enabled) {
        menu_activate(popup.menu, n, (uint32_t)now_ms());
        menu_close();
      }
    } else if (popup_pointer) {
      menu_activate_row(popup.hover);
    } else {
      menu_close();
      // Right-click a tray item: close old menu, open the new one
      if (pointer_bar && button == BTN_RIGHT)
        tray_right_click(pointer_bar, pointer_x, pointer_y);
    }
    return;
  }
  if (pointer_bar)
    dispatch_pointer_event(pointer_bar, pointer_x, pointer_y, button);
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                            uint32_t time, uint32_t axis, wl_fixed_t value) {
  if (axis < 2) {
    axis_value[axis] += value;
    frame_has_axis = true;
  }
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                     uint32_t axis, int32_t discrete) {
  // discrete sign is the scroll direction (positive = down/right)
  if (axis < 2)
    axis_steps[axis] += discrete;
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
  // Axis events can arrive after the pointer left the bar. They do not belong
  // to a hotspot, so discard this frame without contaminating a later one.
  if (!pointer_bar) {
    memset(axis_steps, 0, sizeof(axis_steps));
    memset(axis_value, 0, sizeof(axis_value));
    frame_has_axis = false;
    axis_stop_mask = 0;
    return;
  }

  // Only frames that actually carried axis events may touch the smooth
  // remainder; motion, button and enter frames must not drain it.
  bool has_axis = frame_has_axis;
  frame_has_axis = false;

  double threshold = smooth_scroll_threshold_at(pointer_bar, pointer_x);

  for (int a = 0; a < 2; a++) {
    int steps = axis_steps[a];
    int n = 0;
    int dir = 0; // -1=up/left, 1=down/right

    // Mouse wheels supply a discrete count alongside their continuous value.
    // Prefer that exact count and do not fold its companion value into the
    // smooth-scroll remainder.
    if (steps != 0) {
      dir = steps < 0 ? -1 : 1;
      n = abs(steps);
    } else if (has_axis) {
      axis_smooth_remainder[a] += wl_fixed_to_double(axis_value[a]);
      double remainder = axis_smooth_remainder[a];
      if (remainder != 0.0) {
        dir = remainder < 0.0 ? -1 : 1;
        n = (int)(fabs(remainder) / threshold);
      }
    }

    // Limit a single Wayland frame, but retain any additional distance for
    // subsequent frames instead of dropping it.
    if (n > 4)
      n = 4;
    if (has_axis && steps == 0 && n > 0)
      axis_smooth_remainder[a] -=
          dir * n * threshold;

    if (pointer_bar && n > 0 && dir != 0) {
      for (int i = 0; i < n; i++) {
        uint32_t btn = (a == 0) ? (dir < 0 ? 4 : 5) : (dir < 0 ? 6 : 7);
        dispatch_pointer_event(pointer_bar, pointer_x, pointer_y, btn);
      }
    }

    // axis_* values describe only this wl_pointer.frame. The smooth remainder
    // above is intentionally persistent and is consumed one threshold at a
    // time.
    axis_steps[a] = 0;
    axis_value[a] = 0;
  }

  // The compositor reports the end of a continuous gesture (e.g. fingers
  // lifted) with axis_stop, which arrives before this frame. Drop whatever
  // distance the last frame could not consume; it must not fire stale scrolls
  // after the gesture is over.
  for (int a = 0; a < 2; a++)
    if (axis_stop_mask & (1u << a))
      axis_smooth_remainder[a] = 0.0;
  axis_stop_mask = 0;
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                                   uint32_t axis_source) {
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t time, uint32_t axis) {
  if (axis < 2)
    axis_stop_mask |= 1u << axis;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
    .axis_source = wl_pointer_axis_source,
    .axis_stop = wl_pointer_axis_stop,
    .axis_discrete = wl_pointer_axis_discrete,
};

// ---------- System info (CPU / mem / backlight / volume / time) ----------
static int cpu_prev_total, cpu_prev_idle;
static uint64_t last_cpu_ms;

static void update_brightness() {
  Bar *bar;
  wl_list_for_each(bar, &bar_list, link) {
    if (!bar->profile || !bar->rt)
      continue;
    ProfileRuntime *rt = bar->rt;
    const MangoConfig *cfg = &bar->profile->config;
    if (!rt->brightness_dev_init) {
      rt->brightness_dev_init = true;
      if (cfg->brightness_dev[0]) {
        snprintf(rt->brightness_dev, sizeof(rt->brightness_dev), "%.63s",
                 cfg->brightness_dev);
      } else {
        DIR *d = opendir("/sys/class/backlight");
        if (d) {
          struct dirent *e;
          while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
              continue;
            char mb[512];
            snprintf(mb, sizeof(mb), "/sys/class/backlight/%s/max_brightness",
                     e->d_name);
            struct stat st;
            if (stat(mb, &st) == 0) {
              snprintf(rt->brightness_dev, sizeof(rt->brightness_dev),
                       "%.63s", e->d_name);
              break;
            }
          }
          closedir(d);
        }
      }
    }
    if (!rt->brightness_dev[0])
      continue;
    char p[256], mp[256];
    snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness",
             rt->brightness_dev);
    snprintf(mp, sizeof(mp), "/sys/class/backlight/%s/max_brightness",
             rt->brightness_dev);
    long b = 0, mb = 0;
    FILE *f = fopen(p, "r");
    if (f) {
      if (fscanf(f, "%ld", &b) != 1)
        b = 0;
      fclose(f);
    }
    FILE *f2 = fopen(mp, "r");
    if (f2) {
      if (fscanf(f2, "%ld", &mb) != 1)
        mb = 0;
      fclose(f2);
    }
    bar->brightness_pct = mb ? (int)(b * 100 / mb) : 0;
  }
}

// Create the udev backlight monitor (called once).
static void udev_init(void) {
  if (g_udev)
    return;
  g_udev = udev_new();
  if (!g_udev)
    return;
  g_udev_mon = udev_monitor_new_from_netlink(g_udev, "udev");
  if (!g_udev_mon) {
    udev_unref(g_udev);
    g_udev = NULL;
    return;
  }
  udev_monitor_filter_add_match_subsystem_devtype(g_udev_mon, "backlight",
                                                  NULL);
  udev_monitor_enable_receiving(g_udev_mon);
  g_udev_fd = udev_monitor_get_fd(g_udev_mon);
}

// Handle a backlight uevent; mark the module dirty for redraw.
static void udev_dispatch(void) {
  if (!g_udev_mon)
    return;
  struct udev_device *dev = udev_monitor_receive_device(g_udev_mon);
  if (!dev)
    return;
  const char *action = udev_device_get_action(dev);
  const char *sysname = udev_device_get_sysname(dev);
  if (action && strcmp(action, "change") == 0 && sysname) {
    for (size_t i = 0; i < g_config_set.count; i++) {
      const MangoConfig *cfg = &g_config_set.profiles[i].config;
      if (!cfg->brightness_dev[0] ||
          strcmp(sysname, cfg->brightness_dev) == 0) {
        brightness_dirty = true;
        break;
      }
    }
  }
  udev_device_unref(dev);
}

// Network: active interface name + optional speed (sampled in speed mode)
static void update_network(void) {
  static char ifname[64];
  static bool ifname_init;
  if (!ifname_init) {
    ifname_init = true;
    FILE *f = fopen("/proc/net/route", "r");
    if (f) {
      char line[256];
      if (fgets(line, sizeof(line), f) == NULL)
        line[0] = '\0'; // header; bail out of the loop below
      while (fgets(line, sizeof(line), f)) {
        char ifn[64], dst[16];
        if (sscanf(line, "%31s %15s", ifn, dst) == 2 &&
            strcmp(dst, "00000000") == 0) {
          snprintf(ifname, sizeof(ifname), "%s", ifn);
          break;
        }
      }
      fclose(f);
    }
    if (!ifname[0]) {
      DIR *d = opendir("/sys/class/net");
      if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
          if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0)
            continue;
          char p[512], st[16] = "";
          snprintf(p, sizeof(p), "/sys/class/net/%.240s/operstate",
                   e->d_name);
          FILE *sf = fopen(p, "r");
          if (sf) {
            if (fscanf(sf, "%15s", st) == 1 && strcmp(st, "up") == 0)
              snprintf(ifname, sizeof(ifname), "%.63s", e->d_name);
            fclose(sf);
          }
          if (ifname[0])
            break;
        }
        closedir(d);
      }
    }
  }

  Bar *b;
  bool need_speed = false;
  int nai = alt_index("network");
  wl_list_for_each(b, &bar_list, link) {
    if (nai >= 0 && b->alt_on[nai])
      need_speed = true;
    snprintf(b->net_ifname, sizeof(b->net_ifname), "%s", ifname);
  }
  if (!need_speed || !ifname[0])
    return;

  // Read rx/tx bytes from /proc/net/dev and compute delta speed
  static uint64_t prev_rx, prev_tx;
  static char prev_ifname[64];
  static double last_sec;
  uint64_t rx = 0, tx = 0;
  FILE *f = fopen("/proc/net/dev", "r");
  if (f) {
    char line[512];
    if (fgets(line, sizeof(line), f) == NULL ||
        fgets(line, sizeof(line), f) == NULL)
      line[0] = '\0'; // two header lines
    while (fgets(line, sizeof(line), f)) {
      char ifn[64];
      unsigned long long r, t;
      if (sscanf(line, "%31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                 ifn, &r, &t) == 3 && strcmp(ifn, ifname) == 0) {
        rx = (uint64_t)r;
        tx = (uint64_t)t;
        break;
      }
    }
    fclose(f);
  }

  double now = (double)now_ms() / 1000.0;
  if (prev_ifname[0] && strcmp(prev_ifname, ifname) == 0 && last_sec > 0) {
    double dt = now - last_sec;
    if (dt >= 0.5) {
      double rx_kbps = (double)(rx - prev_rx) / dt / 1024.0;
      double tx_kbps = (double)(tx - prev_tx) / dt / 1024.0;
      wl_list_for_each(b, &bar_list, link) {
        b->net_rx_kbps = rx_kbps;
        b->net_tx_kbps = tx_kbps;
      }
    }
  }
  prev_rx = rx;
  prev_tx = tx;
  snprintf(prev_ifname, sizeof(prev_ifname), "%s", ifname);
  last_sec = now;
}

// PulseAudio sink callback: cache percent and mute state
static void pulse_sink_cb(pa_context *c, const pa_sink_info *i, int eol,
                          void *userdata) {
  if (!i)
    return;
  pa_volume_t avg = pa_cvolume_avg(&i->volume);
  int pct = (int)((avg * 100) / PA_VOLUME_NORM);
  atomic_store(&pa_pct, pct < 0 ? 0 : (pct > 100 ? 100 : pct));
  atomic_store(&pa_muted, i->mute ? 1 : 0);
  int bt = 0;
  const char *api = i->proplist ? pa_proplist_gets(i->proplist, "device.api")
                                : NULL;
  if ((api && strstr(api, "bluez")) ||
      (i->name && strstr(i->name, "bluez")))
    bt = 1;
  atomic_store(&pa_sink_bt, bt);
  atomic_store(&pa_dirty, true);
  IPC_LOG("[pulse] sink pct=%d muted=%d bt=%d\n", atomic_load(&pa_pct),
          atomic_load(&pa_muted), bt);
  if (pulse_event_fd >= 0) {
    uint64_t one = 1;
    ssize_t wr = write(pulse_event_fd, &one, sizeof(one));
    (void)wr;
  }
}

// Source callback: track whether the active input is bluetooth
static void pulse_source_cb(pa_context *c, const pa_source_info *i, int eol,
                            void *userdata) {
  if (!i)
    return;
  int bt = 0;
  const char *api = i->proplist ? pa_proplist_gets(i->proplist, "device.api")
                                : NULL;
  if ((api && strstr(api, "bluez")) ||
      (i->name && strstr(i->name, "bluez")))
    bt = 1;
  atomic_store(&pa_source_bt, bt);
  IPC_LOG("[pulse] source bt=%d\n", bt);
}

static void pulse_query(void) {
  if (!pa_ctx || pa_context_get_state(pa_ctx) != PA_CONTEXT_READY)
    return;
  pa_operation *op =
      pa_context_get_sink_info_by_name(pa_ctx, NULL, pulse_sink_cb, NULL);
  if (op)
    pa_operation_unref(op);
  op = pa_context_get_source_info_by_name(pa_ctx, NULL, pulse_source_cb, NULL);
  if (op)
    pa_operation_unref(op);
}

static void pulse_state_cb(pa_context *c, void *userdata) {
  IPC_LOG("[pulse] state=%d\n", pa_context_get_state(c));
  if (pa_context_get_state(c) == PA_CONTEXT_READY) {
    pa_context_subscribe(c,
                         PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE,
                         NULL, NULL);
    pulse_query();
  }
}

static void pulse_subscribe_cb(pa_context *c,
                               pa_subscription_event_type_t t, uint32_t idx,
                               void *userdata) {
  IPC_LOG("[pulse] event type=%u\n", t);
  if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK ||
      (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE)
    pulse_query();
}

// Recreate the context so a restarted audio server is picked up.
static void pulse_reconnect(void) {
  if (!pa_ml)
    return;
  atomic_store(&pa_pct, -1);
  atomic_store(&pa_muted, -1);
  atomic_store(&pa_sink_bt, 0);
  atomic_store(&pa_source_bt, 0);
  if (pa_ctx) {
    pa_context_disconnect(pa_ctx);
    pa_context_unref(pa_ctx);
  }
  pa_ctx = pa_context_new(pa_mainloop_get_api(pa_ml), "mangobar");
  if (!pa_ctx)
    return;
  pa_context_set_state_callback(pa_ctx, pulse_state_cb, NULL);
  pa_context_set_subscribe_callback(pa_ctx, pulse_subscribe_cb, NULL);
  pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
}

static void *pulse_thread_fn(void *arg) {
  uint64_t last_try = 0;
  while (atomic_load(&pa_running) && pa_ml) {
    // 100 ms bound keeps the reconnect check live without busy polling.
    if (pa_mainloop_prepare(pa_ml, 100000) < 0)
      break;
    if (pa_mainloop_poll(pa_ml) < 0)
      break;
    if (pa_mainloop_dispatch(pa_ml) < 0)
      break;
    if (pa_ctx && (pa_context_get_state(pa_ctx) == PA_CONTEXT_FAILED ||
                   pa_context_get_state(pa_ctx) == PA_CONTEXT_TERMINATED)) {
      uint64_t now = now_ms();
      if (now - last_try >= 2000) {
        last_try = now;
        IPC_LOG("[pulse] reconnecting\n");
        pulse_reconnect();
      }
    }
  }
  return NULL;
}

// Create the persistent PulseAudio context (called once).
static void pulse_init(void) {
  if (pa_ml)
    return;
  pa_ml = pa_mainloop_new();
  if (!pa_ml)
    return;
  pulse_event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  pa_mainloop_api *api = pa_mainloop_get_api(pa_ml);
  pa_ctx = pa_context_new(api, "mangobar");
  if (!pa_ctx) {
    if (pulse_event_fd >= 0)
      close(pulse_event_fd);
    pulse_event_fd = -1;
    pa_mainloop_free(pa_ml);
    pa_ml = NULL;
    return;
  }
  pa_context_set_state_callback(pa_ctx, pulse_state_cb, NULL);
  pa_context_set_subscribe_callback(pa_ctx, pulse_subscribe_cb, NULL);
  pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
  atomic_store(&pa_running, true);
  pthread_create(&pulse_thread, NULL, pulse_thread_fn, NULL);
}

static void update_volume() {
  int pct = atomic_load(&pa_pct);
  int muted = atomic_load(&pa_muted);
  Bar *bar;
  wl_list_for_each(bar, &bar_list, link) {
    // PulseAudio not ready yet: keep 0% until the first sink event arrives.
    bar->volume_pct = pct >= 0 ? pct : 0;
    bar->volume_muted = muted == 1;
    bar->volume_bt = atomic_load(&pa_sink_bt) || atomic_load(&pa_source_bt);
  }
}

// Battery: scan /sys/class/power_supply and fill per-instance state so
// "battery" (first battery found) and "battery#BAT0"/"battery#BAT1" can be
// shown separately. Re-scanning every update also picks up batteries that are
// hot-plugged or removed.
static void update_battery(void) {
  char devs[MANGOBAR_MAX_BATTERIES][32];
  int caps[MANGOBAR_MAX_BATTERIES];
  char status[MANGOBAR_MAX_BATTERIES][16];
  bool have[MANGOBAR_MAX_BATTERIES] = {false};
  int n = 0;
  bool any_charging = false;

  DIR *d = opendir("/sys/class/power_supply");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) && n < MANGOBAR_MAX_BATTERIES) {
      char p[320], type[32] = {0};
      snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type", e->d_name);
      FILE *f = fopen(p, "r");
      if (!f || !fgets(type, sizeof(type), f) ||
          strncmp(type, "Battery", 7) != 0) {
        if (f)
          fclose(f);
        continue;
      }
      fclose(f);

      char st[32] = {0};
      snprintf(p, sizeof(p), "/sys/class/power_supply/%s/status", e->d_name);
      f = fopen(p, "r");
      if (f) {
        if (fgets(st, sizeof(st), f)) {
          st[strcspn(st, "\n")] = '\0';
          if (strncmp(st, "Charging", 8) == 0)
            any_charging = true;
        }
        fclose(f);
      }

      int cap = -1;
      snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity", e->d_name);
      f = fopen(p, "r");
      if (f) {
        if (fscanf(f, "%d", &cap) != 1)
          cap = -1;
        fclose(f);
      }

      // Keep devices sorted so the default "battery" is always the first one
      // (e.g. BAT0) regardless of readdir order.
      int ins = n;
      while (ins > 0 && strcmp(e->d_name, devs[ins - 1]) < 0) {
        snprintf(devs[ins], sizeof(devs[ins]), "%s", devs[ins - 1]);
        caps[ins] = caps[ins - 1];
        snprintf(status[ins], sizeof(status[ins]), "%s", status[ins - 1]);
        have[ins] = have[ins - 1];
        ins--;
      }
      snprintf(devs[ins], sizeof(devs[ins]), "%.31s", e->d_name);
      caps[ins] = cap;
      snprintf(status[ins], sizeof(status[ins]), "%s",
               st[0] ? st : "Unknown");
      have[ins] = cap >= 0;
      n++;
    }
    closedir(d);
  }

  bool on_ac = any_charging;
  d = opendir("/sys/class/power_supply");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      char p[320], type[32] = {0};
      snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type", e->d_name);
      FILE *f = fopen(p, "r");
      if (f && fgets(type, sizeof(type), f) &&
          strncmp(type, "Mains", 5) == 0) {
        fclose(f);
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/online", e->d_name);
        f = fopen(p, "r");
        if (f) {
          int on = 0;
          if (fscanf(f, "%d", &on) == 1 && on)
            on_ac = true;
          fclose(f);
        }
      } else if (f) {
        fclose(f);
      }
    }
    closedir(d);
  }

  Bar *b;
  wl_list_for_each(b, &bar_list, link) {
    if (!b->profile)
      continue;
    const MangoConfig *cfg = &b->profile->config;
    for (int i = 0; i < cfg->battery_count; i++) {
      const MangoBatteryCfg *bc = &cfg->batteries[i];
      int idx = -1;
      if (bc->device[0]) {
        for (int j = 0; j < n; j++)
          if (strcmp(devs[j], bc->device) == 0) {
            idx = j;
            break;
          }
      } else if (n > 0) {
        idx = 0; // default: first battery
      }
      if (idx >= 0 && have[idx]) {
        int pct = caps[idx];
        if (pct < 0)
          pct = 0;
        if (pct > 100)
          pct = 100;
        b->battery_pct[i] = pct;
        b->battery_present[i] = true;
        snprintf(b->battery_status[i], sizeof(b->battery_status[i]), "%s",
                 status[idx]);
      } else {
        b->battery_pct[i] = 0;
        b->battery_present[i] = false;
        snprintf(b->battery_status[i], sizeof(b->battery_status[i]), "%s",
                 "Unknown");
      }
      b->battery_on_ac[i] = on_ac;
    }
  }
}

static void update_system_info() {
  uint64_t ms = now_ms();
  // CPU percentage is a rate; skip sampling on very short windows so
  // scroll-triggered immediate refreshes don't produce noisy spikes.
  if (ms - last_cpu_ms >= 500) {
    last_cpu_ms = ms;
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
      char cpu[8];
      int user, nice, system, idle;
      if (fscanf(f, "%s %d %d %d %d", cpu, &user, &nice, &system, &idle) ==
          5) {
        int total = user + nice + system + idle;
        int pct = 0;
        if (cpu_prev_total) {
          int total_d = total - cpu_prev_total;
          int idle_d = idle - cpu_prev_idle;
          if (total_d)
            pct = 100 * (total_d - idle_d) / total_d;
        }
        cpu_prev_total = total;
        cpu_prev_idle = idle;
        Bar *b;
        wl_list_for_each(b, &bar_list, link) b->cpu_pct = pct;
      }
      fclose(f);
    }
    FILE *lf = fopen("/proc/loadavg", "r");
    if (lf) {
      double load1 = 0;
      if (fscanf(lf, "%lf", &load1) == 1) {
        // Round up to two decimals, matching loadavg's usual display.
        load1 = ceil(load1 * 100.0) / 100.0;
        Bar *b;
        wl_list_for_each(b, &bar_list, link) b->cpu_load = load1;
      }
      fclose(lf);
    }
  }
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    long total = 0, avail = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "MemTotal:", 9) == 0)
        sscanf(line + 9, "%ld", &total);
      else if (strncmp(line, "MemAvailable:", 13) == 0)
        sscanf(line + 13, "%ld", &avail);
    }
    fclose(f);
    int pct = total ? (int)(100 - (avail * 100 / total)) : 0;
    Bar *b;
    wl_list_for_each(b, &bar_list, link) b->mem_pct = pct;
  }
  update_brightness();
  update_volume();
  update_battery();
  update_network();
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  char ts[16];
  strftime(ts, sizeof(ts), "%H:%M", tm);
  Bar *b;
  wl_list_for_each(b, &bar_list, link) strcpy(b->time_str, ts);
}

// Run a custom module command and store its (trimmed) output.
static bool run_custom_module(MangoCustomModule *cm) {
  if (!cm->enabled || !cm->exec[0])
    return false;
  FILE *fp = popen(cm->exec, "r");
  if (!fp) {
    cm->last_run_ms = now_ms();
    return false;
  }
  size_t n = fread(cm->output, 1, sizeof(cm->output) - 1, fp);
  pclose(fp);
  while (n > 0 && (cm->output[n - 1] == '\n' || cm->output[n - 1] == '\r' ||
                   cm->output[n - 1] == ' ' || cm->output[n - 1] == '\t'))
    n--;
  cm->output[n] = '\0';
  cm->last_run_ms = now_ms();
  return true;
}

// Whether any active bar currently uses this profile.
static bool profile_has_active_bar(const MangoConfigProfile *profile) {
  Bar *b;
  wl_list_for_each(b, &bar_list, link)
    if (b->active && b->profile == profile)
      return true;
  return false;
}

// Force a profile's custom modules to run on the next refresh.
static void profile_custom_force_refresh(MangoConfigProfile *profile) {
  MangoConfig *cfg = &profile->config;
  for (int i = 0; i < cfg->custom_count; i++)
    cfg->customs[i].last_run_ms = 0;
}

// Refresh due custom modules; returns true when any output changed.
static bool update_custom_modules(void) {
  bool changed = false;
  uint64_t now = now_ms();
  for (size_t pi = 0; pi < g_config_set.count; pi++) {
    MangoConfigProfile *profile = &g_config_set.profiles[pi];
    if (!profile_has_active_bar(profile))
      continue;
    MangoConfig *cfg = &profile->config;
    for (int i = 0; i < cfg->custom_count; i++) {
      MangoCustomModule *cm = &cfg->customs[i];
      if (!cm->enabled || !cm->exec[0])
        continue;
      bool sig_dirty = custom_signal_dirty[pi][i];
      custom_signal_dirty[pi][i] = 0;
      uint64_t due = (uint64_t)(cm->interval > 0 ? cm->interval : 0) * 1000u;
      if (sig_dirty || cm->last_run_ms == 0 ||
          (due > 0 && now - cm->last_run_ms >= due)) {
        if (run_custom_module(cm))
          changed = true;
      }
    }
  }
  return changed;
}

static void tray_set_dirty() {
  Bar *b;
  wl_list_for_each(b, &bar_list, link) b->redraw = true;
}

static void custom_signal_handler(int signo) {
  for (int i = 0; i < custom_signal_count; i++) {
    if (custom_signal_map[i].signo == signo)
      custom_signal_dirty[custom_signal_map[i].pi][custom_signal_map[i].ci] = 1;
  }
  if (signal_fd >= 0) {
    uint64_t one = 1;
    ssize_t r = write(signal_fd, &one, sizeof(one));
    (void)r;
  }
}

// Install SIGRTMIN+N handlers for custom modules that declare a "signal".
static void register_custom_signals(void) {
  signal_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (signal_fd < 0)
    return;
  for (size_t pi = 0; pi < g_config_set.count; pi++) {
    MangoConfig *cfg = &g_config_set.profiles[pi].config;
    for (int i = 0; i < cfg->custom_count; i++) {
      MangoCustomModule *cm = &cfg->customs[i];
      if (!cm->enabled || cm->signal <= 0)
        continue;
      int signo = SIGRTMIN + cm->signal;
      if (signo > SIGRTMAX) {
        IPC_LOG("[signal] config[%zu] custom-%s: signal %d out of range (max %d)\n",
                pi, cm->name, cm->signal, SIGRTMAX - SIGRTMIN);
        continue;
      }
      if (custom_signal_count <
          MANGOBAR_MAX_CONFIGS * MANGOBAR_MAX_CUSTOM) {
        custom_signal_map[custom_signal_count].signo = signo;
        custom_signal_map[custom_signal_count].pi = (int)pi;
        custom_signal_map[custom_signal_count].ci = i;
        custom_signal_count++;
      }
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_handler = custom_signal_handler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_RESTART;
      sigaction(signo, &sa, NULL);
      IPC_LOG("[signal] config[%zu] custom-%s registered on SIGRTMIN+%d (%d)\n",
              pi, cm->name, cm->signal, signo);
    }
  }
}

static void event_loop() {
  int wl_fd = wl_display_get_fd(display);
  while (running) {
    int tray_fd = tray ? tray_get_fd(tray) : -1;
    struct pollfd fds[6];
    int nfds = 0, ipc_idx = -1, tray_idx = -1, pa_idx = -1, udev_idx = -1;
    int signal_idx = -1;
    fds[nfds++] =
        (struct pollfd){.fd = wl_fd, .events = POLLIN | POLLERR | POLLHUP};
    if (signal_fd >= 0) {
      signal_idx = nfds;
      fds[nfds++] = (struct pollfd){.fd = signal_fd, .events = POLLIN};
    }
    if (ipc_fd >= 0) {
      ipc_idx = nfds;
      fds[nfds++] = (struct pollfd){.fd = ipc_fd, .events = POLLIN};
    }
    if (tray_fd >= 0) {
      tray_idx = nfds;
      short tev = tray_get_events(tray);
      fds[nfds++] = (struct pollfd){.fd = tray_fd,
                                    .events = (short)(POLLIN | tev)};
    }
    if (pulse_event_fd >= 0) {
      pa_idx = nfds;
      fds[nfds++] = (struct pollfd){.fd = pulse_event_fd, .events = POLLIN};
    }
    if (g_udev_fd >= 0) {
      udev_idx = nfds;
      fds[nfds++] = (struct pollfd){.fd = g_udev_fd, .events = POLLIN};
    }
    // Short timeout while refreshing or the menu is open.
    int timeout = 1000;
    if (sys_refresh)
      timeout = 100;
    else if (popup.open)
      timeout = 50;

    // Canonical libwayland read pattern: prepare, poll, read, dispatch.
    // Exit only when the compositor socket is gone (POLLHUP/POLLERR or a
    // failed read), not when a layer surface is closed (e.g. TTY switch).
    int prep = wl_display_prepare_read(display);
    if (prep != 0) {
      if (wl_display_dispatch_pending(display) < 0) {
        IPC_LOG("[wl] dispatch failed, exiting\n");
        break;
      }
    } else {
      if (wl_display_flush(display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        IPC_LOG("[wl] flush failed errno=%d, exiting\n", errno);
        break;
      }
      int ret = poll(fds, (nfds_t)nfds, timeout);
      if (ret < 0) {
        if (errno == EINTR) {
          wl_display_cancel_read(display);
          continue;
        }
        break;
      }
      if (fds[0].revents & (POLLERR | POLLHUP)) {
        wl_display_cancel_read(display);
        IPC_LOG("[wl] compositor exited, exiting\n");
        break;
      }
      if (fds[0].revents & POLLIN) {
        if (wl_display_read_events(display) < 0) {
          IPC_LOG("[wl] read failed, exiting\n");
          break;
        }
        if (wl_display_dispatch_pending(display) < 0) {
          IPC_LOG("[wl] dispatch failed, exiting\n");
          break;
        }
      } else {
        wl_display_cancel_read(display);
      }
    }
    if (signal_idx >= 0 && (fds[signal_idx].revents & POLLIN)) {
      uint64_t v;
      while (read(signal_fd, &v, sizeof(v)) > 0)
        ;
    }
    if (ipc_idx >= 0 && (fds[ipc_idx].revents & POLLIN)) {
      ssize_t n = read(ipc_fd, ipc_buf + ipc_buf_len,
                       sizeof(ipc_buf) - ipc_buf_len - 1);
      if (n > 0) {
        ipc_buf_len += n;
        ipc_buf[ipc_buf_len] = '\0';
        IPC_LOG("[ipc] read n=%zd buflen=%zu\n", n, ipc_buf_len);
        process_ipc_data();
      } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        IPC_LOG("[ipc] read EOF/ERR n=%zd errno=%d, closing fd\n", n, errno);
        close(ipc_fd);
        ipc_fd = -1;
        ipc_buf_len = 0;
      }
    }
    if (tray_idx >= 0 && (fds[tray_idx].revents & (POLLIN | POLLOUT))) {
      tray_dispatch(tray);
    }
    if (pa_idx >= 0 && (fds[pa_idx].revents & POLLIN)) {
      uint64_t v;
      while (read(pulse_event_fd, &v, sizeof(v)) > 0)
        ;
    }
    if (udev_idx >= 0 && (fds[udev_idx].revents & POLLIN))
      udev_dispatch();
    if (sys_refresh) {
      sys_refresh = false;
      update_system_info();
      Bar *b;
      wl_list_for_each(b, &bar_list, link) b->redraw = true;
    }
    if (atomic_exchange(&pa_dirty, false)) {
      update_volume();
      Bar *b;
      wl_list_for_each(b, &bar_list, link) b->redraw = true;
    }
    if (brightness_dirty) {
      brightness_dirty = false;
      update_brightness();
      Bar *b;
      wl_list_for_each(b, &bar_list, link) b->redraw = true;
    }
    if (update_custom_modules()) {
      Bar *b;
      wl_list_for_each(b, &bar_list, link) b->redraw = true;
    }
    static time_t last_sec;
    static time_t last_tray_refresh;
    time_t sec = time(NULL);
    // Auto-reconnect the watch connection if it drops
    static time_t last_reconnect;
    if (ipc_fd < 0 && sec - last_reconnect >= 2) {
      last_reconnect = sec;
      IPC_LOG("[ipc] reconnecting...\n");
      ipc_connect();
      if (ipc_fd >= 0)
        ipc_subscribe();
    }
    if (sec - last_sec >= g_config_set.profiles[0].config.sys_interval) {
      last_sec = sec;
      update_system_info();
      Bar *b;
      wl_list_for_each(b, &bar_list, link) b->redraw = true;
    }
    // Periodically refresh the tray to rediscover items
    if (tray && sec - last_tray_refresh >= 10) {
      last_tray_refresh = sec;
      tray_refresh(tray);
    }
    // Auto-open submenus on hover
    menu_hover_tick();
    // Retry opening the tray menu when its property wasn't ready
    if (popup.pending_item && now_ms() - popup.pending_ms >= 300) {
      MangobarTrayItem *pi = popup.pending_item;
      Bar *pb = popup.pending_bar;
      int px = popup.pending_x, py = popup.pending_y;
      popup.pending_item = NULL;
      popup.pending_bar = NULL;
      if (pi && pb && tray_item_has_menu(pi))
        menu_open(pb, pi, px, py);
    }
    static time_t last_tick;
    if (sec - last_tick >= 5) {
      last_tick = sec;
      Bar *bt;
      wl_list_for_each(bt, &bar_list, link)
          IPC_LOG("[tick] %s ipc_fd=%d redraw=%d\n", bt->name, ipc_fd,
                  bt->redraw);
    }
    Bar *b;
    wl_list_for_each(b, &bar_list, link) {
      if (b->redraw && b->configured) {
        draw_bar(b);
        b->redraw = false;
      }
    }
  }
}

// ---------- Style initialization ----------
static void apply_css_resolved(ModuleStyle *ms, Style s) {
  if (s.color_set)
    hex_to_pixman(s.color, &ms->fg);
  if (s.background_set) {
    hex_to_pixman(s.background, &ms->bg);
    ms->bg_gradient = s.bg_gradient;
    ms->gradient_dir = s.gradient_dir;
    hex_to_pixman(s.gradient_end, &ms->gradient_end);
  }
  if (s.padding_left_set)
    ms->pad_l = s.padding_left;
  if (s.padding_right_set)
    ms->pad_r = s.padding_right;
  if (s.margin_left_set)
    ms->margin_l = s.margin_left;
  if (s.margin_right_set)
    ms->margin_r = s.margin_right;
  if (s.radius_set)
    ms->radius = s.radius;
  if (s.min_width_set)
    ms->min_width = s.min_width;
}

static void apply_css(ModuleStyle *ms, const char *module, const char *state) {
  apply_css_resolved(ms, style_resolve(&g_style_sheet, module, state));
}

static void init_profile_styles(ProfileRuntime *rt, MangoConfigProfile *profile) {
  rt->profile = profile;
  ModuleStyle *all[] = {&rt->st_tags[0], &rt->st_tags[1], &rt->st_tags[2],
                        &rt->st_tags[3], &rt->st_tags[4], &rt->st_layout,
                        &rt->st_title,   &rt->st_clock,   &rt->st_clock_date, &rt->st_cpu,
                        &rt->st_mem,     &rt->st_brightness, &rt->st_volume,
                        &rt->st_keymode, &rt->st_keyboardlayout, &rt->st_network,
                        &rt->st_hide_clients, &rt->st_battery, &rt->st_overview,
                        &rt->st_separator, &rt->st_tray, &rt->st_bar, &rt->st_bar_sel};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
    all[i]->radius = profile->config.radius_default;
  for (int i = 0; i < MANGOBAR_MAX_CUSTOM; i++) {
    rt->st_custom[i].radius = profile->config.radius_default;
  }
  // Tags/layout default to pill shape; CSS border-radius overrides this.
  for (int i = 0; i < 5; i++)
    rt->st_tags[i].radius = -1;
  rt->st_layout.radius = -1;
  for (int i = 0; i < 5; i++)
    rt->st_tags[i].center = true;
  rt->st_layout.center = true;
  rt->st_overview.center = true;

  hex_to_pixman(active_fg_color_hex, &rt->st_tags[0].fg);
  hex_to_pixman(active_bg_color_hex, &rt->st_tags[0].bg);
  hex_to_pixman(occupied_fg_color_hex, &rt->st_tags[1].fg);
  hex_to_pixman(occupied_bg_color_hex, &rt->st_tags[1].bg);
  hex_to_pixman(urgent_fg_color_hex, &rt->st_tags[2].fg);
  hex_to_pixman(urgent_bg_color_hex, &rt->st_tags[2].bg);
  hex_to_pixman(empty_fg_color_hex, &rt->st_tags[3].fg);
  hex_to_pixman(empty_bg_color_hex, &rt->st_tags[3].bg);
  hex_to_pixman(inactive_fg_color_hex, &rt->st_tags[4].fg);
  hex_to_pixman(inactive_bg_color_hex, &rt->st_tags[4].bg);

  hex_to_pixman(layout_fg_color_hex, &rt->st_layout.fg);
  hex_to_pixman(layout_bg_color_hex, &rt->st_layout.bg);
  hex_to_pixman(title_fg_color_hex, &rt->st_title.fg);
  hex_to_pixman(title_bg_color_hex, &rt->st_title.bg);

  hex_to_pixman(cpu_fg_color_hex, &rt->st_cpu.fg);
  hex_to_pixman(cpu_bg_color_hex, &rt->st_cpu.bg);
  hex_to_pixman(mem_fg_color_hex, &rt->st_mem.fg);
  hex_to_pixman(mem_bg_color_hex, &rt->st_mem.bg);
  hex_to_pixman(brightness_fg_color_hex, &rt->st_brightness.fg);
  hex_to_pixman(brightness_bg_color_hex, &rt->st_brightness.bg);
  hex_to_pixman(volume_fg_color_hex, &rt->st_volume.fg);
  hex_to_pixman(volume_bg_color_hex, &rt->st_volume.bg);
  hex_to_pixman(clock_fg_color_hex, &rt->st_clock.fg);
  hex_to_pixman(clock_bg_color_hex, &rt->st_clock.bg);
  for (int i = 0; i < MANGOBAR_MAX_CUSTOM; i++) {
    rt->st_custom[i].fg = rt->st_clock.fg;
    rt->st_custom[i].bg = rt->st_clock.bg;
  }
  hex_to_pixman(clock_fg_color_hex, &rt->st_clock_date.fg);
  hex_to_pixman(clock_bg_color_hex, &rt->st_clock_date.bg);
  hex_to_pixman(keymode_fg_color_hex, &rt->st_keymode.fg);
  hex_to_pixman(keymode_bg_color_hex, &rt->st_keymode.bg);
  hex_to_pixman(keyboardlayout_fg_color_hex, &rt->st_keyboardlayout.fg);
  hex_to_pixman(keyboardlayout_bg_color_hex, &rt->st_keyboardlayout.bg);
  hex_to_pixman(clock_fg_color_hex, &rt->st_network.fg);
  hex_to_pixman(clock_bg_color_hex, &rt->st_network.bg);
  hex_to_pixman(hide_clients_fg_color_hex, &rt->st_hide_clients.fg);
  hex_to_pixman(hide_clients_bg_color_hex, &rt->st_hide_clients.bg);
  hex_to_pixman(battery_fg_color_hex, &rt->st_battery.fg);
  hex_to_pixman(battery_bg_color_hex, &rt->st_battery.bg);

  hex_to_pixman(tray_fg_color_hex, &rt->st_tray.fg);
  hex_to_pixman(tray_bg_color_hex, &rt->st_tray.bg);

  hex_to_pixman(overview_fg_color_hex, &rt->st_overview.fg);
  hex_to_pixman(overview_bg_color_hex, &rt->st_overview.bg);

  hex_to_pixman(separator_fg_color_hex, &rt->st_separator.fg);
  hex_to_pixman(separator_bg_color_hex, &rt->st_separator.bg);

  hex_to_pixman(middle_bg_color_hex, &rt->st_bar.bg);
  hex_to_pixman(middle_bg_color_hex, &rt->st_bar.fg);
  hex_to_pixman(middle_bg_sel_color_hex, &rt->st_bar_sel.bg);
  hex_to_pixman(middle_bg_sel_color_hex, &rt->st_bar_sel.fg);

  if (!g_style_sheet.loaded)
    return;

  apply_css(&rt->st_tags[0], "tags", "active");
  apply_css(&rt->st_tags[1], "tags", "occupied");
  apply_css(&rt->st_tags[2], "tags", "urgent");
  apply_css(&rt->st_tags[3], "tags", "empty");
  apply_css(&rt->st_tags[4], "tags", "inactive");
  // Waybar-style aliases (#workspaces, #workspaces button:hover): overlay only
  // the #workspaces rules so the "*" defaults don't clobber the #tags styles.
  apply_css_resolved(&rt->st_tags[0],
                     style_resolve_module_only(&g_style_sheet, "workspaces",
                                               "active"));
  apply_css_resolved(&rt->st_tags[1],
                     style_resolve_module_only(&g_style_sheet, "workspaces",
                                               "occupied"));
  apply_css_resolved(&rt->st_tags[2],
                     style_resolve_module_only(&g_style_sheet, "workspaces",
                                               "urgent"));
  apply_css_resolved(&rt->st_tags[3],
                     style_resolve_module_only(&g_style_sheet, "workspaces",
                                               "empty"));
  apply_css_resolved(&rt->st_tags[4],
                     style_resolve_module_only(&g_style_sheet, "workspaces",
                                               "inactive"));
  Style hov_tags = style_resolve(&g_style_sheet, "tags", "hover");
  Style hov_ws =
      style_resolve_module_only(&g_style_sheet, "workspaces", "hover");
  rt->st_tag_hover_css = style_overlay(&hov_tags, &hov_ws);
  apply_css(&rt->st_layout, "layout", NULL);
  apply_css(&rt->st_title, "title", NULL);
  apply_css(&rt->st_clock, "clock", NULL);
  apply_css(&rt->st_clock_date, "clock", NULL);
  apply_css(&rt->st_clock_date, "clock", "date");
  apply_css(&rt->st_cpu, "cpu", NULL);
  apply_css(&rt->st_mem, "mem", NULL);
  apply_css(&rt->st_brightness, "brightness", NULL);
  apply_css(&rt->st_volume, "volume", NULL);
  apply_css(&rt->st_keymode, "keymode", NULL);
  apply_css(&rt->st_keyboardlayout, "keyboardlayout", NULL);
  apply_css(&rt->st_network, "network", NULL);
  apply_css(&rt->st_hide_clients, "hideclients", NULL);
  apply_css(&rt->st_battery, "battery", NULL);
  apply_css(&rt->st_overview, "overview", NULL);
  apply_css(&rt->st_separator, "separator", NULL);
  apply_css(&rt->st_tray, "tray", NULL);
  apply_css(&rt->st_bar, "bar", NULL);
  apply_css(&rt->st_bar_sel, "bar", "sel");
  for (int i = 0; i < profile->config.custom_count; i++) {
    if (!profile->config.customs[i].enabled)
      continue;
    char sel[96];
    snprintf(sel, sizeof(sel), "custom-%s", profile->config.customs[i].name);
    apply_css(&rt->st_custom[i], sel, NULL);
  }

  // Top margin comes from the #bar CSS margin.
  Style bar_s = style_resolve(&g_style_sheet, "bar", "");
  rt->bar_top = bar_s.margin_top_set ? bar_s.margin_top : 0;
  rt->bar_left = bar_s.margin_left_set ? (uint32_t)bar_s.margin_left : 0;
  rt->bar_right = bar_s.margin_right_set ? (uint32_t)bar_s.margin_right : 0;
  rt->bar_h = (uint32_t)profile->config.bar_height;
}

int main(int argc, char **argv) {
  // Honor the user's locale for strftime (e.g. Chinese month/day names).
  setlocale(LC_TIME, "");

  // Command-line overrides.
  const char *opt_config = NULL;
  const char *opt_css = NULL;
  int opt;
  while ((opt = getopt(argc, argv, "hvc:s:")) != -1) {
    switch (opt) {
      case 'h':
        print_usage(argv[0]);
        return 0;
      case 'v':
        printf("mangobar %s\n", MANGOBAR_VERSION);
        return 0;
      case 'c':
        opt_config = optarg;
        break;
      case 's':
        opt_css = optarg;
        break;
      default:
        print_usage(argv[0]);
        return 1;
    }
  }

  // Load external JSONC config (a single object or a root array of
  // profiles).
  char err[512];
  char cfg_buf[512];
  const char *cfg_file;
  if (opt_config) {
    if (access(opt_config, R_OK) != 0) {
      fprintf(stderr, "mangobar: cannot read config file: %s\n", opt_config);
      return 1;
    }
    cfg_file = opt_config;
  } else {
    cfg_file = mango_config_find_default(cfg_buf, sizeof(cfg_buf));
  }
  if (cfg_file) {
    if (mango_config_set_load(&g_config_set, cfg_file, err, sizeof(err)) != 0) {
      fprintf(stderr, "mangobar: config error: %s\n", err);
      return 1;
    }
    fprintf(stderr, "Loaded config: %s\n", cfg_file);
  } else {
    if (mango_config_set_parse(&g_config_set, default_config_jsonc, err,
                               sizeof(err)) != 0) {
      fprintf(stderr, "mangobar: config error: %s\n", err);
      return 1;
    }
    fprintf(stderr, "Using built-in default config\n");
  }

  // Defensive check for an empty configuration set.
  if (g_config_set.count == 0) {
    fprintf(stderr, "mangobar: empty configuration, exiting\n");
    return 0;
  }

  // Global baseline config for shared system sampling (CPU/mem/volume/clock).
  g_cfg_ptr = &g_config_set.profiles[0].config;

  // One profile runtime per config profile.
  g_runtimes = calloc(g_config_set.count, sizeof(ProfileRuntime));
  if (!g_runtimes) {
    fprintf(stderr, "mangobar: out of memory allocating profiles\n");
    return 1;
  }

  // Load the shared CSS style sheet. A single root object keeps its inline
  // css/style path; profile arrays use shared/default CSS.
  style_sheet_init(&g_style_sheet);
  char css_buf[512];
  const char *css_file = NULL;
  MangoConfig *base = &g_config_set.profiles[0].config;
  if (opt_css) {
    if (access(opt_css, R_OK) != 0) {
      fprintf(stderr, "mangobar: cannot read css file: %s\n", opt_css);
      return 1;
    }
    css_file = opt_css;
  } else if (base->css_path[0]) {
    css_file = base->css_path;
  } else if (style_find_default_path(css_buf, sizeof(css_buf)) == 0) {
    css_file = css_buf;
  }
  if (css_file) {
    style_sheet_load(&g_style_sheet, css_file);
  } else {
    style_sheet_parse(&g_style_sheet, default_style_css);
    fprintf(stderr, "Using built-in default CSS\n");
  }

  // The font is shared across outputs; fall back to the first profile's font.
  fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
  snprintf(g_font_base, sizeof(g_font_base), "%s",
           style_font_string(&g_style_sheet, base->font));
  font = fcft_from_name(1, (const char *[]){g_font_base}, NULL);
  if (!font) {
    // Fall back to the config font if the CSS font fails
    snprintf(g_font_base, sizeof(g_font_base), "%s", base->font);
    font = fcft_from_name(1, (const char *[]){g_font_base}, NULL);
  }
  if (!font) {
    fprintf(stderr, "Failed to load fonts\n");
    return 1;
  }

  // Resolve styles and geometry for every profile.
  int min_h = font->ascent + font->descent + 4;
  for (size_t i = 0; i < g_config_set.count; i++) {
    ProfileRuntime *rt = &g_runtimes[i];
    init_profile_styles(rt, &g_config_set.profiles[i]);
    // Ensure the bar is tall enough for the text (no vertical clipping).
    if ((int)rt->bar_h < min_h)
      rt->bar_h = (uint32_t)min_h;
    // Keep tag/layout buttons at least as wide as the bar is tall.
    for (int j = 0; j < 5; j++)
      rt->st_tags[j].min_width =
          (int)rt->bar_h + rt->st_tags[j].margin_l + rt->st_tags[j].margin_r;
    rt->st_layout.min_width =
        (int)rt->bar_h + rt->st_layout.margin_l + rt->st_layout.margin_r;
  }

  display = wl_display_connect(NULL);
  if (!display)
    return 1;
  wl_list_init(&bar_list);

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!compositor || !shm || !layer_shell || !output_manager)
    return 1;

  setup_cursor();

  tray = tray_init(tray_set_dirty);

  ipc_connect();
  if (ipc_fd >= 0) {
    wl_display_roundtrip(display);
    ipc_subscribe();
  }
  update_system_info();
  update_custom_modules();
  pulse_init();
  udev_init();
  register_custom_signals();
  signal(SIGTERM, exit);
  signal(SIGINT, exit);
  signal(SIGCHLD, SIG_IGN); // reap action command children

  event_loop();
  if (popup.open)
    menu_close();
  if (ipc_fd >= 0)
    close(ipc_fd);
  if (tray)
    tray_destroy(tray);
  if (pa_ml) {
    atomic_store(&pa_running, false);
    pa_mainloop_wakeup(pa_ml);
    pthread_join(pulse_thread, NULL);
  }
  if (pa_ctx)
    pa_context_disconnect(pa_ctx);
  if (pa_ctx)
    pa_context_unref(pa_ctx);
  if (pa_ml)
    pa_mainloop_free(pa_ml);
  if (pulse_event_fd >= 0)
    close(pulse_event_fd);
  if (signal_fd >= 0)
    close(signal_fd);
  if (g_udev_mon)
    udev_monitor_unref(g_udev_mon);
  if (g_udev)
    udev_unref(g_udev);
  fcft_destroy(font);
  fcft_fini();
  wl_display_disconnect(display);
  mango_config_set_destroy(&g_config_set);
  free(g_runtimes);
  return 0;
}

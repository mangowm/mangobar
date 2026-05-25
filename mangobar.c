#define _GNU_SOURCE
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <pixman.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-util.h>

#include "config.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

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

/* ---------- 颜色工具 ---------- */
static void hex_to_pixman(uint32_t hex, pixman_color_t *c) {
  c->red = ((hex >> 24) & 0xff) * 0x101;
  c->green = ((hex >> 16) & 0xff) * 0x101;
  c->blue = ((hex >> 8) & 0xff) * 0x101;
  c->alpha = (hex & 0xff) * 0x101;
}

/* ---------- Bar ---------- */
typedef struct {
  struct wl_output *wl_output;
  struct wl_surface *wl_surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct zxdg_output_v1 *xdg_output;
  uint32_t registry_name;
  char *name;
  bool configured;
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
  char time_str[16];
  bool redraw;
  bool overview_mode; // 当 active_tags == [0] 时为 true
  struct wl_list link;
} Bar;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *output_manager;
static struct wl_list bar_list;
static struct fcft_font *font;
static bool running = true;

static int ipc_fd = -1;
static char ipc_buf[65536];
static size_t ipc_buf_len = 0;

/* 颜色变量 */
static pixman_color_t active_fg, active_bg;
static pixman_color_t occupied_fg, occupied_bg;
static pixman_color_t inactive_fg, inactive_bg;
static pixman_color_t urgent_fg, urgent_bg;
static pixman_color_t empty_fg, empty_bg;
static pixman_color_t middle_bg, middle_bg_sel;

static pixman_color_t layout_fg, layout_bg;
static pixman_color_t title_fg, title_bg;
static pixman_color_t cpu_fg, cpu_bg;
static pixman_color_t mem_fg, mem_bg;
static pixman_color_t clock_fg, clock_bg;
static pixman_color_t keymode_fg, keymode_bg;
static pixman_color_t keyboardlayout_fg, keyboardlayout_bg;
static pixman_color_t overview_fg, overview_bg;
static pixman_color_t separator_fg, separator_bg;

/* ========== Wayland 缓冲 ========== */
static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
  (void)data;
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

/* ========== 文本绘制 ========== */
static uint32_t draw_text(const char *text, uint32_t x, uint32_t y,
                          pixman_image_t *fg, pixman_image_t *fg_mask,
                          pixman_image_t *bg, pixman_color_t *fg_color,
                          pixman_color_t *bg_color, uint32_t max_x,
                          uint32_t buf_h) {
  if (!text || !*text || x >= max_x)
    return x;
  uint32_t cur_x = x;
  uint32_t state = 0, codepoint = 0, last_cp = 0;
  for (const char *p = text; *p; p++) {
    if (utf8_decode(&state, &codepoint, (uint8_t)*p))
      continue;
    const struct fcft_glyph *g =
        fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
    if (!g)
      continue;
    long kern = 0;
    if (last_cp)
      fcft_kerning(font, last_cp, codepoint, &kern, NULL);
    uint32_t advance = g->advance.x + kern;
    if (cur_x + advance + 4 > max_x)
      break;
    last_cp = codepoint;

    if (fg && fg_color) {
      if (pixman_image_get_format(g->pix) == PIXMAN_a8r8g8b8) {
        pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, fg, 0, 0, 0, 0,
                                 cur_x + g->x, y - g->y, g->width, g->height);
      } else {
        pixman_image_fill_boxes(
            PIXMAN_OP_OVER, fg, fg_color, 1,
            &(pixman_box32_t){
                .x1 = cur_x, .x2 = cur_x + advance, .y1 = 0, .y2 = buf_h});
      }
      pixman_image_t *mask = pixman_image_create_solid_fill(
          &(pixman_color_t){0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF});
      pixman_image_composite32(PIXMAN_OP_OVER, g->pix, mask, fg_mask, 0, 0, 0,
                               0, cur_x + g->x, y - g->y, g->width, g->height);
      pixman_image_unref(mask);
    }
    if (bg && bg_color)
      pixman_image_fill_boxes(
          PIXMAN_OP_OVER, bg, bg_color, 1,
          &(pixman_box32_t){
              .x1 = cur_x, .x2 = cur_x + advance, .y1 = 0, .y2 = buf_h});
    cur_x += advance;
  }
  return cur_x;
}

static uint32_t text_width(const char *text) {
  uint32_t w = 0, state = 0, codepoint = 0;
  for (const char *p = text; *p; p++) {
    if (utf8_decode(&state, &codepoint, (uint8_t)*p))
      continue;
    const struct fcft_glyph *g =
        fcft_rasterize_char_utf32(font, codepoint, FCFT_SUBPIXEL_NONE);
    if (g)
      w += g->advance.x;
  }
  return w;
}

static void draw_bar(Bar *bar) {
  int fd = allocate_shm_file(bar->bufsize);
  if (fd < 0)
    return;
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

  uint32_t x = 0;
  uint32_t y = (bar->height + font->ascent - font->descent) / 2;

  /* --- 1. 左侧模块：标签 --- */
  if (show_tags) {
    if (bar->overview_mode) {
      /* 当 active_tags == [0] 时，只显示 OVERVIEW */
      x = draw_text("OVERVIEW", x, y, fg, fg_mask, bg, &overview_fg,
                    &overview_bg, bar->width, bar->height);
    } else {
      for (int i = 0; i < TAG_COUNT; i++) {
        /* 过滤条件：如果配置为只显示占用标签，则跳过既无客户端又不在
         * active_tags 中的标签 */
#if show_only_occupied_tags
        if (!(bar->ctags & (1 << i)) && !(bar->atags & (1 << i)))
          continue;
#endif
        bool active = bar->mtags & (1 << i);
        bool urgent = bar->urg & (1 << i);
        bool occupied = bar->ctags & (1 << i);
        pixman_color_t *f, *b;
        if (urgent) {
          f = &urgent_fg;
          b = &urgent_bg;
        } else if (active) {
          f = &active_fg;
          b = &active_bg;
        } else if (occupied) {
          f = &occupied_fg;
          b = &occupied_bg;
        } else {
          f = &empty_fg;
          b = &empty_bg;
        }
        x = draw_text(tag_names[i], x, y, fg, fg_mask, bg, f, b, bar->width,
                      bar->height);
      }
    }
  }

  /* 布局模块 */
  if (show_layout) {
    x = draw_text(bar->layout, x, y, fg, fg_mask, bg, &layout_fg, &layout_bg,
                  bar->width, bar->height);
  }

  /* --- 右侧模块列表构建 --- */
  struct {
    const char *text;
    pixman_color_t *fg;
    pixman_color_t *bg;
    bool enabled;
  } modules[5];
  int mod_count = 0;

  char mod_text[5][64];
  int idx = 0;

  if (show_keymode) {
    snprintf(mod_text[idx], sizeof(mod_text[idx]), "%s", bar->keymode);
    modules[idx++] =
        (typeof(modules[0])){mod_text[idx - 1], &keymode_fg, &keymode_bg, true};
  }
  if (show_keyboardlayout) {
    snprintf(mod_text[idx], sizeof(mod_text[idx]), "%s", bar->kb_layout);
    modules[idx++] = (typeof(modules[0])){mod_text[idx - 1], &keyboardlayout_fg,
                                          &keyboardlayout_bg, true};
  }
  if (show_cpu) {
    snprintf(mod_text[idx], sizeof(mod_text[idx]), "CPU:%d%%", bar->cpu_pct);
    modules[idx++] =
        (typeof(modules[0])){mod_text[idx - 1], &cpu_fg, &cpu_bg, true};
  }
  if (show_mem) {
    snprintf(mod_text[idx], sizeof(mod_text[idx]), "MEM:%d%%", bar->mem_pct);
    modules[idx++] =
        (typeof(modules[0])){mod_text[idx - 1], &mem_fg, &mem_bg, true};
  }
  if (show_clock) {
    snprintf(mod_text[idx], sizeof(mod_text[idx]), "%s", bar->time_str);
    modules[idx++] =
        (typeof(modules[0])){mod_text[idx - 1], &clock_fg, &clock_bg, true};
  }
  mod_count = idx;

  /* 计算右侧总宽度（含分隔符） */
  uint32_t sep_w = text_width(separator_str);
  uint32_t right_total_w = 0;
  for (int i = 0; i < mod_count; i++) {
    if (modules[i].enabled) {
      right_total_w += text_width(modules[i].text);
      if (i != 0)
        right_total_w += sep_w;
    }
  }

  uint32_t right_start = bar->width - 8 - right_total_w;
  if (right_start < x)
    right_start = x;

  /* 中间背景 */
  if (right_start > x) {
    pixman_image_fill_boxes(
        PIXMAN_OP_SRC, bg, bar->sel ? &middle_bg_sel : &middle_bg, 1,
        &(pixman_box32_t){
            .x1 = x, .x2 = right_start, .y1 = 0, .y2 = bar->height});
  }

  /* 标题模块（居中） */
  if (show_title && bar->title[0] != '\0' && right_start > x) {
    uint32_t tw = text_width(bar->title);
    uint32_t center_x = x + (right_start - x - tw) / 2;
    if (center_x < x + 8)
      center_x = x + 8;
    draw_text(bar->title, center_x, y, fg, fg_mask, bg, &title_fg, &title_bg,
              bar->width, bar->height);
  }

  /* 绘制右侧模块 */
  uint32_t cur_x = right_start;
  for (int i = 0; i < mod_count; i++) {
    if (!modules[i].enabled)
      continue;
    if (i != 0) {
      cur_x = draw_text(separator_str, cur_x, y, fg, fg_mask, bg, &separator_fg,
                        &separator_bg, bar->width, bar->height);
    }
    cur_x = draw_text(modules[i].text, cur_x, y, fg, fg_mask, bg, modules[i].fg,
                      modules[i].bg, bar->width, bar->height);
  }

  /* 合成最终图像 */
  pixman_image_composite32(PIXMAN_OP_OVER, bg, NULL, final, 0, 0, 0, 0, 0, 0,
                           bar->width, bar->height);
  pixman_image_set_alpha_map(fg, fg_mask, 0, 0);
  pixman_image_composite32(PIXMAN_OP_OVER, fg, fg_mask, final, 0, 0, 0, 0, 0, 0,
                           bar->width, bar->height);

  pixman_image_unref(fg);
  pixman_image_unref(fg_mask);
  pixman_image_unref(bg);
  pixman_image_unref(final);
  munmap(data, bar->bufsize);

  wl_surface_set_buffer_scale(bar->wl_surface, buffer_scale);
  wl_surface_attach(bar->wl_surface, buf, 0, 0);
  wl_surface_damage_buffer(bar->wl_surface, 0, 0, bar->width, bar->height);
  wl_surface_commit(bar->wl_surface);
}

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t w, uint32_t h) {
  Bar *bar = data;
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  if (bar->configured && w == bar->width && h == bar->height)
    return;
  bar->width = w * buffer_scale;
  bar->height = h * buffer_scale;
  bar->stride = bar->width * 4;
  bar->bufsize = bar->stride * bar->height;
  bar->configured = true;
  draw_bar(bar);
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *surface) {
  (void)data;
  (void)surface;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void output_name_handler(void *data, struct zxdg_output_v1 *xdg_output,
                                const char *name) {
  (void)xdg_output;
  Bar *bar = data;
  free(bar->name);
  bar->name = strdup(name);
}

static void output_logical_position(void *data,
                                    struct zxdg_output_v1 *xdg_output,
                                    int32_t x, int32_t y) {
  (void)data;
  (void)xdg_output;
  (void)x;
  (void)y;
}

static void output_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                                int32_t width, int32_t height) {
  (void)data;
  (void)xdg_output;
  (void)width;
  (void)height;
}

static void output_done(void *data, struct zxdg_output_v1 *xdg_output) {
  (void)data;
  (void)xdg_output;
}

static void output_description(void *data, struct zxdg_output_v1 *xdg_output,
                               const char *description) {
  (void)data;
  (void)xdg_output;
  (void)description;
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
  (void)data;
  (void)version;
  if (strcmp(interface, wl_compositor_interface.name) == 0)
    compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  else if (strcmp(interface, wl_shm_interface.name) == 0)
    shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0)
    layer_shell =
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
  else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    output_manager =
        wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
  else if (strcmp(interface, wl_output_interface.name) == 0) {
    Bar *bar = calloc(1, sizeof(Bar));
    bar->registry_name = name;
    bar->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    bar->xdg_output =
        zxdg_output_manager_v1_get_xdg_output(output_manager, bar->wl_output);
    zxdg_output_v1_add_listener(bar->xdg_output, &output_listener, bar);
    bar->height = bar_height * buffer_scale;
    bar->wl_surface = wl_compositor_create_surface(compositor);
    bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, bar->wl_surface, bar->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "mangobar");
    zwlr_layer_surface_v1_add_listener(bar->layer_surface,
                                       &layer_surface_listener, bar);
    zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, bar_height);
    zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar_height);
    wl_surface_commit(bar->wl_surface);
    wl_list_insert(&bar_list, &bar->link);
  }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
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
  if ((item = cJSON_GetObjectItem(json, "active")))
    bar->sel = cJSON_IsTrue(item);
  if ((item = cJSON_GetObjectItem(json, "layout_symbol")))
    strncpy(bar->layout, item->valuestring, sizeof(bar->layout) - 1);

  cJSON *client = cJSON_GetObjectItem(json, "active_client");
  if (client && !cJSON_IsNull(client)) {
    cJSON *t = cJSON_GetObjectItem(client, "title");
    cJSON *a = cJSON_GetObjectItem(client, "appid");
    const char *title_str = (t && cJSON_IsString(t)) ? t->valuestring : "";
    const char *appid_str = (a && cJSON_IsString(a)) ? a->valuestring : "";
    truncate_utf8_string(bar->title, title_str, sizeof(bar->title),
                         max_title_len);
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
      if (idx < 0 || idx >= TAG_COUNT)
        continue;
      if (cJSON_IsTrue(cJSON_GetObjectItem(tobj, "is_active")))
        bar->mtags |= 1 << idx;
      if (cJSON_IsTrue(cJSON_GetObjectItem(tobj, "is_urgent")))
        bar->urg |= 1 << idx;
      if (cJSON_GetObjectItem(tobj, "client_count")->valueint > 0)
        bar->ctags |= 1 << idx;
    }
  }

  /* 解析 active_tags，设置 atags 并判断 overview_mode */
  bar->atags = 0;
  bar->overview_mode = false; // 默认
  cJSON *active_tags = cJSON_GetObjectItem(json, "active_tags");
  if (active_tags && cJSON_IsArray(active_tags)) {
    int len = cJSON_GetArraySize(active_tags);
    if (len == 1) {
      cJSON *item0 = cJSON_GetArrayItem(active_tags, 0);
      if (cJSON_IsNumber(item0) && item0->valueint == 0) {
        bar->overview_mode = true;
        // 此时 atags 保持为 0，标签循环会用 overview_mode 绘制
        // OVERVIEW，不渲染任何具体标签
      } else {
        // 非 0 的单个数字，正常加入 atags
        int idx = item0->valueint - 1;
        if (idx >= 0 && idx < TAG_COUNT)
          bar->atags |= (1 << idx);
      }
    } else {
      // 多个标签，全部加入 atags
      cJSON *elem;
      cJSON_ArrayForEach(elem, active_tags) {
        if (cJSON_IsNumber(elem)) {
          int idx = elem->valueint - 1;
          if (idx >= 0 && idx < TAG_COUNT)
            bar->atags |= (1 << idx);
        }
      }
    }
  }

  bar->redraw = true;
}

static void process_ipc_msg(const char *msg) {
  cJSON *json = cJSON_Parse(msg);
  if (!json)
    return;

  cJSON *monitors = cJSON_GetObjectItem(json, "monitors");
  if (monitors && cJSON_IsArray(monitors)) {
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
  if (!path)
    return;
  ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ipc_fd < 0)
    return;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  if (connect(ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(ipc_fd);
    ipc_fd = -1;
    return;
  }
  fcntl(ipc_fd, F_SETFL, fcntl(ipc_fd, F_GETFL) | O_NONBLOCK);
}

static void ipc_subscribe(Bar *bar) {
  if (ipc_fd < 0 || !bar->name)
    return;
  char msg[256];
  snprintf(msg, sizeof(msg), "watch monitor %s\n", bar->name);
  send(ipc_fd, msg, strlen(msg), MSG_NOSIGNAL);
  snprintf(msg, sizeof(msg), "watch keymode\nwatch keyboardlayout\n");
  send(ipc_fd, msg, strlen(msg), MSG_NOSIGNAL);
}

static int cpu_prev_total, cpu_prev_idle;
static void update_system_info() {
  FILE *f = fopen("/proc/stat", "r");
  if (f) {
    char cpu[8];
    int user, nice, system, idle;
    if (fscanf(f, "%s %d %d %d %d", cpu, &user, &nice, &system, &idle) == 5) {
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
  f = fopen("/proc/meminfo", "r");
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
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  char ts[16];
  strftime(ts, sizeof(ts), "%H:%M", tm);
  Bar *b;
  wl_list_for_each(b, &bar_list, link) strcpy(b->time_str, ts);
}

static void event_loop() {
  int wl_fd = wl_display_get_fd(display);
  while (running) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(wl_fd, &rfds);
    if (ipc_fd >= 0)
      FD_SET(ipc_fd, &rfds);
    int maxfd = (ipc_fd > wl_fd) ? ipc_fd : wl_fd;
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    wl_display_flush(display);

    int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (FD_ISSET(wl_fd, &rfds))
      wl_display_dispatch(display);
    if (ipc_fd >= 0 && FD_ISSET(ipc_fd, &rfds)) {
      ssize_t n = read(ipc_fd, ipc_buf + ipc_buf_len,
                       sizeof(ipc_buf) - ipc_buf_len - 1);
      if (n > 0) {
        ipc_buf_len += n;
        ipc_buf[ipc_buf_len] = '\0';
        process_ipc_data();
      } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
        close(ipc_fd);
        ipc_fd = -1;
        ipc_buf_len = 0;
      }
    }
    static time_t last_sec;
    time_t sec = time(NULL);
    if (sec != last_sec) {
      last_sec = sec;
      update_system_info();
      Bar *b;
      wl_list_for_each(b, &bar_list, link) b->redraw = true;
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

static void init_colors() {
  hex_to_pixman(active_fg_color_hex, &active_fg);
  hex_to_pixman(active_bg_color_hex, &active_bg);
  hex_to_pixman(occupied_fg_color_hex, &occupied_fg);
  hex_to_pixman(occupied_bg_color_hex, &occupied_bg);
  hex_to_pixman(inactive_fg_color_hex, &inactive_fg);
  hex_to_pixman(inactive_bg_color_hex, &inactive_bg);
  hex_to_pixman(urgent_fg_color_hex, &urgent_fg);
  hex_to_pixman(urgent_bg_color_hex, &urgent_bg);
  hex_to_pixman(empty_fg_color_hex, &empty_fg);
  hex_to_pixman(empty_bg_color_hex, &empty_bg);

  hex_to_pixman(layout_fg_color_hex, &layout_fg);
  hex_to_pixman(layout_bg_color_hex, &layout_bg);
  hex_to_pixman(title_fg_color_hex, &title_fg);
  hex_to_pixman(title_bg_color_hex, &title_bg);

  hex_to_pixman(cpu_fg_color_hex, &cpu_fg);
  hex_to_pixman(cpu_bg_color_hex, &cpu_bg);
  hex_to_pixman(mem_fg_color_hex, &mem_fg);
  hex_to_pixman(mem_bg_color_hex, &mem_bg);
  hex_to_pixman(clock_fg_color_hex, &clock_fg);
  hex_to_pixman(clock_bg_color_hex, &clock_bg);
  hex_to_pixman(keymode_fg_color_hex, &keymode_fg);
  hex_to_pixman(keymode_bg_color_hex, &keymode_bg);
  hex_to_pixman(keyboardlayout_fg_color_hex, &keyboardlayout_fg);
  hex_to_pixman(keyboardlayout_bg_color_hex, &keyboardlayout_bg);

  hex_to_pixman(overview_fg_color_hex, &overview_fg);
  hex_to_pixman(overview_bg_color_hex, &overview_bg);

  hex_to_pixman(separator_fg_color_hex, &separator_fg);
  hex_to_pixman(separator_bg_color_hex, &separator_bg);

  hex_to_pixman(middle_bg_color_hex, &middle_bg);
  hex_to_pixman(middle_bg_sel_color_hex, &middle_bg_sel);
}

int main() {
  fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
  font = fcft_from_name(1, (const char *[]){fontstr}, NULL);
  if (!font) {
    fprintf(stderr, "Failed to load fonts\n");
    return 1;
  }

  init_colors();
  display = wl_display_connect(NULL);
  if (!display)
    return 1;
  wl_list_init(&bar_list);

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  if (!compositor || !shm || !layer_shell || !output_manager)
    return 1;

  ipc_connect();
  if (ipc_fd >= 0) {
    wl_display_roundtrip(display);
    Bar *b;
    wl_list_for_each(b, &bar_list, link) ipc_subscribe(b);
  }
  update_system_info();
  signal(SIGTERM, exit);
  signal(SIGINT, exit);

  event_loop();
  if (ipc_fd >= 0)
    close(ipc_fd);
  fcft_destroy(font);
  fcft_fini();
  wl_display_disconnect(display);
  return 0;
}
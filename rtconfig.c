#define _GNU_SOURCE
#include "rtconfig.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

MangoConfig g_cfg;

static void cfg_set(char *dst, size_t sz, const char *s) {
  if (s)
    snprintf(dst, sz, "%s", s);
}

// Strip // and block comments, and trailing commas (JSONC compatible)
static char *jsonc_strip(const char *src) {
  size_t n = strlen(src);
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  size_t o = 0;
  bool in_str = false;
  for (size_t i = 0; i < n; i++) {
    char c = src[i];
    if (in_str) {
      out[o++] = c;
      if (c == '\\' && i + 1 < n)
        out[o++] = src[++i];
      else if (c == '"')
        in_str = false;
      continue;
    }
    if (c == '"') {
      in_str = true;
      out[o++] = c;
      continue;
    }
    if (c == '/' && i + 1 < n && src[i + 1] == '/') {
      while (i < n && src[i] != '\n')
        i++;
      if (i < n)
        out[o++] = '\n';
      continue;
    }
    if (c == '/' && i + 1 < n && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/'))
        i++;
      i++; // skip '/'
      continue;
    }
    if (c == '}' || c == ']') {
      size_t j = o;
      while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t' ||
                       out[j - 1] == '\n' || out[j - 1] == '\r'))
        j--;
      if (j > 0 && out[j - 1] == ',')
        o = j - 1;
    }
    out[o++] = c;
  }
  out[o] = '\0';
  return out;
}

static int cfg_int(cJSON *obj, const char *key, int fallback) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsNumber(v))
    return v->valueint;
  if (cJSON_IsString(v))
    return atoi(v->valuestring);
  return fallback;
}

static bool cfg_bool(cJSON *obj, const char *key, bool fallback) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsBool(v))
    return cJSON_IsTrue(v);
  if (cJSON_IsString(v))
    return strcmp(v->valuestring, "true") == 0 ||
           strcmp(v->valuestring, "1") == 0;
  return fallback;
}

static void cfg_str(cJSON *obj, const char *key, char *dst, size_t sz) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsString(v))
    cfg_set(dst, sz, v->valuestring);
}

static void cfg_icons(cJSON *obj, const char *key, char icons[][16],
                      int *count, int max) {
  *count = 0;
  cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsArray(arr))
    return;
  cJSON *item;
  cJSON_ArrayForEach(item, arr) {
    if (*count >= max)
      break;
    if (cJSON_IsString(item))
      snprintf(icons[(*count)++], 16, "%s", item->valuestring);
  }
}

static int module_id(const char *name) {
  if (!name)
    return M_NONE;
  if (strcmp(name, "workspaces") == 0 || strcmp(name, "mango/workspaces") == 0)
    return M_TAGS;
  if (strcmp(name, "layout") == 0 || strcmp(name, "mango/layout") == 0)
    return M_LAYOUT;
  if (strcmp(name, "window") == 0 || strcmp(name, "mango/window") == 0)
    return M_TITLE;
  if (strcmp(name, "tray") == 0)
    return M_TRAY;
  if (strcmp(name, "cpu") == 0)
    return M_CPU;
  if (strcmp(name, "memory") == 0)
    return M_MEM;
  if (strcmp(name, "backlight") == 0)
    return M_BRIGHTNESS;
  if (strcmp(name, "pulseaudio") == 0)
    return M_VOLUME;
  if (strcmp(name, "clock") == 0 || strcmp(name, "clock#time") == 0)
    return M_CLOCK_TIME;
  if (strcmp(name, "clock#date") == 0)
    return M_CLOCK_DATE;
  if (strcmp(name, "keymode") == 0 || strcmp(name, "mango/keymode") == 0)
    return M_KEYMODE;
  if (strcmp(name, "keyboardlayout") == 0 ||
      strcmp(name, "keyboard-layout") == 0 ||
      strcmp(name, "mango/language") == 0 ||
      strcmp(name, "language") == 0)
    return M_KBLAYOUT;
  if (strcmp(name, "network") == 0)
    return M_NETWORK;
  if (strcmp(name, "hide_clients") == 0 || strcmp(name, "hideclients") == 0)
    return M_HIDE_CLIENTS;
  if (strcmp(name, "battery") == 0)
    return M_BATTERY;
  return M_NONE;
}

// Map a config key ("clock#date", "custom/x", ...) to the internal module
// name used for hotspots/actions.
static const char *module_internal_name(const char *name) {
  static char custom[64];
  if (!name)
    return NULL;
  if (strcmp(name, "workspaces") == 0 ||
      strcmp(name, "mango/workspaces") == 0)
    return "tags";
  if (strcmp(name, "layout") == 0 || strcmp(name, "mango/layout") == 0)
    return "layout";
  if (strcmp(name, "window") == 0 || strcmp(name, "mango/window") == 0)
    return "title";
  if (strcmp(name, "clock#date") == 0)
    return "clock.date";
  if (strcmp(name, "clock") == 0 || strcmp(name, "clock#time") == 0)
    return "clock";
  if (strcmp(name, "memory") == 0)
    return "mem";
  if (strcmp(name, "backlight") == 0)
    return "brightness";
  if (strcmp(name, "pulseaudio") == 0)
    return "volume";
  if (strcmp(name, "hideclients") == 0 || strcmp(name, "hide_clients") == 0)
    return "hideclients";
  if (strncmp(name, "custom/", 7) == 0) {
    snprintf(custom, sizeof(custom), "custom-%s", name + 7);
    return custom;
  }
  return name; // cpu, network, battery, keymode, keyboardlayout, tray...
}

// Convert {:L%H:%M} time format to strftime
static void convert_clock_format(const char *in, char *out, size_t sz) {
  size_t o = 0;
  const char *p = in ? in : "";
  while (*p && o + 1 < sz) {
    if (p[0] == '{' && p[1] == ':' && p[2] == 'L') {
      p += 3;
      while (*p && *p != '}' && o + 1 < sz)
        out[o++] = *p++;
      if (*p == '}')
        p++;
    } else {
      out[o++] = *p++;
    }
  }
  out[o] = '\0';
}

static void add_action(const char *module, const char *left,
                       const char *middle, const char *right,
                       const char *scroll_up, const char *scroll_down) {
  if (g_cfg.action_count >= MANGOBAR_MAX_ACTIONS)
    return;
  MangoAction *a = &g_cfg.actions[g_cfg.action_count];
  cfg_set(a->module, sizeof(a->module), module);
  cfg_set(a->left, sizeof(a->left), left);
  cfg_set(a->middle, sizeof(a->middle), middle);
  cfg_set(a->right, sizeof(a->right), right);
  cfg_set(a->scroll_up, sizeof(a->scroll_up), scroll_up);
  cfg_set(a->scroll_down, sizeof(a->scroll_down), scroll_down);
  a->scroll_interval = -1;
  a->smooth_scroll_threshold = -1.0;
  g_cfg.action_count++;
}

static void set_action(const char *module, const char *left,
                       const char *middle, const char *right,
                       const char *scroll_up, const char *scroll_down) {
  for (int i = 0; i < g_cfg.action_count; i++) {
    if (strcmp(g_cfg.actions[i].module, module) == 0) {
      cfg_set(g_cfg.actions[i].left, sizeof(g_cfg.actions[i].left), left);
      cfg_set(g_cfg.actions[i].middle, sizeof(g_cfg.actions[i].middle),
              middle);
      cfg_set(g_cfg.actions[i].right, sizeof(g_cfg.actions[i].right), right);
      cfg_set(g_cfg.actions[i].scroll_up, sizeof(g_cfg.actions[i].scroll_up),
              scroll_up);
      cfg_set(g_cfg.actions[i].scroll_down,
              sizeof(g_cfg.actions[i].scroll_down), scroll_down);
      return;
    }
  }
  add_action(module, left, middle, right, scroll_up, scroll_down);
}

static void set_alt(const char *module, const char *fmt) {
  if (!module || !fmt || !*fmt)
    return;
  for (int i = 0; i < g_cfg.alt_count; i++) {
    if (strcmp(g_cfg.alts[i].module, module) == 0) {
      cfg_set(g_cfg.alts[i].fmt, sizeof(g_cfg.alts[i].fmt), fmt);
      return;
    }
  }
  if (g_cfg.alt_count >= MANGOBAR_MAX_ALTS)
    return;
  MangoAltFormat *a = &g_cfg.alts[g_cfg.alt_count++];
  snprintf(a->module, sizeof(a->module), "%s", module);
  cfg_set(a->fmt, sizeof(a->fmt), fmt);
}

static void set_action_interval(const char *module, cJSON *m) {
  cJSON *iv = cJSON_GetObjectItemCaseSensitive(m, "scroll-interval");
  if (cJSON_IsNumber(iv)) {
    for (int i = 0; i < g_cfg.action_count; i++) {
      if (strcmp(g_cfg.actions[i].module, module) == 0) {
        g_cfg.actions[i].scroll_interval = iv->valueint;
        break;
      }
    }
  }
}

static void set_action_smooth_threshold(const char *module, cJSON *m) {
  cJSON *threshold =
      cJSON_GetObjectItemCaseSensitive(m, "smooth-scrolling-threshold");
  if (!cJSON_IsNumber(threshold) || threshold->valuedouble <= 0.0)
    return;
  for (int i = 0; i < g_cfg.action_count; i++) {
    if (strcmp(g_cfg.actions[i].module, module) == 0) {
      g_cfg.actions[i].smooth_scroll_threshold = threshold->valuedouble;
      break;
    }
  }
}

// Register all click/scroll actions of a module block at once.
static void set_module_actions(cJSON *m, const char *module) {
  set_action(module,
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
  set_action_interval(module, m);
  set_action_smooth_threshold(module, m);
}

static void cfg_alt(cJSON *m, const char *module) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(m, "format-alt");
  if (cJSON_IsString(v))
    set_alt(module, v->valuestring);
}

static MangoCustomModule *lookup_custom(const char *name) {
  for (int i = 0; i < g_cfg.custom_count; i++) {
    if (strcmp(g_cfg.customs[i].name, name) == 0)
      return &g_cfg.customs[i];
  }
  return NULL;
}

static MangoCustomModule *find_custom(const char *name) {
  MangoCustomModule *cm = lookup_custom(name);
  if (cm)
    return cm;
  if (g_cfg.custom_count >= MANGOBAR_MAX_CUSTOM)
    return NULL;
  cm = &g_cfg.customs[g_cfg.custom_count++];
  memset(cm, 0, sizeof(*cm));
  snprintf(cm->name, sizeof(cm->name), "%s", name);
  cm->enabled = true;
  return cm;
}

// Parse a custom/<name> module definition
static void parse_custom_module(cJSON *obj, const char *name) {
  // Only configure modules that were actually placed in a modules-* list
  MangoCustomModule *cm = lookup_custom(name);
  if (!cm)
    return;
  cfg_str(obj, "exec", cm->exec, sizeof(cm->exec));
  cm->interval = cfg_int(obj, "interval", 0);
  cfg_str(obj, "format", cm->format, sizeof(cm->format));

  char css_name[64];
  snprintf(css_name, sizeof(css_name), "custom-%s", name);
  cfg_alt(obj, css_name);
  set_action(css_name,
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "on-click")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "on-click-middle")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "on-click-right")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "on-scroll-up")),
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, "on-scroll-down")));
  set_action_smooth_threshold(css_name, obj);
}

// Map "activate"/"toggle" actions to mango IPC commands
static const char *map_workspace_action(const char *a) {
  if (!a)
    return NULL;
  if (strcmp(a, "activate") == 0)
    return "@view";
  if (strcmp(a, "toggle") == 0)
    return "@toggle";
  return a;
}

static void add_module(int *order, int *count, int id) {
  if (*count < MANGOBAR_MAX_MODULES)
    order[(*count)++] = id;
}

static void parse_module_list(cJSON *root, const char *key, int *order,
                              int *count) {
  cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsArray(arr))
    return;
  cJSON *item;
  cJSON_ArrayForEach(item, arr) {
    if (!cJSON_IsString(item))
      continue;
    const char *name = item->valuestring;
    if (strncmp(name, "custom/", 7) == 0) {
      MangoCustomModule *cm = find_custom(name + 7);
      if (cm)
        add_module(order, count, M_CUSTOM + (cm - g_cfg.customs));
      continue;
    }
    int id = module_id(name);
    if (id == M_NONE)
      continue;
    add_module(order, count, id);
  }
}

static void parse_modules(cJSON *root) {
  g_cfg.left_count = g_cfg.center_count = g_cfg.right_count = 0;
  parse_module_list(root, "modules-left", g_cfg.left_order, &g_cfg.left_count);
  parse_module_list(root, "modules-center", g_cfg.center_order,
                    &g_cfg.center_count);
  parse_module_list(root, "modules-right", g_cfg.right_order,
                    &g_cfg.right_count);
}

static void parse_module_configs(cJSON *root) {
  cJSON *m;

  // Per-module max-length (0 = unlimited)
  g_cfg.max_len_count = 0;
  cJSON *child;
  cJSON_ArrayForEach(child, root) {
    if (!cJSON_IsObject(child) || !child->string)
      continue;
    cJSON *ml = cJSON_GetObjectItemCaseSensitive(child, "max-length");
    if (!cJSON_IsNumber(ml))
      continue;
    const char *iname = module_internal_name(child->string);
    if (iname && g_cfg.max_len_count < MANGOBAR_MAX_LENS) {
      snprintf(g_cfg.max_lens[g_cfg.max_len_count].module, 32, "%.31s", iname);
      g_cfg.max_lens[g_cfg.max_len_count].max_length = ml->valueint;
      g_cfg.max_len_count++;
    }
  }

  // Custom modules live under custom/<name> keys
  cJSON_ArrayForEach(child, root) {
    if (!cJSON_IsObject(child) || !child->string)
      continue;
    if (strncmp(child->string, "custom/", 7) == 0)
      parse_custom_module(child, child->string + 7);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "workspaces");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/workspaces");
  if (cJSON_IsObject(m)) {
    g_cfg.only_occupied = cfg_bool(m, "hide-empty", true);
    cfg_str(m, "overview-label", g_cfg.overview_label,
            sizeof(g_cfg.overview_label));
    // Tags that stay visible even when empty (1-based numbers)
    cJSON *pin = cJSON_GetObjectItemCaseSensitive(m, "pinned");
    if (cJSON_IsArray(pin)) {
      g_cfg.pinned_tags = 0;
      cJSON *it;
      cJSON_ArrayForEach(it, pin) {
        if (cJSON_IsNumber(it) && it->valueint >= 1 &&
            it->valueint <= MANGOBAR_MAX_TAGS)
          g_cfg.pinned_tags |= (uint32_t)1 << (it->valueint - 1);
      }
    }
    // Custom tag labels (index 0 = tag 1)
    cJSON *tn = cJSON_GetObjectItemCaseSensitive(m, "tag-names");
    if (cJSON_IsArray(tn)) {
      int idx = 0;
      cJSON *it;
      cJSON_ArrayForEach(it, tn) {
        if (idx >= MANGOBAR_MAX_TAGS)
          break;
        if (cJSON_IsString(it))
          snprintf(g_cfg.tag_names[idx], sizeof(g_cfg.tag_names[idx]), "%s",
                   it->valuestring);
        idx++;
      }
    }
    set_action("tags", map_workspace_action(
                           cJSON_GetObjectItemCaseSensitive(m, "on-click")
                               ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click"))
                               : NULL),
               NULL,
               map_workspace_action(
                   cJSON_GetObjectItemCaseSensitive(m, "on-click-right")
                       ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right"))
                       : NULL),
               cJSON_GetStringValue(
                   cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(
                   cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_interval("tags", m);
    set_action_smooth_threshold("tags", m);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "layout");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/layout");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.layout_format, sizeof(g_cfg.layout_format));
    cfg_alt(m, "layout");
    set_module_actions(m, "layout");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "window");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/window");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.title_format, sizeof(g_cfg.title_format));
    cfg_alt(m, "title");
    set_module_actions(m, "title");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "keymode");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/keymode");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.keymode_format, sizeof(g_cfg.keymode_format));
    cfg_alt(m, "keymode");
    set_module_actions(m, "keymode");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "keyboardlayout");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "language");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/language");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.keyboardlayout_format,
            sizeof(g_cfg.keyboardlayout_format));
    cfg_alt(m, "keyboardlayout");
    set_module_actions(m, "keyboardlayout");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "tray");
  if (cJSON_IsObject(m)) {
    g_cfg.tray_icon_size = cfg_int(m, "icon-size", g_cfg.tray_icon_size);
    g_cfg.tray_gap = cfg_int(m, "spacing", g_cfg.tray_gap);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "cpu");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.cpu_format, sizeof(g_cfg.cpu_format));
    cfg_alt(m, "cpu");
    set_action("cpu", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_smooth_threshold("cpu", m);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "memory");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.mem_format, sizeof(g_cfg.mem_format));
    cfg_alt(m, "mem");
    set_action("mem", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_smooth_threshold("mem", m);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "backlight");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.brightness_fmt,
            sizeof(g_cfg.brightness_fmt));
    cfg_icons(m, "icons", g_cfg.brightness_icons,
              &g_cfg.brightness_icon_count, MANGOBAR_MAX_ICONS);
    cfg_alt(m, "brightness");
    cfg_str(m, "device", g_cfg.brightness_dev,
            sizeof(g_cfg.brightness_dev));
    set_action("brightness",
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_smooth_threshold("brightness", m);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "pulseaudio");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.volume_fmt, sizeof(g_cfg.volume_fmt));
    cfg_str(m, "format-muted", g_cfg.volume_fmt_muted,
            sizeof(g_cfg.volume_fmt_muted));
    cfg_str(m, "icon-muted", g_cfg.volume_muted_icon,
            sizeof(g_cfg.volume_muted_icon));
    cfg_str(m, "icon-bluetooth", g_cfg.volume_bt_icon,
            sizeof(g_cfg.volume_bt_icon));
    cfg_icons(m, "icons", g_cfg.volume_icons, &g_cfg.volume_icon_count,
              MANGOBAR_MAX_ICONS);
    cfg_alt(m, "volume");
    set_action("volume",
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_smooth_threshold("volume", m);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "clock#time");
  if (cJSON_IsObject(m)) {
    char tmp[128];
    cfg_str(m, "format", tmp, sizeof(tmp));
    if (tmp[0])
      convert_clock_format(tmp, g_cfg.clock_time_format,
                           sizeof(g_cfg.clock_time_format));
    char alt[128] = {0};
    cfg_str(m, "format-alt", alt, sizeof(alt));
    if (alt[0]) {
      char conv[128];
      convert_clock_format(alt, conv, sizeof(conv));
      set_alt("clock", conv);
    }
    set_module_actions(m, "clock");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "clock#date");
  if (cJSON_IsObject(m)) {
    char tmp[128];
    cfg_str(m, "format", tmp, sizeof(tmp));
    if (tmp[0])
      convert_clock_format(tmp, g_cfg.clock_date_format,
                           sizeof(g_cfg.clock_date_format));
    char alt[128] = {0};
    cfg_str(m, "format-alt", alt, sizeof(alt));
    if (alt[0]) {
      char conv[128];
      convert_clock_format(alt, conv, sizeof(conv));
      set_alt("clock.date", conv);
    }
    set_module_actions(m, "clock.date");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "clock");
  if (cJSON_IsObject(m) && !cJSON_GetObjectItemCaseSensitive(root, "clock#time")) {
    char tmp[128];
    cfg_str(m, "format", tmp, sizeof(tmp));
    if (tmp[0])
      convert_clock_format(tmp, g_cfg.clock_time_format,
                           sizeof(g_cfg.clock_time_format));
    char alt[128] = {0};
    cfg_str(m, "format-alt", alt, sizeof(alt));
    if (alt[0]) {
      char conv[128];
      convert_clock_format(alt, conv, sizeof(conv));
      set_alt("clock", conv);
    }
    set_module_actions(m, "clock");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "network");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.network_format, sizeof(g_cfg.network_format));
    cfg_alt(m, "network");
    set_module_actions(m, "network");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "hideclients");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "hide_clients");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.hide_clients_format,
            sizeof(g_cfg.hide_clients_format));
    cfg_alt(m, "hideclients");
    set_action("hideclients",
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_smooth_threshold("hideclients", m);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "battery");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", g_cfg.battery_fmt, sizeof(g_cfg.battery_fmt));
    cfg_str(m, "device", g_cfg.battery_dev, sizeof(g_cfg.battery_dev));
    cfg_str(m, "icon-charging", g_cfg.battery_icon_charging,
            sizeof(g_cfg.battery_icon_charging));
    cfg_str(m, "icon-full", g_cfg.battery_icon_full,
            sizeof(g_cfg.battery_icon_full));
    cfg_str(m, "icon-discharging", g_cfg.battery_icon_discharging,
            sizeof(g_cfg.battery_icon_discharging));
    cfg_str(m, "icon-ac", g_cfg.battery_icon_ac,
            sizeof(g_cfg.battery_icon_ac));
    cfg_icons(m, "icons", g_cfg.battery_icons, &g_cfg.battery_icon_count,
              MANGOBAR_MAX_ICONS);
    g_cfg.hide_on_ac = cfg_bool(m, "hide-on-ac", false);
    cfg_alt(m, "battery");
    set_action("battery",
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_smooth_threshold("battery", m);
  }
}

void mango_config_defaults(void) {
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.bar_height = 30;
  g_cfg.buffer_scale = 1;
  snprintf(g_cfg.font, sizeof(g_cfg.font), "%s",
           "Maple Mono NF CN:style=Bold:size=24");
  g_cfg.radius_default = 4;
  g_cfg.layer = 2; // TOP
  g_cfg.max_title_len = 50;
  g_cfg.sys_interval = 2;
  g_cfg.smooth_scroll_threshold = 5.0;
  g_cfg.tag_count = MANGOBAR_MAX_TAGS;
  for (int i = 0; i < g_cfg.tag_count && i < MANGOBAR_MAX_TAGS; i++)
    snprintf(g_cfg.tag_names[i], sizeof(g_cfg.tag_names[i]), "%d", i + 1);
  snprintf(g_cfg.overview_label, sizeof(g_cfg.overview_label), "%s",
           "OVERVIEW");
  g_cfg.only_occupied = true;
  snprintf(g_cfg.separator, sizeof(g_cfg.separator), "%s", " | ");
  g_cfg.tray_pad = 2;
  g_cfg.tray_gap = 4;
  snprintf(g_cfg.brightness_dev, sizeof(g_cfg.brightness_dev), "%s",
           "");
  snprintf(g_cfg.brightness_fmt, sizeof(g_cfg.brightness_fmt), "%s",
           "☀{}%");
  snprintf(g_cfg.volume_ctrl, sizeof(g_cfg.volume_ctrl), "%s",
           "Master");
  g_cfg.volume_mix_index = 0;
  snprintf(g_cfg.volume_fmt, sizeof(g_cfg.volume_fmt), "%s",
           "♪{}%");
  snprintf(g_cfg.volume_fmt_muted, sizeof(g_cfg.volume_fmt_muted), "%s",
           "🔇 {}%");
  snprintf(g_cfg.layout_format, sizeof(g_cfg.layout_format), "%s", "{}");
  snprintf(g_cfg.title_format, sizeof(g_cfg.title_format), "%s", "{}");
  snprintf(g_cfg.cpu_format, sizeof(g_cfg.cpu_format), "%s", "CPU:{}%");
  snprintf(g_cfg.mem_format, sizeof(g_cfg.mem_format), "%s", "MEM:{}%");
  snprintf(g_cfg.clock_time_format, sizeof(g_cfg.clock_time_format), "%s",
           "%H:%M");
  snprintf(g_cfg.clock_date_format, sizeof(g_cfg.clock_date_format), "%s",
           "%m-%d %a");
  snprintf(g_cfg.keymode_format, sizeof(g_cfg.keymode_format), "%s", "{}");
  snprintf(g_cfg.keyboardlayout_format, sizeof(g_cfg.keyboardlayout_format),
           "%s", "{}");
  snprintf(g_cfg.network_format, sizeof(g_cfg.network_format), "%s",
           "{ifname}");
  snprintf(g_cfg.hide_clients_format, sizeof(g_cfg.hide_clients_format), "%s",
           "{}");
  snprintf(g_cfg.battery_fmt, sizeof(g_cfg.battery_fmt), "%s",
           "{icon} {percent}% {status}");
  snprintf(g_cfg.battery_icon_charging, sizeof(g_cfg.battery_icon_charging),
           "%s", "󰂄");
  snprintf(g_cfg.battery_icon_full, sizeof(g_cfg.battery_icon_full), "%s",
           "󰁹");
  snprintf(g_cfg.battery_icon_discharging,
           sizeof(g_cfg.battery_icon_discharging), "%s", "󰁿");
  snprintf(g_cfg.battery_icon_ac, sizeof(g_cfg.battery_icon_ac), "%s", "");
  static const char *battery_level_icons[] = {
      "󰂎", "󰁺", "󰁻", "󰁼", "󰁽", "󰁾", "󰁿", "󰂀", "󰂁", "󰂂", "󰁹"};
  g_cfg.battery_icon_count = 0;
  for (size_t i = 0;
       i < sizeof(battery_level_icons) / sizeof(battery_level_icons[0]) &&
       g_cfg.battery_icon_count < MANGOBAR_MAX_ICONS;
       i++)
    snprintf(g_cfg.battery_icons[g_cfg.battery_icon_count++], 16, "%s",
             battery_level_icons[i]);
  static const char *brightness_level_icons[] = {
      "󰃚", "󰃛", "󰃜", "󰃝", "󰃞", "󰃟"};
  g_cfg.brightness_icon_count = 0;
  for (size_t i = 0;
       i < sizeof(brightness_level_icons) /
                sizeof(brightness_level_icons[0]) &&
       g_cfg.brightness_icon_count < MANGOBAR_MAX_ICONS;
       i++)
    snprintf(g_cfg.brightness_icons[g_cfg.brightness_icon_count++], 16, "%s",
             brightness_level_icons[i]);
  static const char *volume_level_icons[] = {"󰝟", "󰕿", "󰕾"};
  g_cfg.volume_icon_count = 0;
  for (size_t i = 0;
       i < sizeof(volume_level_icons) / sizeof(volume_level_icons[0]) &&
       g_cfg.volume_icon_count < MANGOBAR_MAX_ICONS;
       i++)
    snprintf(g_cfg.volume_icons[g_cfg.volume_icon_count++], 16, "%s",
             volume_level_icons[i]);
  snprintf(g_cfg.volume_muted_icon, sizeof(g_cfg.volume_muted_icon), "%s",
           "󰝟");
  snprintf(g_cfg.volume_bt_icon, sizeof(g_cfg.volume_bt_icon), "%s", "󰂯");
  add_action("tags", "@view", NULL, NULL, NULL, NULL);
  add_action("volume", NULL, NULL, NULL, "pamixer -i 2", "pamixer -d 2");
  add_action("brightness", NULL, NULL, NULL, "brightnessctl s +5%",
             "brightnessctl s 5%-");
  // Default module layout
  g_cfg.left_order[g_cfg.left_count++] = M_TAGS;
  g_cfg.left_order[g_cfg.left_count++] = M_LAYOUT;
  g_cfg.center_order[g_cfg.center_count++] = M_TITLE;
  g_cfg.right_order[g_cfg.right_count++] = M_TRAY;
  g_cfg.right_order[g_cfg.right_count++] = M_CPU;
  g_cfg.right_order[g_cfg.right_count++] = M_MEM;
  g_cfg.right_order[g_cfg.right_count++] = M_BRIGHTNESS;
  g_cfg.right_order[g_cfg.right_count++] = M_VOLUME;
  g_cfg.right_order[g_cfg.right_count++] = M_CLOCK_TIME;
  g_cfg.css_path[0] = '\0';
}

const char *mango_config_find_default(char *buf, size_t sz) {
  const char *env = getenv("MANGOBAR_CONFIG");
  if (env && *env && access(env, R_OK) == 0) {
    snprintf(buf, sz, "%s", env);
    return buf;
  }
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  char p[512];
  if (xdg && *xdg) {
    snprintf(p, sizeof(p), "%s/mangobar/config.jsonc", xdg);
    if (access(p, R_OK) == 0) {
      snprintf(buf, sz, "%s", p);
      return buf;
    }
  }
  if (home && *home) {
    snprintf(p, sizeof(p), "%s/.config/mangobar/config.jsonc", home);
    if (access(p, R_OK) == 0) {
      snprintf(buf, sz, "%s", p);
      return buf;
    }
  }
  return NULL;
}

int mango_config_parse(const char *jsonc) {
  char *stripped = jsonc_strip(jsonc);
  if (!stripped)
    return -1;
  cJSON *root = cJSON_Parse(stripped);
  free(stripped);
  if (!root)
    return -1;

  cJSON *v;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "height")) &&
      cJSON_IsNumber(v))
    g_cfg.bar_height = v->valueint;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "buffer-scale")) &&
      cJSON_IsNumber(v) && v->valueint > 0)
    g_cfg.buffer_scale = v->valueint;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "scroll-interval")) &&
      cJSON_IsNumber(v) && v->valueint >= 0)
    g_cfg.scroll_interval = v->valueint;
  if ((v = cJSON_GetObjectItemCaseSensitive(root,
                                             "smooth-scrolling-threshold")) &&
      cJSON_IsNumber(v) && v->valuedouble > 0.0)
    g_cfg.smooth_scroll_threshold = v->valuedouble;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "layer")) &&
      cJSON_IsString(v)) {
    if (strcmp(v->valuestring, "overlay") == 0)
      g_cfg.layer = 3;
    else if (strcmp(v->valuestring, "bottom") == 0)
      g_cfg.layer = 1;
    else
      g_cfg.layer = 2;
  }
  // CSS style file (accepts "css" or "style")
  v = cJSON_GetObjectItemCaseSensitive(root, "css");
  if (!cJSON_IsString(v))
    v = cJSON_GetObjectItemCaseSensitive(root, "style");
  if (cJSON_IsString(v))
    cfg_set(g_cfg.css_path, sizeof(g_cfg.css_path), v->valuestring);

  parse_modules(root);
  parse_module_configs(root);
  cJSON_Delete(root);
  return 0;
}

int mango_config_load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return -1;
  }
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  buf[rd] = '\0';
  fclose(f);
  int ret = mango_config_parse(buf);
  free(buf);
  return ret;
}

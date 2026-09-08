#define _GNU_SOURCE
#include "rtconfig.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

static MangoConfig legacy_cfg;
MangoConfig *g_cfg_ptr = &legacy_cfg;
static MangoConfig *parse_target = &legacy_cfg;

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
  if (parse_target->action_count >= MANGOBAR_MAX_ACTIONS)
    return;
  MangoAction *a = &parse_target->actions[parse_target->action_count];
  cfg_set(a->module, sizeof(a->module), module);
  cfg_set(a->left, sizeof(a->left), left);
  cfg_set(a->middle, sizeof(a->middle), middle);
  cfg_set(a->right, sizeof(a->right), right);
  cfg_set(a->scroll_up, sizeof(a->scroll_up), scroll_up);
  cfg_set(a->scroll_down, sizeof(a->scroll_down), scroll_down);
  a->scroll_interval = -1;
  a->smooth_scroll_threshold = -1.0;
  parse_target->action_count++;
}

static void set_action(const char *module, const char *left,
                       const char *middle, const char *right,
                       const char *scroll_up, const char *scroll_down) {
  for (int i = 0; i < parse_target->action_count; i++) {
    if (strcmp(parse_target->actions[i].module, module) == 0) {
      cfg_set(parse_target->actions[i].left, sizeof(parse_target->actions[i].left), left);
      cfg_set(parse_target->actions[i].middle, sizeof(parse_target->actions[i].middle),
              middle);
      cfg_set(parse_target->actions[i].right, sizeof(parse_target->actions[i].right), right);
      cfg_set(parse_target->actions[i].scroll_up, sizeof(parse_target->actions[i].scroll_up),
              scroll_up);
      cfg_set(parse_target->actions[i].scroll_down,
              sizeof(parse_target->actions[i].scroll_down), scroll_down);
      return;
    }
  }
  add_action(module, left, middle, right, scroll_up, scroll_down);
}

static void set_alt(const char *module, const char *fmt) {
  if (!module || !fmt || !*fmt)
    return;
  for (int i = 0; i < parse_target->alt_count; i++) {
    if (strcmp(parse_target->alts[i].module, module) == 0) {
      cfg_set(parse_target->alts[i].fmt, sizeof(parse_target->alts[i].fmt), fmt);
      return;
    }
  }
  if (parse_target->alt_count >= MANGOBAR_MAX_ALTS)
    return;
  MangoAltFormat *a = &parse_target->alts[parse_target->alt_count++];
  snprintf(a->module, sizeof(a->module), "%s", module);
  cfg_set(a->fmt, sizeof(a->fmt), fmt);
}

static void set_action_interval(const char *module, cJSON *m) {
  cJSON *iv = cJSON_GetObjectItemCaseSensitive(m, "scroll-interval");
  if (cJSON_IsNumber(iv)) {
    for (int i = 0; i < parse_target->action_count; i++) {
      if (strcmp(parse_target->actions[i].module, module) == 0) {
        parse_target->actions[i].scroll_interval = iv->valueint;
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
  for (int i = 0; i < parse_target->action_count; i++) {
    if (strcmp(parse_target->actions[i].module, module) == 0) {
      parse_target->actions[i].smooth_scroll_threshold = threshold->valuedouble;
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
  for (int i = 0; i < parse_target->custom_count; i++) {
    if (strcmp(parse_target->customs[i].name, name) == 0)
      return &parse_target->customs[i];
  }
  return NULL;
}

static MangoCustomModule *find_custom(const char *name) {
  MangoCustomModule *cm = lookup_custom(name);
  if (cm)
    return cm;
  if (parse_target->custom_count >= MANGOBAR_MAX_CUSTOM)
    return NULL;
  cm = &parse_target->customs[parse_target->custom_count++];
  memset(cm, 0, sizeof(*cm));
  snprintf(cm->name, sizeof(cm->name), "%s", name);
  cm->enabled = true;
  return cm;
}

static MangoBatteryCfg *lookup_battery(const char *name) {
  for (int i = 0; i < parse_target->battery_count; i++)
    if (strcmp(parse_target->batteries[i].name, name) == 0)
      return &parse_target->batteries[i];
  return NULL;
}

static MangoBatteryCfg *find_battery(const char *name) {
  MangoBatteryCfg *bc = lookup_battery(name);
  if (bc) {
    bc->enabled = true;
    return bc;
  }
  if (parse_target->battery_count >= MANGOBAR_MAX_BATTERIES)
    return NULL;
  bc = &parse_target->batteries[parse_target->battery_count++];
  // batteries[0] holds the defaults; inherit them for battery#<device> too
  *bc = parse_target->batteries[0];
  snprintf(bc->name, sizeof(bc->name), "%s", name);
  bc->enabled = true;
  if (strncmp(name, "battery#", 8) == 0)
    snprintf(bc->device, sizeof(bc->device), "%.31s", name + 8);
  else
    bc->device[0] = '\0';
  return bc;
}

static void parse_battery_module(cJSON *m, MangoBatteryCfg *bc) {
  cfg_str(m, "format", bc->fmt, sizeof(bc->fmt));
  if (strcmp(bc->name, "battery") == 0)
    cfg_str(m, "device", bc->device, sizeof(bc->device));
  cfg_str(m, "icon-charging", bc->icon_charging, sizeof(bc->icon_charging));
  cfg_str(m, "icon-full", bc->icon_full, sizeof(bc->icon_full));
  cfg_str(m, "icon-discharging", bc->icon_discharging,
          sizeof(bc->icon_discharging));
  cfg_str(m, "icon-ac", bc->icon_ac, sizeof(bc->icon_ac));
  // Only replace icons when the key is present, so battery#BAT0/BAT1 without
  // their own "icons" inherit the default level icons.
  if (cJSON_GetObjectItemCaseSensitive(m, "icons"))
    cfg_icons(m, "icons", bc->icons, &bc->icon_count, MANGOBAR_MAX_ICONS);
  bc->hide_on_ac = cfg_bool(m, "hide-on-ac", bc->hide_on_ac);
  cfg_alt(m, bc->name);
  set_module_actions(m, bc->name);
}

// Parse a custom/<name> module definition
static void parse_custom_module(cJSON *obj, const char *name) {
  // Only configure modules that were actually placed in a modules-* list
  MangoCustomModule *cm = lookup_custom(name);
  if (!cm)
    return;
  cfg_str(obj, "exec", cm->exec, sizeof(cm->exec));
  cm->interval = cfg_int(obj, "interval", 0);
  cm->signal = cfg_int(obj, "signal", 0);
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
        add_module(order, count, M_CUSTOM + (cm - parse_target->customs));
      continue;
    }
    if (strcmp(name, "battery") == 0 || strncmp(name, "battery#", 8) == 0) {
      MangoBatteryCfg *bc = find_battery(name);
      if (bc)
        add_module(order, count, M_BATTERY + (bc - parse_target->batteries));
      continue;
    }
    int id = module_id(name);
    if (id == M_NONE)
      continue;
    add_module(order, count, id);
  }
}

static void parse_modules(cJSON *root) {
  parse_target->left_count = parse_target->center_count = parse_target->right_count = 0;
  parse_module_list(root, "modules-left", parse_target->left_order, &parse_target->left_count);
  parse_module_list(root, "modules-center", parse_target->center_order,
                    &parse_target->center_count);
  parse_module_list(root, "modules-right", parse_target->right_order,
                    &parse_target->right_count);
}

static void parse_module_configs(cJSON *root) {
  cJSON *m;

  // Per-module max-length in pixels (0 = unlimited)
  parse_target->max_len_count = 0;
  cJSON *child;
  cJSON_ArrayForEach(child, root) {
    if (!cJSON_IsObject(child) || !child->string)
      continue;
    cJSON *ml = cJSON_GetObjectItemCaseSensitive(child, "max-length");
    if (!cJSON_IsNumber(ml))
      continue;
    const char *iname = module_internal_name(child->string);
    if (iname && parse_target->max_len_count < MANGOBAR_MAX_LENS) {
      snprintf(parse_target->max_lens[parse_target->max_len_count].module, 32, "%.31s", iname);
      parse_target->max_lens[parse_target->max_len_count].max_length = ml->valueint;
      parse_target->max_len_count++;
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
    parse_target->only_occupied = cfg_bool(m, "hide-empty", true);
    cfg_str(m, "overview-label", parse_target->overview_label,
            sizeof(parse_target->overview_label));
    // Tags that stay visible even when empty (1-based numbers)
    cJSON *pin = cJSON_GetObjectItemCaseSensitive(m, "pinned");
    if (cJSON_IsArray(pin)) {
      parse_target->pinned_tags = 0;
      cJSON *it;
      cJSON_ArrayForEach(it, pin) {
        if (cJSON_IsNumber(it) && it->valueint >= 1 &&
            it->valueint <= MANGOBAR_MAX_TAGS)
          parse_target->pinned_tags |= (uint32_t)1 << (it->valueint - 1);
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
          snprintf(parse_target->tag_names[idx], sizeof(parse_target->tag_names[idx]), "%s",
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
    cfg_str(m, "format", parse_target->layout_format, sizeof(parse_target->layout_format));
    cfg_alt(m, "layout");
    set_module_actions(m, "layout");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "window");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/window");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", parse_target->title_format, sizeof(parse_target->title_format));
    cfg_alt(m, "title");
    set_module_actions(m, "title");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "keymode");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/keymode");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", parse_target->keymode_format, sizeof(parse_target->keymode_format));
    cfg_alt(m, "keymode");
    set_module_actions(m, "keymode");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "keyboardlayout");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "language");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "mango/language");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", parse_target->keyboardlayout_format,
            sizeof(parse_target->keyboardlayout_format));
    cfg_alt(m, "keyboardlayout");
    set_module_actions(m, "keyboardlayout");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "tray");
  if (cJSON_IsObject(m)) {
    parse_target->tray_icon_size = cfg_int(m, "icon-size", parse_target->tray_icon_size);
    parse_target->tray_gap = cfg_int(m, "spacing", parse_target->tray_gap);
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "cpu");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", parse_target->cpu_format, sizeof(parse_target->cpu_format));
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
    cfg_str(m, "format", parse_target->mem_format, sizeof(parse_target->mem_format));
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
    cfg_str(m, "format", parse_target->brightness_fmt,
            sizeof(parse_target->brightness_fmt));
    cfg_icons(m, "icons", parse_target->brightness_icons,
              &parse_target->brightness_icon_count, MANGOBAR_MAX_ICONS);
    cfg_alt(m, "brightness");
    cfg_str(m, "device", parse_target->brightness_dev,
            sizeof(parse_target->brightness_dev));
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
    cfg_str(m, "format", parse_target->volume_fmt, sizeof(parse_target->volume_fmt));
    cfg_str(m, "format-muted", parse_target->volume_fmt_muted,
            sizeof(parse_target->volume_fmt_muted));
    cfg_str(m, "icon-muted", parse_target->volume_muted_icon,
            sizeof(parse_target->volume_muted_icon));
    cfg_str(m, "icon-bluetooth", parse_target->volume_bt_icon,
            sizeof(parse_target->volume_bt_icon));
    cfg_icons(m, "icons", parse_target->volume_icons, &parse_target->volume_icon_count,
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
      convert_clock_format(tmp, parse_target->clock_time_format,
                           sizeof(parse_target->clock_time_format));
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
      convert_clock_format(tmp, parse_target->clock_date_format,
                           sizeof(parse_target->clock_date_format));
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
      convert_clock_format(tmp, parse_target->clock_time_format,
                           sizeof(parse_target->clock_time_format));
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
    cfg_str(m, "format", parse_target->network_format, sizeof(parse_target->network_format));
    cfg_alt(m, "network");
    set_module_actions(m, "network");
  }

  m = cJSON_GetObjectItemCaseSensitive(root, "hideclients");
  if (!cJSON_IsObject(m))
    m = cJSON_GetObjectItemCaseSensitive(root, "hide_clients");
  if (cJSON_IsObject(m)) {
    cfg_str(m, "format", parse_target->hide_clients_format,
            sizeof(parse_target->hide_clients_format));
    cfg_alt(m, "hideclients");
    set_action("hideclients",
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-middle")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-click-right")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-up")),
               cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "on-scroll-down")));
    set_action_smooth_threshold("hideclients", m);
  }

  // Battery instances: "battery" plus "battery#<device>"
  cJSON_ArrayForEach(child, root) {
    if (!cJSON_IsObject(child) || !child->string)
      continue;
    if (strcmp(child->string, "battery") == 0 ||
        strncmp(child->string, "battery#", 8) == 0) {
      MangoBatteryCfg *bc = lookup_battery(child->string);
      if (bc)
        parse_battery_module(child, bc);
    }
  }
}

static void defaults_into(MangoConfig *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->bar_height = 30;
  cfg->buffer_scale = 1;
  snprintf(cfg->font, sizeof(cfg->font), "%s",
           "Maple Mono NF CN:style=Bold:size=24");
  cfg->radius_default = 4;
  cfg->layer = 2; // TOP
  cfg->max_title_len = 50;
  cfg->sys_interval = 2;
  cfg->smooth_scroll_threshold = 5.0;
  cfg->tag_count = MANGOBAR_MAX_TAGS;
  for (int i = 0; i < cfg->tag_count && i < MANGOBAR_MAX_TAGS; i++)
    snprintf(cfg->tag_names[i], sizeof(cfg->tag_names[i]), "%d", i + 1);
  snprintf(cfg->overview_label, sizeof(cfg->overview_label), "%s",
           "OVERVIEW");
  cfg->only_occupied = true;
  snprintf(cfg->separator, sizeof(cfg->separator), "%s", " | ");
  cfg->tray_pad = 2;
  cfg->tray_gap = 4;
  snprintf(cfg->brightness_dev, sizeof(cfg->brightness_dev), "%s",
           "");
  snprintf(cfg->brightness_fmt, sizeof(cfg->brightness_fmt), "%s",
           "☀{}%");
  snprintf(cfg->volume_fmt, sizeof(cfg->volume_fmt), "%s",
           "♪{}%");
  snprintf(cfg->volume_fmt_muted, sizeof(cfg->volume_fmt_muted), "%s",
           "🔇 {}%");
  snprintf(cfg->layout_format, sizeof(cfg->layout_format), "%s", "{}");
  snprintf(cfg->title_format, sizeof(cfg->title_format), "%s", "{}");
  snprintf(cfg->cpu_format, sizeof(cfg->cpu_format), "%s", "CPU:{}%");
  snprintf(cfg->mem_format, sizeof(cfg->mem_format), "%s", "MEM:{}%");
  snprintf(cfg->clock_time_format, sizeof(cfg->clock_time_format), "%s",
           "%H:%M");
  snprintf(cfg->clock_date_format, sizeof(cfg->clock_date_format), "%s",
           "%m-%d %a");
  snprintf(cfg->keymode_format, sizeof(cfg->keymode_format), "%s", "{}");
  snprintf(cfg->keyboardlayout_format, sizeof(cfg->keyboardlayout_format),
           "%s", "{}");
  snprintf(cfg->network_format, sizeof(cfg->network_format), "%s",
           "{ifname}");
  snprintf(cfg->hide_clients_format, sizeof(cfg->hide_clients_format), "%s",
           "{}");
  MangoBatteryCfg *bc0 = &cfg->batteries[0];
  cfg->battery_count = 1;
  snprintf(bc0->name, sizeof(bc0->name), "%s", "battery");
  snprintf(bc0->fmt, sizeof(bc0->fmt), "%s", "{icon} {percent}% {status}");
  snprintf(bc0->icon_charging, sizeof(bc0->icon_charging), "%s", "󰂄");
  snprintf(bc0->icon_full, sizeof(bc0->icon_full), "%s", "󰁹");
  snprintf(bc0->icon_discharging, sizeof(bc0->icon_discharging), "%s", "󰁿");
  snprintf(bc0->icon_ac, sizeof(bc0->icon_ac), "%s", "");
  static const char *battery_level_icons[] = {
      "󰂎", "󰁺", "󰁻", "󰁼", "󰁽", "󰁾", "󰁿", "󰂀", "󰂁", "󰂂", "󰁹"};
  for (size_t i = 0;
       i < sizeof(battery_level_icons) / sizeof(battery_level_icons[0]) &&
       bc0->icon_count < MANGOBAR_MAX_ICONS;
       i++)
    snprintf(bc0->icons[bc0->icon_count++], 16, "%s", battery_level_icons[i]);
  static const char *brightness_level_icons[] = {
      "󰃚", "󰃛", "󰃜", "󰃝", "󰃞", "󰃟"};
  cfg->brightness_icon_count = 0;
  for (size_t i = 0;
       i < sizeof(brightness_level_icons) /
                sizeof(brightness_level_icons[0]) &&
       cfg->brightness_icon_count < MANGOBAR_MAX_ICONS;
       i++)
    snprintf(cfg->brightness_icons[cfg->brightness_icon_count++], 16, "%s",
             brightness_level_icons[i]);
  static const char *volume_level_icons[] = {"󰝟", "󰕿", "󰕾"};
  cfg->volume_icon_count = 0;
  for (size_t i = 0;
       i < sizeof(volume_level_icons) / sizeof(volume_level_icons[0]) &&
       cfg->volume_icon_count < MANGOBAR_MAX_ICONS;
       i++)
    snprintf(cfg->volume_icons[cfg->volume_icon_count++], 16, "%s",
             volume_level_icons[i]);
  snprintf(cfg->volume_muted_icon, sizeof(cfg->volume_muted_icon), "%s",
           "󰝟");
  snprintf(cfg->volume_bt_icon, sizeof(cfg->volume_bt_icon), "%s", "󰂯");
  add_action("tags", "@view", NULL, NULL, NULL, NULL);
  add_action("volume", NULL, NULL, NULL, "pamixer -i 2", "pamixer -d 2");
  add_action("brightness", NULL, NULL, NULL, "brightnessctl s +5%",
             "brightnessctl s 5%-");
  // Default module layout
  cfg->left_order[cfg->left_count++] = M_TAGS;
  cfg->left_order[cfg->left_count++] = M_LAYOUT;
  cfg->center_order[cfg->center_count++] = M_TITLE;
  cfg->right_order[cfg->right_count++] = M_TRAY;
  cfg->right_order[cfg->right_count++] = M_CPU;
  cfg->right_order[cfg->right_count++] = M_MEM;
  cfg->right_order[cfg->right_count++] = M_BRIGHTNESS;
  cfg->right_order[cfg->right_count++] = M_VOLUME;
  cfg->right_order[cfg->right_count++] = M_CLOCK_TIME;
  cfg->css_path[0] = '\0';
}

void mango_config_defaults(void) {
  defaults_into(g_cfg_ptr);
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

static int parse_object_into(cJSON *root, MangoConfig *cfg) {
  MangoConfig *saved = parse_target;
  parse_target = cfg;
  cJSON *v;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "height")) &&
      cJSON_IsNumber(v))
    cfg->bar_height = v->valueint;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "buffer-scale")) &&
      cJSON_IsNumber(v) && v->valueint > 0)
    cfg->buffer_scale = v->valueint;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "scroll-interval")) &&
      cJSON_IsNumber(v) && v->valueint >= 0)
    cfg->scroll_interval = v->valueint;
  if ((v = cJSON_GetObjectItemCaseSensitive(root,
                                             "smooth-scrolling-threshold")) &&
      cJSON_IsNumber(v) && v->valuedouble > 0.0)
    cfg->smooth_scroll_threshold = v->valuedouble;
  if ((v = cJSON_GetObjectItemCaseSensitive(root, "layer")) &&
      cJSON_IsString(v)) {
    if (strcmp(v->valuestring, "overlay") == 0)
      cfg->layer = 3;
    else if (strcmp(v->valuestring, "bottom") == 0)
      cfg->layer = 1;
    else
      cfg->layer = 2;
  }
  // CSS style file (accepts "css" or "style")
  v = cJSON_GetObjectItemCaseSensitive(root, "css");
  if (!cJSON_IsString(v))
    v = cJSON_GetObjectItemCaseSensitive(root, "style");
  if (cJSON_IsString(v))
    cfg_set(cfg->css_path, sizeof(cfg->css_path), v->valuestring);

  parse_modules(root);
  parse_module_configs(root);
  parse_target = saved;
  return 0;
}

int mango_config_parse(const char *jsonc) {
  char *stripped = jsonc_strip(jsonc);
  if (!stripped)
    return -1;
  cJSON *root = cJSON_Parse(stripped);
  free(stripped);
  if (!root)
    return -1;

  int ret = parse_object_into(root, g_cfg_ptr);
  cJSON_Delete(root);
  return ret;
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

static int set_err(char *err, size_t errsz, const char *fmt, ...) {
  if (err && errsz) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
  }
  return -1;
}

static int parse_profile_outputs(MangoConfigProfile *p, cJSON *item,
                                 int index, char *err, size_t errsz) {
  cJSON *o = cJSON_GetObjectItemCaseSensitive(item, "output");
  p->output_count = 0;
  if (cJSON_IsString(o)) {
    p->output_names[0] = strdup(o->valuestring);
    if (!p->output_names[0])
      return set_err(err, errsz, "config[%d]: out of memory", index);
    if (!p->output_names[0][0]) {
      free(p->output_names[0]);
      p->output_names[0] = NULL;
      return set_err(err, errsz, "config[%d]: empty output name", index);
    }
    p->output_count = 1;
  } else if (cJSON_IsArray(o)) {
    int n = cJSON_GetArraySize(o);
    if (n > MANGOBAR_MAX_OUTPUT_NAMES)
      return set_err(err, errsz, "config[%d]: too many output names (%d > %d)",
                     index, n, MANGOBAR_MAX_OUTPUT_NAMES);
    for (int j = 0; j < n; j++) {
      cJSON *e = cJSON_GetArrayItem(o, j);
      if (!cJSON_IsString(e)) {
        for (int k = 0; k < j; k++)
          free(p->output_names[k]);
        p->output_count = 0;
        return set_err(err, errsz, "config[%d]: output[%d] must be a string",
                       index, j);
      }
      if (!e->valuestring[0]) {
        for (int k = 0; k < j; k++)
          free(p->output_names[k]);
        p->output_count = 0;
        return set_err(err, errsz, "config[%d]: output[%d] is empty", index, j);
      }
      p->output_names[j] = strdup(e->valuestring);
      if (!p->output_names[j])
        return set_err(err, errsz, "config[%d]: out of memory", index);
      p->output_count = (size_t)j + 1;
    }
  } else if (o != NULL) {
    return set_err(err, errsz,
                   "config[%d]: 'output' must be a string or an array of strings",
                   index);
  }
  return 0;
}

static int parse_set_single(MangoConfigSet *set, cJSON *obj, char *err,
                            size_t errsz) {
  set->profiles = calloc(1, sizeof(MangoConfigProfile));
  if (!set->profiles)
    return set_err(err, errsz, "out of memory");
  set->count = 1;
  set->default_index = 0;
  MangoConfigProfile *p = &set->profiles[0];
  defaults_into(&p->config);
  if (parse_profile_outputs(p, obj, 0, err, errsz) != 0)
    return -1;
  if (cJSON_GetObjectItemCaseSensitive(obj, "output") != NULL)
    set->default_index = -1;
  if (parse_object_into(obj, &p->config) != 0)
    return set_err(err, errsz, "failed to parse config");
  return 0;
}

// Validate profiles in a cJSON array (a root `[...]` config).
static int parse_profiles(MangoConfigSet *set, cJSON *arr, char *err,
                          size_t errsz) {
  int n = cJSON_GetArraySize(arr);
  if (n == 0) {
    return set_err(err, errsz, "no config objects found");
  }
  if (n > MANGOBAR_MAX_CONFIGS)
    return set_err(err, errsz, "too many config items (%d > %d)", n,
                   MANGOBAR_MAX_CONFIGS);

  set->profiles = calloc((size_t)n, sizeof(MangoConfigProfile));
  if (!set->profiles)
    return set_err(err, errsz, "out of memory");
  set->count = (size_t)n;

  for (int i = 0; i < n; i++) {
    cJSON *item = cJSON_GetArrayItem(arr, i);
    if (!cJSON_IsObject(item))
      return set_err(err, errsz, "config[%d]: must be an object", i);
    if (cJSON_GetObjectItemCaseSensitive(item, "css") != NULL ||
        cJSON_GetObjectItemCaseSensitive(item, "style") != NULL)
      return set_err(err, errsz,
                     "config[%d]: 'css'/'style' are not allowed in profile "
                     "mode", i);
  }

  int default_seen = -1;
  for (int i = 0; i < n; i++) {
    cJSON *item = cJSON_GetArrayItem(arr, i);
    MangoConfigProfile *p = &set->profiles[i];

    if (parse_profile_outputs(p, item, i, err, errsz) != 0)
      return -1;
    if (cJSON_GetObjectItemCaseSensitive(item, "output") == NULL) {
      if (default_seen >= 0)
        return set_err(err, errsz,
                       "config[%d]: multiple default configs (also config[%d])",
                       i, default_seen);
      default_seen = i;
      set->default_index = i;
    }

    defaults_into(&p->config);
    if (parse_object_into(item, &p->config) != 0)
      return set_err(err, errsz, "config[%d]: failed to parse", i);
  }

  for (int i = 0; i < n; i++) {
    MangoConfigProfile *pi = &set->profiles[i];
    for (size_t a = 0; a < pi->output_count; a++) {
      for (int j = i + 1; j < n; j++) {
        MangoConfigProfile *pj = &set->profiles[j];
        for (size_t b = 0; b < pj->output_count; b++) {
          if (strcmp(pi->output_names[a], pj->output_names[b]) == 0)
            return set_err(err, errsz,
                           "config[%d]: output '%s' also matched by config[%d]",
                           i, pi->output_names[a], j);
        }
      }
    }
  }
  return 0;
}

static void skip_json_ws(const char **cursor) {
  while (**cursor == ' ' || **cursor == '\t' || **cursor == '\n' ||
         **cursor == '\r')
    (*cursor)++;
}

int mango_config_set_parse(MangoConfigSet *set, const char *jsonc, char *err,
                           size_t errsz) {
  memset(set, 0, sizeof(*set));
  set->default_index = -1;
  if (err && errsz)
    err[0] = '\0';

  char *stripped = jsonc_strip(jsonc);
  if (!stripped)
    return set_err(err, errsz, "out of memory stripping JSONC comments");

  const char *end = NULL;
  cJSON *root = cJSON_ParseWithOpts(stripped, &end, 0);
  if (!root) {
    free(stripped);
    return set_err(err, errsz, "invalid JSON");
  }
  skip_json_ws(&end);

  int ret;
  if (cJSON_IsObject(root) && *end == ',') {
    cJSON_Delete(root);
    free(stripped);
    return set_err(err, errsz,
                   "comma-separated root objects are no longer supported; "
                   "wrap multiple profiles in a root array [ {...}, {...} ]");
  } else if (*end != '\0') {
    cJSON_Delete(root);
    free(stripped);
    return set_err(err, errsz, "invalid trailing content after config root");
  } else if (cJSON_IsArray(root)) {
    ret = parse_profiles(set, root, err, errsz);
  } else if (cJSON_IsObject(root)) {
    ret = parse_set_single(set, root, err, errsz);
  } else {
    ret = set_err(err, errsz,
                  "config root must be an object or an array of objects");
  }

  if (root)
    cJSON_Delete(root);
  free(stripped);
  if (ret != 0)
    mango_config_set_destroy(set);
  return ret;
}

int mango_config_set_load(MangoConfigSet *set, const char *path, char *err,
                          size_t errsz) {
  FILE *f = fopen(path, "r");
  if (!f)
    return set_err(err, errsz, "cannot open %s", path);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return set_err(err, errsz, "cannot read %s", path);
  }
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return set_err(err, errsz, "out of memory");
  }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  buf[rd] = '\0';
  fclose(f);
  int ret = mango_config_set_parse(set, buf, err, errsz);
  free(buf);
  return ret;
}

void mango_config_set_destroy(MangoConfigSet *set) {
  if (!set)
    return;
  for (size_t i = 0; i < set->count; i++)
    for (size_t j = 0; j < set->profiles[i].output_count; j++)
      free(set->profiles[i].output_names[j]);
  free(set->profiles);
  memset(set, 0, sizeof(*set));
  set->default_index = -1;
}

MangoConfigMatch mango_config_set_match(const MangoConfigSet *set,
                                        const char *output_name,
                                        MangoConfigProfile **profile) {
  if (!set || !output_name)
    return MANGO_CONFIG_NO_MATCH;
  for (size_t i = 0; i < set->count; i++) {
    MangoConfigProfile *p = &set->profiles[i];
    for (size_t j = 0; j < p->output_count; j++) {
      if (strcmp(p->output_names[j], output_name) == 0) {
        if (profile)
          *profile = p;
        return MANGO_CONFIG_MATCH_ENABLED;
      }
    }
  }
  if (set->default_index >= 0) {
    if (profile)
      *profile = &set->profiles[set->default_index];
    return MANGO_CONFIG_MATCH_ENABLED;
  }
  return MANGO_CONFIG_NO_MATCH;
}

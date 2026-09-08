#define _GNU_SOURCE
#include "tray.h"
#include "sd-bus.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WATCHER_PATH "/StatusNotifierWatcher"
#define DEFAULT_ICON_SIZE 64

typedef struct {
  int size;
  uint32_t pixels[]; // host-order ARGB (unpremultiplied)
} TrayPixmap;

struct MangobarTrayItem {
  char *watcher_id;
  char *service;
  char *path;
  const char *interface;
  char *status;
  char *icon_name;
  char *attention_icon_name;
  char *menu;
  char *icon_theme_path;
  TrayPixmap **pixmaps;
  int pixmap_count;
  TrayPixmap **attention_pixmaps;
  int attention_pixmap_count;
  bool item_is_menu;
  pixman_image_t *icon;
  int icon_size;
  sd_bus_slot **slots;
  int slot_count;
  struct MangobarTray *tray;
};

typedef struct {
  char *interface;
  sd_bus_slot *vtable_slot;
  sd_bus_slot *signal_slot;
  char **items;
  int item_count;
  char **hosts;
  int host_count;
  bool owns_name;
  struct MangobarTray *tray;
} TrayWatcher;

typedef struct {
  char *interface;
  char *service;
  sd_bus_slot *reg_slot;
  sd_bus_slot *unreg_slot;
  sd_bus_slot *watcher_slot;
  struct MangobarTray *tray;
} TrayHost;

struct MangobarTray {
  sd_bus *bus;
  int fd;
  TrayWatcher *watcher_xdg;
  TrayWatcher *watcher_kde;
  TrayHost *host_xdg;
  TrayHost *host_kde;
  MangobarTrayItem **items;
  int item_count;
  void (*set_dirty)(void);
  char **basedirs;
  int basedir_count;
  char *icon_theme; // GTK icon theme name
  sd_bus_slot *name_slot; // NameOwnerChanged subscription for cleanup
  bool dead;
};

// ---------- Utilities ----------
static char *fmt_str(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return strdup(buf);
}

static void set_tray_dirty(MangobarTray *tray) {
  if (tray && tray->set_dirty)
    tray->set_dirty();
}

static int find_index(char **arr, int n, const char *id) {
  for (int i = 0; i < n; i++)
    if (arr[i] && strcmp(arr[i], id) == 0)
      return i;
  return -1;
}

// ---------- GTK icon theme ----------
// Read gtk-icon-theme-name from a GTK settings.ini file.
static bool read_settings_ini_value(const char *path, char *out,
                                    size_t outsz) {
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  bool found = false;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0' || *p == '#' || *p == ';' || *p == '\n')
      continue; // empty line or comment
    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    char *key_end = eq;
    while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t'))
      key_end--;
    static const char key[] = "gtk-icon-theme-name";
    if ((size_t)(key_end - p) != sizeof(key) - 1 ||
        memcmp(p, key, sizeof(key) - 1) != 0)
      continue;
    char *v = eq + 1;
    while (*v == ' ' || *v == '\t')
      v++;
    char *end = v + strlen(v);
    while (end > v && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' ' || end[-1] == '\t'))
      end--;
    *end = '\0';
    // Strip optional surrounding quotes
    if (*v == '"' || *v == '\'') {
      char quote = *v;
      size_t l = strlen(v);
      if (l >= 2 && v[l - 1] == quote) {
        v++;
        v[l - 2] = '\0';
      }
    }
    size_t len = strlen(v);
    if (len > 0) {
      if (len >= outsz)
        len = outsz - 1;
      memcpy(out, v, len);
      out[len] = '\0';
      found = true;
      break;
    }
  }
  fclose(f);
  return found;
}

// Try $XDG_CONFIG_HOME/gtk-X.0/settings.ini, then /etc/gtk-X.0,
// with GTK3 before GTK4.
static char *gtk_icon_theme_name(void) {
  char cfgdir[1600];
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg && xdg[0] == '/') {
    snprintf(cfgdir, sizeof(cfgdir), "%s", xdg);
  } else {
    const char *home = getenv("HOME");
    if (!home || !*home)
      return NULL;
    snprintf(cfgdir, sizeof(cfgdir), "%s/.config", home);
  }
  char value[256];
  static const char *versions[] = {"3.0", "4.0"};
  for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
    char path[1800];
    snprintf(path, sizeof(path), "%s/gtk-%s/settings.ini", cfgdir,
             versions[i]);
    if (read_settings_ini_value(path, value, sizeof(value)))
      return strdup(value);
    snprintf(path, sizeof(path), "/etc/gtk-%s/settings.ini", versions[i]);
    if (read_settings_ini_value(path, value, sizeof(value)))
      return strdup(value);
  }
  return NULL;
}

static void init_icon_theme(MangobarTray *tray) {
  tray->icon_theme = gtk_icon_theme_name();
}

// ---------- Icon conversion ----------
static void image_free_data(pixman_image_t *img, void *data) {
  free(data);
}

static pixman_image_t *pixmap_to_pixman(const TrayPixmap *pm) {
  int s = pm->size;
  uint32_t *buf = malloc(sizeof(uint32_t) * (size_t)s * s);
  if (!buf)
    return NULL;
  for (int i = 0; i < s * s; i++) {
    uint32_t p = pm->pixels[i];
    uint32_t a = (p >> 24) & 0xff;
    uint32_t r = ((p >> 16) & 0xff) * a / 255;
    uint32_t g = ((p >> 8) & 0xff) * a / 255;
    uint32_t b = (p & 0xff) * a / 255;
    buf[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }
  pixman_image_t *img =
      pixman_image_create_bits(PIXMAN_a8r8g8b8, s, s, buf, s * 4);
  if (img)
    pixman_image_set_destroy_function(img, image_free_data, buf);
  else
    free(buf);
  return img;
}

static pixman_image_t *pixbuf_to_pixman(GdkPixbuf *pb) {
  int w = gdk_pixbuf_get_width(pb);
  int h = gdk_pixbuf_get_height(pb);
  GdkPixbuf *rgba = pb;
  bool need_unref = false;
  if (gdk_pixbuf_get_n_channels(pb) != 4 ||
      gdk_pixbuf_get_bits_per_sample(pb) != 8) {
    rgba = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    gdk_pixbuf_composite(pb, rgba, 0, 0, w, h, 0, 0, 1.0, 1.0,
                         GDK_INTERP_BILINEAR, 255);
    need_unref = true;
  } else if (!gdk_pixbuf_get_has_alpha(pb)) {
    rgba = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
    need_unref = true;
  }
  const guchar *px = gdk_pixbuf_get_pixels(rgba);
  int rowstride = gdk_pixbuf_get_rowstride(rgba);
  uint32_t *buf = malloc(sizeof(uint32_t) * (size_t)w * h);
  if (buf) {
    for (int y = 0; y < h; y++) {
      const guchar *row = px + y * rowstride;
      for (int x = 0; x < w; x++) {
        int r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2],
            a = row[x * 4 + 3];
        buf[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) |
                         ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
      }
    }
  }
  if (need_unref)
    g_object_unref(rgba);
  pixman_image_t *img =
      buf ? pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, buf, w * 4) : NULL;
  if (img)
    pixman_image_set_destroy_function(img, image_free_data, buf);
  else
    free(buf);
  return img;
}

static pixman_image_t *load_icon_from_file(const char *path) {
  GError *err = NULL;
  GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
  if (!pb) {
    if (err)
      g_error_free(err);
    return NULL;
  }
  pixman_image_t *img = pixbuf_to_pixman(pb);
  g_object_unref(pb);
  return img;
}

static pixman_image_t *make_placeholder(int size) {
  pixman_image_t *img =
      pixman_image_create_bits(PIXMAN_a8r8g8b8, size, size, NULL, size * 4);
  if (!img)
    return NULL;
  pixman_image_t *solid = pixman_image_create_solid_fill(
      &(pixman_color_t){0x6666, 0x6666, 0x6666, 0xFFFF});
  pixman_image_composite32(PIXMAN_OP_OVER, solid, NULL, img, 0, 0, 0, 0, 0, 0,
                           size, size);
  pixman_image_unref(solid);
  return img;
}

// ---------- Icon lookup (simplified theme search) ----------
#define MAX_ICON_CANDIDATES 32

typedef struct {
  char path[1600];
  int score; // higher is better: png + size proximity
} IconCandidate;

// Max entries scanned per lookup
#define MAX_ICON_LOOKUP_ENTRIES 200000

// Icon name -> path cache, avoids full theme scans on every NewIcon
#define ICON_CACHE_MAX 64
typedef struct {
  char name[128];
  char path[1600];
  bool found;
} IconCacheEntry;
static IconCacheEntry icon_cache[ICON_CACHE_MAX];
static int icon_cache_count;

// Build exact candidate names: original + -symbolic variant
static void icon_name_variants(const char *name,
                               char variants[][128], int *n) {
  *n = 0;
  snprintf(variants[(*n)++], 128, "%s", name);
  size_t l = strlen(name);
  if (l >= 9 && strcmp(name + l - 9, "-symbolic") == 0)
    snprintf(variants[(*n)++], 128, "%.*s", (int)(l - 9), name);
  else
    snprintf(variants[(*n)++], 128, "%s-symbolic", name);
}

static void find_icon_in_dir(const char *base, const char *name, int target,
                             int depth, IconCandidate *cands, int *count,
                             int *budget, int base_score,
                             const char variants[][128], int nvariants) {
  if (*budget <= 0)
    return;
  DIR *d = opendir(base);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) && *budget > 0) {
    (*budget)--;
    if (e->d_name[0] == '.')
      continue;
    if (*count >= MAX_ICON_CANDIDATES)
      break;
    char full[1600];
    snprintf(full, sizeof(full), "%.*s/%s", (int)(sizeof(full) - 300), base,
             e->d_name);
    bool is_dir;
    if (e->d_type == DT_DIR)
      is_dir = true;
    else if (e->d_type == DT_REG)
      is_dir = false;
    else {
      struct stat st;
      is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    }
    if (is_dir) {
      if (depth < 4)
        find_icon_in_dir(full, name, target, depth + 1, cands, count, budget,
                         base_score, variants, nvariants);
    } else {
      size_t dl = strlen(e->d_name);
      bool is_png = dl >= 4 && strcmp(e->d_name + dl - 4, ".png") == 0;
      bool is_svg = dl >= 4 && strcmp(e->d_name + dl - 4, ".svg") == 0;
      if (is_png || is_svg) {
        if (dl < 5)
          continue;
        char stem[128];
        size_t sl = dl - 4;
        if (sl >= sizeof(stem))
          continue;
        memcpy(stem, e->d_name, sl);
        stem[sl] = '\0';
        bool match = false;
        for (int v = 0; v < nvariants; v++)
          if (strcmp(stem, variants[v]) == 0) {
            match = true;
            break;
          }
        if (!match)
          continue;
        int score = base_score;
        if (is_png)
          score += 500; // prefer png at equal size
        int best_err = INT_MAX;
        for (const char *q = full; *q; q++) {
          if (*q >= '0' && *q <= '9') {
            int n = 0;
            while (*q >= '0' && *q <= '9') {
              n = n * 10 + (*q - '0');
              q++;
            }
            if (n > 0 && n <= 512) {
              int err = abs(n - target);
              if (err < best_err)
                best_err = err;
            }
          }
        }
        if (best_err != INT_MAX) {
          int bonus = 1000 - (best_err > 1000 ? 1000 : best_err);
          if (bonus > 0)
            score += bonus;
        }
        // Penalize symbolic icons
        if (strstr(stem, "-symbolic"))
          score -= 200;
        IconCandidate *c = &cands[(*count)++];
        snprintf(c->path, sizeof(c->path), "%s", full);
        c->score = score;
      }
    }
  }
  closedir(d);
}

// Whether this top-level theme dir was already searched by an earlier,
// higher-priority stage.
static bool theme_already_searched(const char *dir_name, const char *theme) {
  if (strcmp(dir_name, "hicolor") == 0 || strcmp(dir_name, "Adwaita") == 0)
    return true;
  return theme && strcmp(dir_name, theme) == 0;
}

// Search the icon theme directory with the given name under every basedir.
static void scan_named_theme(MangobarTray *tray, const char *dir_name,
                             const char *name, int target,
                             IconCandidate *cands, int *count, int *budget,
                             int pri, const char variants[][128],
                             int nvariants) {
  for (int i = 0; i < tray->basedir_count && *count < MAX_ICON_CANDIDATES &&
                     *budget > 0;
       i++) {
    DIR *rd = opendir(tray->basedirs[i]);
    if (!rd)
      continue;
    struct dirent *e;
    while ((e = readdir(rd)) && *count < MAX_ICON_CANDIDATES && *budget > 0) {
      (*budget)--;
      if (e->d_name[0] == '.' || strcmp(e->d_name, dir_name) != 0)
        continue;
      char p[1600];
      snprintf(p, sizeof(p), "%.*s/%s", (int)(sizeof(p) - 300),
               tray->basedirs[i], e->d_name);
      struct stat st;
      if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
        find_icon_in_dir(p, name, target, 0, cands, count, budget, pri,
                         variants, nvariants);
      break; // a theme directory appears at most once per basedir
    }
    closedir(rd);
  }
}

static char *find_icon(MangobarTray *tray, const char *name,
                       const char *extra, int target) {
  if (!name || !*name)
    return NULL;
  // Return cached result
  for (int i = 0; i < icon_cache_count; i++) {
    if (strcmp(icon_cache[i].name, name) == 0)
      return icon_cache[i].found ? strdup(icon_cache[i].path) : NULL;
  }
  // IconName may be an absolute path
  if (name[0] == '/') {
    struct stat st;
    if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
      if (icon_cache_count < ICON_CACHE_MAX) {
        IconCacheEntry *c = &icon_cache[icon_cache_count++];
        snprintf(c->name, sizeof(c->name), "%s", name);
        snprintf(c->path, sizeof(c->path), "%s", name);
        c->found = true;
      }
      return strdup(name);
    }
    return NULL;
  }
  char variants[2][128];
  int nvariants;
  icon_name_variants(name, variants, &nvariants);
  IconCandidate cands[MAX_ICON_CANDIDATES];
  int count = 0;
  int budget = MAX_ICON_LOOKUP_ENTRIES;
  const char *theme = tray->icon_theme;
  // App-provided IconThemePath overrides the configured theme.
  if (extra && *extra && count < MAX_ICON_CANDIDATES)
    find_icon_in_dir(extra, name, target, 0, cands, &count, &budget, 50000,
                     variants, nvariants);
  // Search preferred themes first, and only continue when none has the icon.
  if (count == 0 && theme && *theme)
    scan_named_theme(tray, theme, name, target, cands, &count, &budget, 40000,
                     variants, nvariants);
  if (count == 0 && (!theme || strcmp(theme, "hicolor") != 0))
    scan_named_theme(tray, "hicolor", name, target, cands, &count, &budget,
                     30000, variants, nvariants);
  if (count == 0 && (!theme || strcmp(theme, "Adwaita") != 0))
    scan_named_theme(tray, "Adwaita", name, target, cands, &count, &budget,
                     20000, variants, nvariants);
  if (count == 0) {
    // Search remaining themes and loose icon files.
    for (int i = 0; i < tray->basedir_count && count < MAX_ICON_CANDIDATES;
         i++) {
      DIR *rd = opendir(tray->basedirs[i]);
      if (!rd) {
        find_icon_in_dir(tray->basedirs[i], name, target, 0, cands, &count,
                         &budget, 10000, variants, nvariants);
        continue;
      }
      struct dirent *e;
      while ((e = readdir(rd)) && count < MAX_ICON_CANDIDATES && budget > 0) {
        budget--;
        if (e->d_name[0] == '.')
          continue;
        char p[1600];
        snprintf(p, sizeof(p), "%.*s/%s", (int)(sizeof(p) - 300),
                 tray->basedirs[i], e->d_name);
        struct stat st;
        if (stat(p, &st) != 0)
          continue;
        if (S_ISDIR(st.st_mode)) {
          if (theme_already_searched(e->d_name, theme))
            continue;
          find_icon_in_dir(p, name, target, 0, cands, &count, &budget, 10000,
                           variants, nvariants);
        } else {
          // Loose files at the basedir root (e.g. /usr/share/pixmaps)
          size_t dl = strlen(e->d_name);
          bool is_png = dl >= 4 && strcmp(e->d_name + dl - 4, ".png") == 0;
          bool is_svg = dl >= 4 && strcmp(e->d_name + dl - 4, ".svg") == 0;
          if (!is_png && !is_svg)
            continue;
          if (dl < 5)
            continue;
          char stem[128];
          size_t sl = dl - 4;
          if (sl >= sizeof(stem))
            continue;
          memcpy(stem, e->d_name, sl);
          stem[sl] = '\0';
          bool match = false;
          for (int v = 0; v < nvariants; v++)
            if (strcmp(stem, variants[v]) == 0) {
              match = true;
              break;
            }
          if (match && count < MAX_ICON_CANDIDATES) {
            IconCandidate *c = &cands[count++];
            snprintf(c->path, sizeof(c->path), "%s", p);
            c->score = 10000;
          }
        }
      }
      closedir(rd);
    }
  }
  if (count == 0) {
    // Cache misses too
    if (icon_cache_count < ICON_CACHE_MAX) {
      IconCacheEntry *c = &icon_cache[icon_cache_count++];
      snprintf(c->name, sizeof(c->name), "%s", name);
      c->path[0] = '\0';
      c->found = false;
    }
    return NULL;
  }
  int best = 0;
  for (int i = 1; i < count; i++)
    if (cands[i].score > cands[best].score)
      best = i;
  if (icon_cache_count < ICON_CACHE_MAX) {
    IconCacheEntry *c = &icon_cache[icon_cache_count++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    size_t len = strlen(cands[best].path);
    if (len >= sizeof(c->path))
      len = sizeof(c->path) - 1;
    memcpy(c->path, cands[best].path, len);
    c->path[len] = '\0';
    c->found = true;
  }
  return strdup(cands[best].path);
}

static void init_basedirs(MangobarTray *tray) {
  const char *home = getenv("HOME");
  const char *xdg_data = getenv("XDG_DATA_HOME");
  const char *xdg_dirs = getenv("XDG_DATA_DIRS");
  char *candidates[16] = {0};
  int nc = 0;
  if (home && *home) {
    candidates[nc++] = fmt_str("%s/.icons", home);
    if (xdg_data && *xdg_data)
      candidates[nc++] = fmt_str("%s/icons", xdg_data);
    else
      candidates[nc++] = fmt_str("%s/.local/share/icons", home);
  }
  candidates[nc++] = strdup("/usr/share/pixmaps");
  if (!(xdg_dirs && *xdg_dirs))
    xdg_dirs = "/usr/local/share:/usr/share";
  {
    char *copy = strdup(xdg_dirs);
    char *tok = strtok(copy, ":");
    while (tok && nc < 16) {
      candidates[nc++] = fmt_str("%s/icons", tok);
      tok = strtok(NULL, ":");
    }
    free(copy);
  }
  for (int i = 0; i < nc; i++) {
    struct stat st;
    if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode)) {
      tray->basedirs = realloc(tray->basedirs,
                               sizeof(char *) * (tray->basedir_count + 1));
      tray->basedirs[tray->basedir_count++] = candidates[i];
    } else {
      free(candidates[i]);
    }
  }
}

// ---------- Items ----------
static void pixmaps_clear(TrayPixmap ***arr, int *count) {
  if (!*arr)
    return;
  for (int i = 0; i < *count; i++)
    free((*arr)[i]);
  free(*arr);
  *arr = NULL;
  *count = 0;
}

static void destroy_sni(MangobarTrayItem *sni) {
  if (!sni)
    return;
  if (sni->icon)
    pixman_image_unref(sni->icon);
  free(sni->watcher_id);
  free(sni->service);
  free(sni->path);
  free(sni->status);
  free(sni->icon_name);
  free(sni->attention_icon_name);
  free(sni->menu);
  free(sni->icon_theme_path);
  pixmaps_clear(&sni->pixmaps, &sni->pixmap_count);
  pixmaps_clear(&sni->attention_pixmaps, &sni->attention_pixmap_count);
  for (int i = 0; i < sni->slot_count; i++)
    sd_bus_slot_unref(sni->slots[i]);
  free(sni->slots);
  free(sni);
}

static bool sni_ready(MangobarTrayItem *sni) {
  if (!sni->status)
    return false;
  if (sni->status[0] == 'N')
    return sni->attention_icon_name || sni->attention_pixmap_count > 0;
  return sni->icon_name || sni->pixmap_count > 0;
}

static void reload_icon(MangobarTrayItem *sni) {
  pixman_image_t *old = sni->icon;
  sni->icon = NULL;
  sni->icon_size = 0;
  bool attention = sni->status && sni->status[0] == 'N';
  const char *icon_name = attention ? sni->attention_icon_name : sni->icon_name;
  TrayPixmap **pixmaps = attention ? sni->attention_pixmaps : sni->pixmaps;
  int pixmap_count = attention ? sni->attention_pixmap_count
                               : sni->pixmap_count;
  if (pixmap_count > 0) {
    TrayPixmap *best = pixmaps[0];
    // Pick the largest pixmap so downscaling stays sharp.
    for (int i = 1; i < pixmap_count; i++)
      if (pixmaps[i]->size > best->size)
        best = pixmaps[i];
    sni->icon = pixmap_to_pixman(best);
    sni->icon_size = best->size;
  } else if (icon_name && *icon_name) {
    char *path = find_icon(sni->tray, icon_name, sni->icon_theme_path,
                           DEFAULT_ICON_SIZE);
    if (path) {
      sni->icon = load_icon_from_file(path);
      sni->icon_size = sni->icon ? pixman_image_get_width(sni->icon) : 0;
      free(path);
    }
  }
  if (!sni->icon) {
    sni->icon = make_placeholder(DEFAULT_ICON_SIZE);
    sni->icon_size = DEFAULT_ICON_SIZE;
  }
  if (old)
    pixman_image_unref(old);
}

static void sni_dirty(MangobarTrayItem *sni) {
  if (sni_ready(sni)) {
    reload_icon(sni);
    set_tray_dirty(sni->tray);
  }
}

// ---------- Async property reads ----------
typedef struct {
  MangobarTrayItem *sni;
  const char *prop;
  const char *type; // NULL = icon pixmap
  void *dest;
} PropSlot;

static int read_pixmap(sd_bus_message *msg, MangobarTrayItem *sni,
                       bool attention) {
  int ret = sd_bus_message_enter_container(msg, 'a', "(iiay)");
  if (ret < 0)
    return ret;
  if (sd_bus_message_at_end(msg, 0))
    return 0;
  TrayPixmap **pixmaps = NULL;
  int n = 0;
  while (!sd_bus_message_at_end(msg, 0)) {
    ret = sd_bus_message_enter_container(msg, 'r', "iiay");
    if (ret < 0)
      goto err;
    int width, height;
    ret = sd_bus_message_read(msg, "ii", &width, &height);
    if (ret < 0) {
      sd_bus_message_exit_container(msg);
      goto err;
    }
    const void *pixels;
    size_t size;
    ret = sd_bus_message_read_array(msg, 'y', &pixels, &size);
    if (ret < 0) {
      sd_bus_message_exit_container(msg);
      goto err;
    }
    sd_bus_message_exit_container(msg);
    if (height > 0 && width == height && (size_t)width * height <= size / 4) {
      TrayPixmap *pm = malloc(sizeof(TrayPixmap) +
                              sizeof(uint32_t) * (size_t)width * height);
      if (!pm)
        continue;
      pm->size = height;
      const uint32_t *src = pixels;
      for (int i = 0; i < width * height; i++)
        pm->pixels[i] = ntohl(src[i]);
      pixmaps = realloc(pixmaps, sizeof(*pixmaps) * (n + 1));
      if (!pixmaps)
        break;
      pixmaps[n++] = pm;
    }
  }
  if (attention) {
    pixmaps_clear(&sni->attention_pixmaps, &sni->attention_pixmap_count);
    sni->attention_pixmaps = pixmaps;
    sni->attention_pixmap_count = n;
  } else {
    pixmaps_clear(&sni->pixmaps, &sni->pixmap_count);
    sni->pixmaps = pixmaps;
    sni->pixmap_count = n;
  }
  return 0;
err:
  for (int i = 0; i < n; i++)
    free(pixmaps[i]);
  free(pixmaps);
  return ret;
}

static int get_property_callback(sd_bus_message *msg, void *data,
                                 sd_bus_error *error) {
  PropSlot *d = data;
  MangobarTrayItem *sni = d->sni;
  const char *prop = d->prop;
  const char *type = d->type;
  void *dest = d->dest;
  int ret;
  if (sd_bus_message_is_method_error(msg, NULL)) {
    ret = sd_bus_message_get_errno(msg);
    goto cleanup;
  }
  ret = sd_bus_message_enter_container(msg, 'v', type);
  if (ret < 0)
    goto cleanup;
  if (!type) {
    bool attention = strncmp(prop, "Attention", 9) == 0;
    ret = read_pixmap(msg, sni, attention);
    if (ret < 0)
      goto cleanup;
  } else {
    if ((*type == 's' || *type == 'o') && *(char **)dest)
      free(*(char **)dest);
    ret = sd_bus_message_read(msg, type, dest);
    if (ret < 0)
      goto cleanup;
    if (*type == 's' || *type == 'o')
      *(char **)dest = strdup(*(char **)dest);
  }
  if (strcmp(prop, "Status") == 0 ||
      (sni->status && (sni->status[0] == 'N' ? prop[0] == 'A'
                                             : strncmp(prop, "Icon", 4) == 0)))
    sni_dirty(sni);
cleanup:
  free(d);
  return ret;
}

static void sni_get_property_async(MangobarTrayItem *sni, const char *prop,
                                   const char *type, void *dest) {
  PropSlot *data = calloc(1, sizeof(*data));
  data->sni = sni;
  data->prop = prop;
  data->type = type;
  data->dest = dest;
  int ret = sd_bus_call_method_async(
      sni->tray->bus, NULL, sni->service, sni->path,
      "org.freedesktop.DBus.Properties", "Get", get_property_callback, data,
      "ss", sni->interface, prop);
  if (ret < 0)
    free(data);
}

// ---------- Item signals ----------
static int handle_new_icon(sd_bus_message *msg, void *data,
                           sd_bus_error *error) {
  MangobarTrayItem *sni = data;
  sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
  sni_get_property_async(sni, "IconPixmap", NULL, NULL);
  return 0;
}

static int handle_new_attention_icon(sd_bus_message *msg, void *data,
                                     sd_bus_error *error) {
  MangobarTrayItem *sni = data;
  sni_get_property_async(sni, "AttentionIconName", "s",
                         &sni->attention_icon_name);
  sni_get_property_async(sni, "AttentionIconPixmap", NULL, NULL);
  return 0;
}

static int handle_new_status(sd_bus_message *msg, void *data,
                             sd_bus_error *error) {
  MangobarTrayItem *sni = data;
  char *status;
  int r = sd_bus_message_read(msg, "s", &status);
  if (r < 0)
    return r;
  free(sni->status);
  sni->status = strdup(status);
  sni_dirty(sni);
  return 0;
}

static void sni_match_signal_async(MangobarTrayItem *sni, const char *signal,
                                   sd_bus_message_handler_t callback) {
  sd_bus_slot *slot = NULL;
  int ret = sd_bus_match_signal_async(sni->tray->bus, &slot, sni->service,
                                      sni->path, sni->interface, signal,
                                      callback, NULL, sni);
  if (ret >= 0) {
    sni->slots = realloc(sni->slots, sizeof(*sni->slots) * (sni->slot_count + 1));
    sni->slots[sni->slot_count++] = slot;
  }
}

static MangobarTrayItem *create_sni(char *id, MangobarTray *tray) {
  MangobarTrayItem *sni = calloc(1, sizeof(*sni));
  if (!sni)
    return NULL;
  sni->tray = tray;
  sni->watcher_id = strdup(id);
  char *path_ptr = strchr(id, '/');
  if (!path_ptr) {
    sni->service = strdup(id);
    sni->path = strdup("/StatusNotifierItem");
    // SNIs may register only a service name (no path); use the org.kde interface
    sni->interface = "org.kde.StatusNotifierItem";
  } else {
    sni->service = strndup(id, (size_t)(path_ptr - id));
    sni->path = strdup(path_ptr);
    sni->interface = "org.kde.StatusNotifierItem";
    sni_get_property_async(sni, "IconThemePath", "s", &sni->icon_theme_path);
  }
  sni_get_property_async(sni, "Status", "s", &sni->status);
  sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
  sni_get_property_async(sni, "IconPixmap", NULL, NULL);
  sni_get_property_async(sni, "AttentionIconName", "s",
                         &sni->attention_icon_name);
  sni_get_property_async(sni, "AttentionIconPixmap", NULL, NULL);
  sni_get_property_async(sni, "ItemIsMenu", "b", &sni->item_is_menu);
  sni_get_property_async(sni, "Menu", "o", &sni->menu);

  sni_match_signal_async(sni, "NewIcon", handle_new_icon);
  sni_match_signal_async(sni, "NewAttentionIcon", handle_new_attention_icon);
  sni_match_signal_async(sni, "NewStatus", handle_new_status);
  return sni;
}

static void add_sni(MangobarTray *tray, char *id) {
  for (int i = 0; i < tray->item_count; i++)
    if (strcmp(tray->items[i]->watcher_id, id) == 0)
      return;
  MangobarTrayItem *sni = create_sni(id, tray);
  if (sni) {
    tray->items = realloc(tray->items,
                          sizeof(*tray->items) * (tray->item_count + 1));
    tray->items[tray->item_count++] = sni;
    set_tray_dirty(tray);
  }
}

static void remove_sni(MangobarTray *tray, const char *id) {
  for (int i = 0; i < tray->item_count; i++) {
    if (strcmp(tray->items[i]->watcher_id, id) == 0) {
      destroy_sni(tray->items[i]);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      return;
    }
  }
}

// Remove stale items whose service (unique or well-known) lost its owner
static void remove_sni_by_service(MangobarTray *tray, const char *service) {
  for (int i = 0; i < tray->item_count; i++) {
    if (tray->items[i]->service &&
        strcmp(tray->items[i]->service, service) == 0) {
      destroy_sni(tray->items[i]);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      i--;
    }
  }
}

static int handle_name_owner_changed(sd_bus_message *msg, void *data,
                                     sd_bus_error *error) {
  char *name = NULL, *old_owner = NULL, *new_owner = NULL;
  int ret = sd_bus_message_read(msg, "sss", &name, &old_owner, &new_owner);
  if (ret < 0)
    return ret;
  if (!*new_owner && name)
    remove_sni_by_service((MangobarTray *)data, name);
  return 0;
}

// ---------- Host ----------
static int handle_sni_registered(sd_bus_message *msg, void *data,
                                 sd_bus_error *error) {
  char *id;
  int ret = sd_bus_message_read(msg, "s", &id);
  if (ret < 0)
    return ret;
  add_sni((MangobarTray *)data, id);
  return 0;
}

static int handle_sni_unregistered(sd_bus_message *msg, void *data,
                                   sd_bus_error *error) {
  char *id;
  int ret = sd_bus_message_read(msg, "s", &id);
  if (ret < 0)
    return ret;
  remove_sni((MangobarTray *)data, id);
  return 0;
}

static int get_registered_snis_callback(sd_bus_message *msg, void *data,
                                        sd_bus_error *error) {
  if (sd_bus_message_is_method_error(msg, NULL))
    return 0;
  MangobarTray *tray = data;
  int ret = sd_bus_message_enter_container(msg, 'v', NULL);
  if (ret < 0)
    return ret;
  char **ids;
  ret = sd_bus_message_read_strv(msg, &ids);
  if (ret < 0)
    return ret;
  if (ids) {
    for (char **id = ids; *id; ++id) {
      add_sni(tray, *id);
      free(*id);
    }
    free(ids);
  }
  return ret;
}

static void register_to_watcher(TrayHost *host) {
  if (!host->tray->dead) {
    sd_bus_call_method_async(
        host->tray->bus, NULL, host->interface, WATCHER_PATH,
        host->interface, "RegisterStatusNotifierHost", NULL, NULL, "s",
        host->service);
    sd_bus_call_method_async(host->tray->bus, NULL, host->interface,
                             WATCHER_PATH, "org.freedesktop.DBus.Properties",
                             "Get", get_registered_snis_callback,
                             host->tray, "ss", host->interface,
                             "RegisteredStatusNotifierItems");
  }
}

static int handle_new_watcher(sd_bus_message *msg, void *data,
                              sd_bus_error *error) {
  char *service, *old_owner, *new_owner;
  int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
  if (ret < 0)
    return ret;
  if (!*old_owner) {
    TrayHost *host = data;
    if (strcmp(service, host->interface) == 0)
      register_to_watcher(host);
  }
  return 0;
}

static void init_host(MangobarTray *tray, const char *protocol,
                      TrayHost **out) {
  TrayHost *host = calloc(1, sizeof(*host));
  if (!host)
    return;
  host->interface = fmt_str("org.%s.StatusNotifierWatcher", protocol);
  host->tray = tray;
  if (sd_bus_match_signal(tray->bus, &host->reg_slot, host->interface,
                          WATCHER_PATH, host->interface,
                          "StatusNotifierItemRegistered", handle_sni_registered,
                          tray) < 0)
    goto err;
  if (sd_bus_match_signal(tray->bus, &host->unreg_slot, host->interface,
                          WATCHER_PATH, host->interface,
                          "StatusNotifierItemUnregistered",
                          handle_sni_unregistered, tray) < 0)
    goto err;
  if (sd_bus_match_signal(tray->bus, &host->watcher_slot,
                          "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "NameOwnerChanged",
                          handle_new_watcher, host) < 0)
    goto err;
  host->service =
      fmt_str("org.%s.StatusNotifierHost-%d", protocol, (int)getpid());
  sd_bus_request_name(tray->bus, host->service, 0);
  register_to_watcher(host);
  *out = host;
  return;
err:
  if (host->reg_slot)
    sd_bus_slot_unref(host->reg_slot);
  if (host->unreg_slot)
    sd_bus_slot_unref(host->unreg_slot);
  if (host->watcher_slot)
    sd_bus_slot_unref(host->watcher_slot);
  free(host->interface);
  free(host->service);
  free(host);
}

// ---------- Watcher (optionally owns the watcher name) ----------
static int watcher_register_sni(sd_bus_message *msg, void *data,
                                sd_bus_error *error) {
  TrayWatcher *watcher = data;
  char *service_or_path;
  int ret = sd_bus_message_read(msg, "s", &service_or_path);
  if (ret < 0)
    return ret;
  char *id;
  if (service_or_path[0] == '/') {
    const char *sender = sd_bus_message_get_sender(msg);
    id = fmt_str("%s%s", sender, service_or_path);
  } else {
    id = strdup(service_or_path);
  }
  if (find_index(watcher->items, watcher->item_count, id) == -1) {
    watcher->items =
        realloc(watcher->items, sizeof(char *) * (watcher->item_count + 1));
    watcher->items[watcher->item_count++] = id;
    // When we are the watcher, our own signals don't loop back; add directly
    add_sni(watcher->tray, id);
    sd_bus_emit_signal(watcher->tray->bus, WATCHER_PATH, watcher->interface,
                       "StatusNotifierItemRegistered", "s", id);
    sd_bus_emit_properties_changed(watcher->tray->bus, WATCHER_PATH,
                                   watcher->interface,
                                   "RegisteredStatusNotifierItems", NULL);
  } else {
    free(id);
  }
  return sd_bus_reply_method_return(msg, "");
}

static int watcher_register_host(sd_bus_message *msg, void *data,
                                 sd_bus_error *error) {
  TrayWatcher *watcher = data;
  char *service;
  int ret = sd_bus_message_read(msg, "s", &service);
  if (ret < 0)
    return ret;
  if (find_index(watcher->hosts, watcher->host_count, service) == -1) {
    watcher->hosts =
        realloc(watcher->hosts, sizeof(char *) * (watcher->host_count + 1));
    watcher->hosts[watcher->host_count++] = strdup(service);
  }
  return sd_bus_reply_method_return(msg, "");
}

static int watcher_get_registered_snis(sd_bus *bus, const char *obj_path,
                                       const char *interface,
                                       const char *property,
                                       sd_bus_message *reply, void *data,
                                       sd_bus_error *error) {
  TrayWatcher *watcher = data;
  watcher->items =
      realloc(watcher->items, sizeof(char *) * (watcher->item_count + 1));
  watcher->items[watcher->item_count] = NULL;
  int ret = sd_bus_message_append_strv(reply, watcher->items);
  free(watcher->items[watcher->item_count]);
  return ret;
}

static int watcher_is_host_registered(sd_bus *bus, const char *obj_path,
                                      const char *interface,
                                      const char *property,
                                      sd_bus_message *reply, void *data,
                                      sd_bus_error *error) {
  TrayWatcher *watcher = data;
  int val = watcher->host_count > 0;
  return sd_bus_message_append_basic(reply, 'b', &val);
}

static int watcher_get_protocol_version(sd_bus *bus, const char *obj_path,
                                        const char *interface,
                                        const char *property,
                                        sd_bus_message *reply, void *data,
                                        sd_bus_error *error) {
  int val = 0;
  return sd_bus_message_append_basic(reply, 'i', &val);
}

static int watcher_noop(sd_bus_message *msg, void *data, sd_bus_error *error) {
  return 0;
}

static const sd_bus_vtable watcher_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", watcher_register_sni,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", watcher_register_host,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as",
                    watcher_get_registered_snis, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b",
                    watcher_is_host_registered, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ProtocolVersion", "i", watcher_get_protocol_version, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", NULL, 0),
    SD_BUS_VTABLE_END};

static TrayWatcher *create_watcher(MangobarTray *tray, const char *protocol) {
  TrayWatcher *w = calloc(1, sizeof(*w));
  if (!w)
    return NULL;
  w->interface = fmt_str("org.%s.StatusNotifierWatcher", protocol);
  w->tray = tray;
  if (sd_bus_add_object_vtable(tray->bus, &w->vtable_slot, WATCHER_PATH,
                               w->interface, watcher_vtable, w) < 0)
    goto err;
  if (sd_bus_match_signal(tray->bus, &w->signal_slot, "org.freedesktop.DBus",
                          "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "NameOwnerChanged", watcher_noop, w) < 0)
    goto err;
  if (sd_bus_request_name(tray->bus, w->interface, 0) < 0)
    goto err;
  w->owns_name = true;
  return w;
err:
  if (w->vtable_slot)
    sd_bus_slot_unref(w->vtable_slot);
  if (w->signal_slot)
    sd_bus_slot_unref(w->signal_slot);
  free(w->interface);
  free(w);
  return NULL;
}

// ---------- Public API ----------
MangobarTray *tray_init(void (*set_dirty)(void)) {
  sd_bus *bus;
  if (sd_bus_open_user(&bus) < 0)
    return NULL;
  MangobarTray *tray = calloc(1, sizeof(*tray));
  if (!tray) {
    sd_bus_unref(bus);
    return NULL;
  }
  tray->bus = bus;
  tray->fd = sd_bus_get_fd(bus);
  tray->set_dirty = set_dirty;
  init_basedirs(tray);
  init_icon_theme(tray);
  tray->watcher_xdg = create_watcher(tray, "freedesktop");
  tray->watcher_kde = create_watcher(tray, "kde");
  init_host(tray, "freedesktop", &tray->host_xdg);
  init_host(tray, "kde", &tray->host_kde);
  // Clean up stale tray items when services die
  sd_bus_match_signal(bus, &tray->name_slot, "org.freedesktop.DBus",
                      "/org/freedesktop/DBus", "org.freedesktop.DBus",
                      "NameOwnerChanged", handle_name_owner_changed, tray);
  sd_bus_process(bus, NULL);
  return tray;
}

void tray_destroy(MangobarTray *tray) {
  if (!tray)
    return;
  if (tray->name_slot)
    sd_bus_slot_unref(tray->name_slot);
  if (tray->host_xdg) {
    if (tray->host_xdg->service)
      sd_bus_release_name(tray->bus, tray->host_xdg->service);
    if (tray->host_xdg->reg_slot)
      sd_bus_slot_unref(tray->host_xdg->reg_slot);
    if (tray->host_xdg->unreg_slot)
      sd_bus_slot_unref(tray->host_xdg->unreg_slot);
    if (tray->host_xdg->watcher_slot)
      sd_bus_slot_unref(tray->host_xdg->watcher_slot);
    free(tray->host_xdg->interface);
    free(tray->host_xdg->service);
    free(tray->host_xdg);
  }
  if (tray->host_kde) {
    if (tray->host_kde->service)
      sd_bus_release_name(tray->bus, tray->host_kde->service);
    if (tray->host_kde->reg_slot)
      sd_bus_slot_unref(tray->host_kde->reg_slot);
    if (tray->host_kde->unreg_slot)
      sd_bus_slot_unref(tray->host_kde->unreg_slot);
    if (tray->host_kde->watcher_slot)
      sd_bus_slot_unref(tray->host_kde->watcher_slot);
    free(tray->host_kde->interface);
    free(tray->host_kde->service);
    free(tray->host_kde);
  }
  if (tray->watcher_xdg) {
    sd_bus_release_name(tray->bus, tray->watcher_xdg->interface);
    sd_bus_slot_unref(tray->watcher_xdg->vtable_slot);
    sd_bus_slot_unref(tray->watcher_xdg->signal_slot);
    for (int i = 0; i < tray->watcher_xdg->item_count; i++)
      free(tray->watcher_xdg->items[i]);
    free(tray->watcher_xdg->items);
    for (int i = 0; i < tray->watcher_xdg->host_count; i++)
      free(tray->watcher_xdg->hosts[i]);
    free(tray->watcher_xdg->hosts);
    free(tray->watcher_xdg->interface);
    free(tray->watcher_xdg);
  }
  if (tray->watcher_kde) {
    sd_bus_release_name(tray->bus, tray->watcher_kde->interface);
    sd_bus_slot_unref(tray->watcher_kde->vtable_slot);
    sd_bus_slot_unref(tray->watcher_kde->signal_slot);
    for (int i = 0; i < tray->watcher_kde->item_count; i++)
      free(tray->watcher_kde->items[i]);
    free(tray->watcher_kde->items);
    for (int i = 0; i < tray->watcher_kde->host_count; i++)
      free(tray->watcher_kde->hosts[i]);
    free(tray->watcher_kde->hosts);
    free(tray->watcher_kde->interface);
    free(tray->watcher_kde);
  }
  for (int i = 0; i < tray->item_count; i++)
    destroy_sni(tray->items[i]);
  free(tray->items);
  for (int i = 0; i < tray->basedir_count; i++)
    free(tray->basedirs[i]);
  free(tray->basedirs);
  free(tray->icon_theme);
  sd_bus_flush_close_unref(tray->bus);
  free(tray);
}

int tray_get_fd(MangobarTray *tray) { return tray ? tray->fd : -1; }

int tray_get_events(MangobarTray *tray) {
  if (!tray || !tray->bus)
    return POLLIN;
  int ev = sd_bus_get_events(tray->bus);
  return ev >= 0 ? ev : POLLIN;
}

void tray_dispatch(MangobarTray *tray) {
  if (!tray || tray->dead)
    return;
  int ret;
  while ((ret = sd_bus_process(tray->bus, NULL)) > 0) {
  }
  // Flush outgoing messages (e.g. menu Event clicks) promptly
  sd_bus_flush(tray->bus);
  if (ret < 0)
    tray->dead = true;
}

MangobarTrayItem **tray_visible_items(MangobarTray *tray, int *count) {
  *count = 0;
  if (!tray)
    return NULL;
  static MangobarTrayItem *out[MANGOBAR_TRAY_MAX_ITEMS];
  for (int i = 0; i < tray->item_count && *count < MANGOBAR_TRAY_MAX_ITEMS;
       i++) {
    MangobarTrayItem *it = tray->items[i];
    if (!it->status || it->status[0] == 'P' || !it->icon)
      continue;
    out[(*count)++] = it;
  }
  return out;
}

const char *tray_item_id(MangobarTrayItem *item) {
  return item ? item->watcher_id : NULL;
}
pixman_image_t *tray_item_icon(MangobarTrayItem *item) {
  return item ? item->icon : NULL;
}
int tray_item_icon_size(MangobarTrayItem *item) {
  return item ? item->icon_size : 0;
}
void tray_remove_item(MangobarTray *tray, MangobarTrayItem *item) {
  if (!tray || !item)
    return;
  for (int i = 0; i < tray->item_count; i++) {
    if (tray->items[i] == item) {
      destroy_sni(item);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      return;
    }
  }
}
void tray_prune(MangobarTray *tray) {
  if (!tray || tray->dead)
    return;
  for (int i = 0; i < tray->item_count; i++) {
    MangobarTrayItem *it = tray->items[i];
    if (!it->service)
      continue;
    // Check whether the service name is still owned via GetNameOwner
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(
        tray->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", &err, &reply, "s", it->service);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    if (r < 0) { // no owner -> stale, remove
      destroy_sni(it);
      memmove(&tray->items[i], &tray->items[i + 1],
              sizeof(*tray->items) * (tray->item_count - i - 1));
      tray->item_count--;
      set_tray_dirty(tray);
      i--;
    }
  }
}

void tray_refresh(MangobarTray *tray) {
  if (!tray || tray->dead)
    return;
  tray_prune(tray);
  if (tray->host_xdg)
    register_to_watcher(tray->host_xdg);
  if (tray->host_kde)
    register_to_watcher(tray->host_kde);
  sd_bus_flush(tray->bus);
}
bool tray_item_has_menu(MangobarTrayItem *item) {
  return item && item->menu && *item->menu;
}
const char *tray_item_service(MangobarTrayItem *item) {
  return item ? item->service : NULL;
}
const char *tray_item_menu_path(MangobarTrayItem *item) {
  return item ? item->menu : NULL;
}
void *tray_get_bus(MangobarTray *tray) { return tray ? tray->bus : NULL; }

static uint32_t event_to_x11_button(uint32_t button) {
  switch (button) {
  case 0x110: // BTN_LEFT
    return 1;
  case 0x112: // BTN_MIDDLE
    return 2;
  case 0x111: // BTN_RIGHT
    return 3;
  case 4:
  case 5:
  case 6:
  case 7:
    return button;
  case 0x113: // BTN_SIDE
    return 8;
  case 0x114: // BTN_EXTRA
    return 9;
  default:
    return 0;
  }
}

void tray_handle_click(MangobarTray *tray, MangobarTrayItem *sni, double x,
                       double y, uint32_t button) {
  if (!tray || tray->dead || !sni)
    return;
  static const char *defaults[10] = {NULL,          "Activate",
                                     "SecondaryActivate", "ContextMenu",
                                     "ScrollUp",    "ScrollDown",
                                     "ScrollLeft",  "ScrollRight",
                                     NULL,          NULL};
  uint32_t idx = event_to_x11_button(button);
  if (idx == 0 || idx > 9)
    return;
  const char *method = defaults[idx];
  if (!method)
    return;
  if (sni->item_is_menu && strcmp(method, "Activate") == 0)
    method = "ContextMenu";
  if (strncmp(method, "Scroll", 6) == 0) {
    char dir = method[6];
    const char *orientation =
        (dir == 'U' || dir == 'D') ? "vertical" : "horizontal";
    int sign = (dir == 'U' || dir == 'L') ? -1 : 1;
    sd_bus_call_method_async(tray->bus, NULL, sni->service, sni->path,
                             sni->interface, "Scroll", NULL, NULL, "is", sign,
                             orientation);
  } else {
    sd_bus_call_method_async(tray->bus, NULL, sni->service, sni->path,
                             sni->interface, method, NULL, NULL, "ii", (int)x,
                             (int)y);
  }
}

#ifndef MANGOBAR_RTCONFIG_H
#define MANGOBAR_RTCONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MANGOBAR_MAX_TAGS 31
#define MANGOBAR_MAX_ACTIONS 32
#define MANGOBAR_MAX_MODULES 16
#define MANGOBAR_MAX_CUSTOM 16
#define MANGOBAR_MAX_ALTS 48
#define MANGOBAR_MAX_ICONS 12
#define MANGOBAR_MAX_LENS 32

enum MangoModule {
  M_NONE = 0,
  M_TAGS,
  M_LAYOUT,
  M_TITLE,
  M_TRAY,
  M_CPU,
  M_MEM,
  M_BRIGHTNESS,
  M_VOLUME,
  M_CLOCK_TIME,
  M_CLOCK_DATE,
  M_KEYMODE,
  M_KBLAYOUT,
  M_NETWORK,
  M_HIDE_CLIENTS,
  M_BATTERY,
  M_CUSTOM = 100, // + index into g_cfg.customs
};

typedef struct {
  char name[64]; // e.g. "power" -> CSS #custom-power
  char exec[256]; // command that prints the module text
  char format[256]; // format string; "{}" is replaced with exec output
  int interval; // refresh interval in seconds
  char output[256]; // latest command output
  uint64_t last_run_ms;
  bool enabled;
} MangoCustomModule;

typedef struct {
  char module[32];
  char left[256];
  char middle[256];
  char right[256];
  char scroll_up[256];
  char scroll_down[256];
  int scroll_interval; // ms; -1 = use the global setting
  double smooth_scroll_threshold; // -1 = use the global setting
} MangoAction;

typedef struct {
  char module[32]; /* internal module name, e.g. "cpu", "custom-power" */
  char fmt[256];   /* format-alt string */
} MangoAltFormat;

typedef struct {
  char module[32]; /* internal module name */
  int max_length;  /* 0 = unlimited */
} MangoMaxLen;

typedef struct {
  int bar_height;
  int buffer_scale;
  int radius_default;
  int layer;
  int max_title_len;
  int sys_interval;
  int tag_count;
  int scroll_interval; // ms; 0 disables scroll debounce
  double smooth_scroll_threshold; // axis units per synthetic scroll step
  char font[256]; // fallback font when CSS sets none
  char tag_names[MANGOBAR_MAX_TAGS][16];
  uint32_t pinned_tags; // bitmask of tags always shown
  char overview_label[64];
  bool only_occupied;
  char separator[16];
  int tray_pad;
  int tray_gap;
  int tray_icon_size; // 0 = auto from bar height
  char brightness_dev[64];
  char brightness_fmt[64];
  char volume_ctrl[32];
  int volume_mix_index;
  char volume_fmt[64];
  char volume_fmt_muted[64];
  char layout_format[64];
  char title_format[128];
  char cpu_format[64];
  char mem_format[64];
  char clock_time_format[128];
  char clock_date_format[128];
  char keymode_format[64];
  char keyboardlayout_format[64];
  char network_format[64];
  char hide_clients_format[64];
  char battery_dev[64];
  char battery_fmt[64];
  char battery_icon_charging[16];
  char battery_icon_full[16];
  char battery_icon_discharging[16];
  char battery_icon_ac[16];
  char battery_icons[MANGOBAR_MAX_ICONS][16];
  int battery_icon_count;
  char brightness_icons[MANGOBAR_MAX_ICONS][16];
  int brightness_icon_count;
  char volume_icons[MANGOBAR_MAX_ICONS][16];
  int volume_icon_count;
  char volume_muted_icon[16];
  char volume_bt_icon[16];
  bool hide_on_ac;
  int left_order[MANGOBAR_MAX_MODULES];
  int left_count;
  int center_order[MANGOBAR_MAX_MODULES];
  int center_count;
  int right_order[MANGOBAR_MAX_MODULES];
  int right_count;
  MangoCustomModule customs[MANGOBAR_MAX_CUSTOM];
  int custom_count;
  MangoAction actions[MANGOBAR_MAX_ACTIONS];
  int action_count;
  MangoAltFormat alts[MANGOBAR_MAX_ALTS];
  int alt_count;
  MangoMaxLen max_lens[MANGOBAR_MAX_LENS];
  int max_len_count;
  char css_path[512];
} MangoConfig;

extern MangoConfig g_cfg;

void mango_config_defaults(void);
// Load config from JSONC; returns 0 on success
int mango_config_load(const char *path);
// Parse JSONC config from a string; returns 0 on success
int mango_config_parse(const char *jsonc);
// Search order: $MANGOBAR_CONFIG, ~/.config/mangobar/config.jsonc
const char *mango_config_find_default(char *buf, size_t sz);

#endif

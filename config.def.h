#ifndef CONFIG_H
#define CONFIG_H

static const char *fontstr = "Maple Mono NF CN:style=Bold:size=24";
static const int bar_height = 30;
static const int buffer_scale = 1;
static const int max_title_len = 50;

#define TAG_COUNT 9
static const char *tag_names[TAG_COUNT] = {
    " 1 ", " 2 ", " 3 ", " 4 ", " 5 ", " 6 ", " 7 ", " 8 ", " 9 "
};

/* enable module */
#define show_tags            1
#define show_layout          1
#define show_title           1
#define show_cpu             1
#define show_mem             1
#define show_clock           1
#define show_keymode         0
#define show_keyboardlayout  0

/* show only occupied tags */
#define show_only_occupied_tags 1

/* separator */
#define separator_str " | "

/* ========== color definitions (ARGB hexadecimal) ========== */
#define active_fg_color_hex     0x000000FF
#define active_bg_color_hex     0x8BAA9BFF
#define occupied_fg_color_hex   0xc3b695FF
#define occupied_bg_color_hex   0x201B14FF
#define inactive_fg_color_hex   0xC68A93FF
#define inactive_bg_color_hex   0x201B14FF
#define urgent_fg_color_hex     0x201B14FF
#define urgent_bg_color_hex     0xDBD0C6FF
#define empty_fg_color_hex      0xC68A93FF
#define empty_bg_color_hex      0x201B14FF

/* --- layout symbol --- */
#define layout_fg_color_hex     0xC68A93FF
#define layout_bg_color_hex     0x201B14FF

/* --- title --- */
#define title_fg_color_hex      0xC68A93FF
#define title_bg_color_hex      0x201B14FF

/* --- right side modules --- */
#define cpu_fg_color_hex        0xC68A93FF
#define cpu_bg_color_hex        0x201B14FF
#define mem_fg_color_hex        0xC68A93FF
#define mem_bg_color_hex        0x201B14FF
#define clock_fg_color_hex      0xC68A93FF
#define clock_bg_color_hex      0x201B14FF
#define keymode_fg_color_hex    0xC68A93FF
#define keymode_bg_color_hex    0x201B14FF
#define keyboardlayout_fg_color_hex 0xC68A93FF
#define keyboardlayout_bg_color_hex 0x201B14FF

/* --- OVERVIEW mode (text displayed when active_tags == [0]) --- */
#define overview_fg_color_hex   0x111012FF
#define overview_bg_color_hex   0x718b80FF

/* --- separator --- */
#define separator_fg_color_hex  0xC68A93FF
#define separator_bg_color_hex  0x201B14FF

/* --- middle blank area background --- */
#define middle_bg_color_hex      0x201B14FF
#define middle_bg_sel_color_hex  0x201B14FF

#endif

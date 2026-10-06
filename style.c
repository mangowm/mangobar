#define _GNU_SOURCE
#include "style.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MANGOBAR_MAX_IMPORT_DEPTH 8

static void apply_rule(Style *dst, const StyleRule *r);
static int style_parse_internal(StyleSheet *ss, const char *buf,
                                const char *base_dir, char (*stack)[PATH_MAX],
                                int depth);
static int style_load_recursive(StyleSheet *ss, const char *path,
                                char (*stack)[PATH_MAX], int depth);

static char *read_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  buf[rd] = '\0';
  fclose(f);
  return buf;
}

static int parse_import_url(const char **pp, char *out, size_t outsz) {
  const char *p = *pp;
  while (*p && isspace((unsigned char)*p))
    p++;
  out[0] = '\0';
  if (*p == '"' || *p == '\'') {
    char q = *p++;
    size_t n = 0;
    while (*p && *p != q && n + 1 < outsz)
      out[n++] = *p++;
    out[n] = '\0';
    if (*p == q)
      p++;
    *pp = p;
    return 1;
  }
  if (strncmp(p, "url(", 4) == 0) {
    p += 4;
    while (*p && isspace((unsigned char)*p))
      p++;
    char q = 0;
    if (*p == '"' || *p == '\'')
      q = *p++;
    size_t n = 0;
    if (q) {
      while (*p && *p != q && n + 1 < outsz)
        out[n++] = *p++;
      if (*p == q)
        p++;
    } else {
      while (*p && *p != ')' && !isspace((unsigned char)*p) &&
             n + 1 < outsz)
        out[n++] = *p++;
    }
    out[n] = '\0';
    while (*p && *p != ')')
      p++;
    if (*p == ')')
      p++;
    *pp = p;
    return 1;
  }
  return 0;
}

static void expand_tilde(const char *in, char *out, size_t outsz) {
  if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
    const char *home = getenv("HOME");
    if (home && *home) {
      snprintf(out, outsz, "%s%s", home, in + 1);
      return;
    }
  }
  snprintf(out, outsz, "%s", in);
}

static int config_dir(char *out, size_t outsz) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    snprintf(out, outsz, "%s/mangobar", xdg);
    return 0;
  }
  const char *home = getenv("HOME");
  if (home && *home) {
    snprintf(out, outsz, "%s/.config/mangobar", home);
    return 0;
  }
  return -1;
}

static int try_load(StyleSheet *ss, const char *full, char (*stack)[PATH_MAX],
                    int depth) {
  if (access(full, R_OK) != 0)
    return -1;
  return style_load_recursive(ss, full, stack, depth + 1) == 0 ? 0 : -1;
}

static int join_path(char *out, size_t outsz, const char *dir,
                     const char *file) {
  size_t dl = strlen(dir), fl = strlen(file);
  if (dl + 1 + fl + 1 > outsz)
    return -1;
  memcpy(out, dir, dl);
  out[dl] = '/';
  memcpy(out + dl + 1, file, fl + 1);
  return 0;
}

static void import_css(StyleSheet *ss, const char *base_dir, const char *url,
                       char (*stack)[PATH_MAX], int depth) {
  char full[PATH_MAX];
  if (url[0] == '~' || url[0] == '/') {
    expand_tilde(url, full, sizeof(full));
    if (try_load(ss, full, stack, depth) != 0)
      fprintf(stderr, "mangobar: failed to import css: %s\n", full);
    return;
  }
  char cfg[PATH_MAX];
  int have_cfg = config_dir(cfg, sizeof(cfg)) == 0;
  const char *dirs[3] = {base_dir, have_cfg ? cfg : NULL, "."};
  for (int i = 0; i < 3; i++) {
    if (!dirs[i] || !*dirs[i])
      continue;
    if (join_path(full, sizeof(full), dirs[i], url) != 0)
      continue;
    if (try_load(ss, full, stack, depth) == 0)
      return;
  }
  fprintf(stderr, "mangobar: failed to import css: %s\n", url);
}

static void skip_ws_comments(const char **p) {
  for (;;) {
    const char *s = *p;
    while (*s && isspace((unsigned char)*s))
      s++;
    if (s[0] == '/' && s[1] == '*') {
      s += 2;
      while (*s && !(s[0] == '*' && s[1] == '/'))
        s++;
      if (*s)
        s += 2;
      *p = s;
    } else {
      *p = s;
      return;
    }
  }
}

// Parse #RGB / #RGBA / #RRGGBB / #AARRGGBB -> ARGB (uint32)
static int parse_hex_color(const char *s, uint32_t *out) {
  if (*s == '#')
    s++;
  size_t n = strlen(s);
  if (n != 3 && n != 4 && n != 6 && n != 8)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (!isxdigit((unsigned char)s[i]))
      return 0;
  char buf[16];
  int b = 0;
  if (n == 3 || n == 4) {
    for (size_t i = 0; i < n; i++) {
      buf[b++] = s[i];
      buf[b++] = s[i];
    }
    buf[b] = '\0';
  } else {
    strcpy(buf, s);
    b = (int)n;
  }
  unsigned long v = strtoul(buf, NULL, 16);
  if (b == 6)
    v = (v << 8) | 0xFFu; // fill alpha when absent
  *out = (uint32_t)v;
  return 1;
}

static int parse_int(const char *s) {
  while (*s && !isdigit((unsigned char)*s))
    s++;
  if (!isdigit((unsigned char)*s))
    return 0;
  int sign = 1;
  const char *q = s;
  while (q > s && (q[-1] == '-' || q[-1] == '+')) {
    if (q[-1] == '-')
      sign = -sign;
    q--;
  }
  return sign * atoi(s);
}

static const char *find_var(const StyleSheet *ss, const char *name) {
  for (int i = 0; i < ss->var_count; i++)
    if (strcmp(ss->var_names[i], name) == 0)
      return ss->var_values[i];
  return NULL;
}

// Resolve a color value to hex:
// supports #hex, @var, linear-gradient(...) (first color), none (transparent)
static int resolve_color_value(const StyleSheet *ss, const char *val,
                               char *out, size_t outsz) {
  const char *p = val;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (strncmp(p, "none", 4) == 0) {
    snprintf(out, outsz, "#00000000");
    return 1;
  }
  if (*p == '#') {
    snprintf(out, outsz, "%s", p);
    return 1;
  }
  if (*p == '@') {
    p++;
    char name[64];
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && p[n] != ';' &&
           p[n] != ')' && n + 1 < sizeof(name))
      {
        name[n] = p[n];
        n++;
      }
    name[n] = '\0';
    const char *v = find_var(ss, name);
    if (v)
      snprintf(out, outsz, "%s", v);
    return v ? 1 : 0;
  }
  // Find first #hex or @var inside parentheses
  while (*p) {
    if (*p == '#') {
      const char *q = p;
      while (*q && !isspace((unsigned char)*q) && *q != ')' && *q != ',')
        q++;
      size_t n = (size_t)(q - p);
      if (n < outsz) {
        memcpy(out, p, n);
        out[n] = '\0';
        return 1;
      }
      return 0;
    }
    if (*p == '@') {
      p++;
      char name[64];
      size_t n = 0;
      while (p[n] && !isspace((unsigned char)p[n]) && p[n] != ')' &&
             p[n] != ',' && n + 1 < sizeof(name))
        {
          name[n] = p[n];
          n++;
        }
      name[n] = '\0';
      const char *v = find_var(ss, name);
      if (v) {
        snprintf(out, outsz, "%s", v);
        return 1;
      }
      return 0;
    }
    p++;
  }
  return 0;
}

// Mix two ARGB colors with ratio t (0..1), interpolating each channel.
static uint32_t mix_colors(uint32_t a, uint32_t b, double t) {
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    double ca = (a >> sh) & 0xFF;
    double cb = (b >> sh) & 0xFF;
    out |= (uint32_t)(ca + (cb - ca) * t + 0.5) << sh;
  }
  return out;
}

// Resolve a single color token: #hex, @var, or mix(color, color, t).
static int parse_color_token(const StyleSheet *ss, const char *tok,
                             uint32_t *out, int depth) {
  if (depth > 8)
    return 0;
  while (*tok && isspace((unsigned char)*tok))
    tok++;
  if (strncmp(tok, "mix(", 4) == 0) {
    const char *p = tok + 4;
    // Split the arguments on top-level commas (track paren depth).
    const char *args[3] = {NULL, NULL, NULL};
    int ai = 0;
    int depth2 = 0;
    const char *start = p;
    for (; *p && ai < 3; p++) {
      if (*p == '(')
        depth2++;
      else if (*p == ')') {
        if (depth2 == 0)
          break;
        depth2--;
      } else if (*p == ',' && depth2 == 0) {
        if (ai < 3)
          args[ai++] = start;
        start = p + 1;
      }
    }
    if (ai == 2) {
      args[2] = start;
      char tbuf[32] = {0};
      uint32_t c1, c2;
      const char *q = args[2];
      size_t n = 0;
      while (*q && *q != ')' && n + 1 < sizeof(tbuf))
        tbuf[n++] = *q++;
      if (parse_color_token(ss, args[0], &c1, depth + 1) &&
          parse_color_token(ss, args[1], &c2, depth + 1)) {
        *out = mix_colors(c1, c2, atof(tbuf));
        return 1;
      }
    }
    return 0;
  }
  if (*tok == '@') {
    char name[64];
    size_t n = 0;
    tok++;
    while (tok[n] && !isspace((unsigned char)tok[n]) && tok[n] != ',' &&
           tok[n] != ')' && n + 1 < sizeof(name))
      {
        name[n] = tok[n];
        n++;
      }
    name[n] = '\0';
    const char *v = find_var(ss, name);
    return v ? parse_color_token(ss, v, out, depth + 1) : 0;
  }
  if (*tok == '#') {
    // Truncate at delimiters so a trailing ')' from a gradient is ignored.
    char buf[16];
    size_t n = 0;
    while (tok[n] && n + 1 < sizeof(buf) &&
           !isspace((unsigned char)tok[n]) && tok[n] != ')' && tok[n] != ',')
      {
        buf[n] = tok[n];
        n++;
      }
    buf[n] = '\0';
    return parse_hex_color(buf, out);
  }
  if (strncmp(tok, "transparent", 11) == 0 ||
      strncmp(tok, "none", 4) == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

// Parse "linear-gradient(to top, c1, c2)" (or to bottom/left/right; plain
// two-color gradients; stops may use @vars and mix()). Fills start/end ARGB
// and direction. Returns 1 on success.
static int parse_linear_gradient(const StyleSheet *ss, const char *val,
                                 uint32_t *c1, uint32_t *c2, int *dir) {
  const char *p = val;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (strncasecmp(p, "linear-gradient", 15) != 0)
    return 0;
  p += 15;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p != '(')
    return 0;
  p++;

  // Everything until the matching ')' is the argument list.
  int depth = 1;
  const char *start = p;
  while (*p && depth > 0) {
    if (*p == '(')
      depth++;
    else if (*p == ')')
      depth--;
    if (depth > 0)
      p++;
  }
  size_t len = (size_t)(p - start);
  if (len == 0)
    return 0;
  char argbuf[512];
  if (len >= sizeof(argbuf))
    len = sizeof(argbuf) - 1;
  memcpy(argbuf, start, len);
  argbuf[len] = '\0';

  *dir = 0; // default: to bottom
  char *q = argbuf;
  while (*q && isspace((unsigned char)*q))
    q++;
  if (strncmp(q, "to ", 3) == 0) {
    q += 3;
    while (*q && isspace((unsigned char)*q))
      q++;
    if (strncmp(q, "top", 3) == 0) {
      *dir = 1;
      q += 3;
    } else if (strncmp(q, "bottom", 6) == 0) {
      *dir = 0;
      q += 6;
    } else if (strncmp(q, "left", 4) == 0) {
      *dir = 2;
      q += 4;
    } else if (strncmp(q, "right", 5) == 0) {
      *dir = 3;
      q += 5;
    }
    while (*q && *q != ',')
      q++;
    if (*q == ',')
      q++;
  }

  // Split the two color stops on top-level commas.
  const char *stops[2] = {q, NULL};
  int si = 1;
  int d2 = 0;
  char *s = q;
  for (; *s && si < 2; s++) {
    if (*s == '(')
      d2++;
    else if (*s == ')') {
      if (d2 > 0)
        d2--;
    } else if (*s == ',' && d2 == 0) {
      stops[si++] = s + 1;
      *s = '\0';
    }
  }
  if (si < 2)
    return 0;
  return parse_color_token(ss, stops[0], c1, 0) &&
         parse_color_token(ss, stops[1], c2, 0);
}

// Parse 1-2 ints: one value -> all sides; two -> vertical, horizontal
static void parse_int_pair(const char *s, int *a, int *b) {
  *a = parse_int(s);
  const char *p = s;
  while (*p && !isdigit((unsigned char)*p))
    p++;
  if (!*p)
    return;
  while (*p && (isdigit((unsigned char)*p) || *p == '-' || *p == '+'))
    p++;
  while (*p && !isdigit((unsigned char)*p))
    p++;
  if (*p)
    *b = parse_int(p);
  else
    *b = *a;
}

static void apply_decl(StyleSheet *ss, Style *st, const char *prop,
                       const char *val) {
  uint32_t c;
  char color_buf[64];
  if (strcmp(prop, "color") == 0 &&
      resolve_color_value(ss, val, color_buf, sizeof(color_buf)) &&
      parse_hex_color(color_buf, &c)) {
    st->color = c;
    st->color_set = true;
  } else if (strcmp(prop, "background") == 0 ||
             strcmp(prop, "background-color") == 0) {
    uint32_t c1, c2;
    int dir;
    if (parse_linear_gradient(ss, val, &c1, &c2, &dir)) {
      st->background = c1;
      st->gradient_end = c2;
      st->gradient_dir = dir;
      st->bg_gradient = true;
      st->background_set = true;
    } else if (resolve_color_value(ss, val, color_buf, sizeof(color_buf)) &&
               parse_hex_color(color_buf, &c)) {
      st->background = c;
      st->bg_gradient = false;
      st->background_set = true;
    }
  } else if (strcmp(prop, "border-color") == 0 &&
             resolve_color_value(ss, val, color_buf, sizeof(color_buf)) &&
             parse_hex_color(color_buf, &c)) {
    st->border_color = c;
    st->border_color_set = true;
  } else if (strcmp(prop, "border-radius") == 0) {
    st->radius = parse_int(val);
    st->radius_set = true;
  } else if (strcmp(prop, "min-width") == 0) {
    st->min_width = parse_int(val);
    st->min_width_set = true;
  } else if (strcmp(prop, "padding") == 0) {
    int a, b;
    parse_int_pair(val, &a, &b);
    st->padding_left = st->padding_right = b;
    st->padding_left_set = st->padding_right_set = true;
  } else if (strcmp(prop, "padding-left") == 0) {
    st->padding_left = parse_int(val);
    st->padding_left_set = true;
  } else if (strcmp(prop, "padding-right") == 0) {
    st->padding_right = parse_int(val);
    st->padding_right_set = true;
  } else if (strcmp(prop, "margin") == 0) {
    int a, b;
    parse_int_pair(val, &a, &b);
    st->margin_top = a;
    st->margin_top_set = true;
    st->margin_left = st->margin_right = b;
    st->margin_left_set = st->margin_right_set = true;
  } else if (strcmp(prop, "margin-top") == 0) {
    st->margin_top = parse_int(val);
    st->margin_top_set = true;
  } else if (strcmp(prop, "margin-left") == 0) {
    st->margin_left = parse_int(val);
    st->margin_left_set = true;
  } else if (strcmp(prop, "margin-right") == 0) {
    st->margin_right = parse_int(val);
    st->margin_right_set = true;
  } else if (strcmp(prop, "font-family") == 0) {
    if (val[0]) {
      // Take only the first family name, strip quotes and fallbacks
      const char *p = val;
      while (*p && isspace((unsigned char)*p))
        p++;
      char fam[sizeof(ss->font_family)];
      size_t n = 0;
      bool quoted = false;
      if (*p == '"' || *p == '\'') {
        quoted = true;
        p++;
      }
      while (*p && n + 1 < sizeof(fam)) {
        if (quoted && (*p == '"' || *p == '\'')) {
          p++;
          break;
        }
        if (!quoted && *p == ',')
          break;
        fam[n++] = *p++;
      }
      while (n > 0 && isspace((unsigned char)fam[n - 1]))
        n--;
      fam[n] = '\0';
      if (fam[0]) {
        strncpy(ss->font_family, fam, sizeof(ss->font_family) - 1);
        ss->font_family[sizeof(ss->font_family) - 1] = '\0';
        ss->font_set = true;
      }
    }
  } else if (strcmp(prop, "font-size") == 0) {
    ss->font_size = parse_int(val);
    if (ss->font_size > 0)
      ss->font_set = true;
  } else if (strcmp(prop, "font-weight") == 0) {
    if (val[0]) {
      strncpy(ss->font_weight, val, sizeof(ss->font_weight) - 1);
      ss->font_weight[sizeof(ss->font_weight) - 1] = '\0';
      ss->font_set = true;
    }
  }
  // Ignore: font shorthand, border, border-radius, opacity, etc.
}

void style_sheet_init(StyleSheet *ss) {
  memset(ss, 0, sizeof(*ss));
}

// Parse a selector string (with optional state) into module / state
static void parse_selector(const char *sel, char *module, size_t msz,
                           char *state, size_t ssz) {
  module[0] = state[0] = '\0';
  const char *p = sel;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '*') {
    strncpy(module, "*", msz - 1);
    p++;
  } else if (*p == '#') {
    p++;
    const char *q = p;
    while (*q && *q != '.' && *q != ':' && !isspace((unsigned char)*q) &&
           *q != '{')
      q++;
    size_t n = (size_t)(q - p);
    if (n >= msz)
      n = msz - 1;
    memcpy(module, p, n);
    module[n] = '\0';
    p = q;
  } else if (strncmp(p, "menuitem", 8) == 0) {
    strncpy(module, "menuitem", msz - 1);
    p += 8;
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == ':') {
      p++;
      const char *q = p;
      while (*q && !isspace((unsigned char)*q) && *q != '{' && *q != '}')
        q++;
      size_t n = (size_t)(q - p);
      if (n >= ssz)
        n = ssz - 1;
      memcpy(state, p, n);
      state[n] = '\0';
    }
    return;
  } else if (strncmp(p, "menu", 4) == 0) {
    strncpy(module, "menu", msz - 1);
    return;
  }
  // Skip child prefixes like "button"
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '>') {
    // Child rules like #tray > .passive don't apply to the module
    module[0] = '\0';
    return;
  }
  if (strncmp(p, "button", 6) == 0)
    p += 6;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (*p == '.') {
    p++;
    const char *q = p;
    while (*q && !isspace((unsigned char)*q) && *q != '{' && *q != '}')
      q++;
    size_t n = (size_t)(q - p);
    if (n >= ssz)
      n = ssz - 1;
    memcpy(state, p, n);
    state[n] = '\0';
  } else if (*p == ':') {
    p++;
    const char *q = p;
    while (*q && !isspace((unsigned char)*q) && *q != '{' && *q != '}')
      q++;
    size_t n = (size_t)(q - p);
    if (n >= ssz)
      n = ssz - 1;
    memcpy(state, p, n);
    state[n] = '\0';
  }
}

static int style_parse_internal(StyleSheet *ss, const char *buf,
                                const char *base_dir, char (*stack)[PATH_MAX],
                                int depth) {
  const char *p = buf;
  for (;;) {
    skip_ws_comments(&p);
    if (!*p)
      break;

    if (strncmp(p, "@import", 7) == 0 &&
        !(isalnum((unsigned char)p[7]) || p[7] == '-' || p[7] == '_')) {
      p += 7;
      char url[PATH_MAX];
      if (parse_import_url(&p, url, sizeof(url)) && url[0]) {
        while (*p && *p != ';')
          p++;
        if (*p == ';')
          p++;
        import_css(ss, base_dir, url, stack, depth);
      } else {
        while (*p && *p != ';')
          p++;
        if (*p == ';')
          p++;
      }
      continue;
    }

    // @define-color name value;
    if (strncmp(p, "@define-color", 13) == 0) {
      p += 13;
      while (*p && isspace((unsigned char)*p))
        p++;
      char name[64];
      size_t nl = 0;
      while (*p && !isspace((unsigned char)*p) && *p != ';' &&
             nl + 1 < sizeof(name))
        name[nl++] = *p++;
      name[nl] = '\0';
      while (*p && isspace((unsigned char)*p))
        p++;
      char val[64];
      size_t vl = 0;
      while (*p && *p != ';' && vl + 1 < sizeof(val))
        val[vl++] = *p++;
      val[vl] = '\0';
      if (*p == ';')
        p++;
      if (nl && vl && ss->var_count < MANGOBAR_MAX_VARS) {
        snprintf(ss->var_names[ss->var_count], sizeof(ss->var_names[0]), "%s",
                 name);
        snprintf(ss->var_values[ss->var_count], sizeof(ss->var_values[0]),
                 "%s", val);
        ss->var_count++;
      }
      continue;
    }

    // Skip @keyframes ... { ... } blocks
    if (strncmp(p, "@keyframes", 10) == 0) {
      while (*p && *p != '{')
        p++;
      if (*p == '{') {
        int depth = 0;
        while (*p) {
          if (*p == '{')
            depth++;
          else if (*p == '}' && --depth <= 0) {
            p++;
            break;
          }
          p++;
        }
      }
      continue;
    }

    // Selector list
    char selector[2048];
    size_t sl = 0;
    while (*p && *p != '{' && sl < sizeof(selector) - 1)
      selector[sl++] = *p++;
    selector[sl] = '\0';
    if (*p == '{')
      p++;
    bool is_menu_rule = strstr(selector, "menu") != NULL;

    // Declaration block
    Style decl = {0};
    for (;;) {
      skip_ws_comments(&p);
      if (!*p)
        break;
      if (*p == '}') {
        p++;
        break;
      }
      char prop[64];
      size_t pl = 0;
      while (*p && *p != ':' && *p != ';' && *p != '}' &&
             pl < sizeof(prop) - 1)
        prop[pl++] = *p++;
      prop[pl] = '\0';
      while (*p && *p != ':' && *p != ';' && *p != '}')
        p++;
      if (*p == ':')
        p++;
      skip_ws_comments(&p);
      char val[512];
      size_t vl = 0;
      while (*p && *p != ';' && *p != '}' && vl < sizeof(val) - 1)
        val[vl++] = *p++;
      val[vl] = '\0';
      char *vp = val;
      while (*vp && isspace((unsigned char)*vp))
        vp++;
      char *ve = val + vl;
      while (ve > vp && isspace((unsigned char)ve[-1]))
        ve--;
      *ve = '\0';
      if (*p == ';')
        p++;
      if (prop[0]) {
        if (is_menu_rule && strcmp(prop, "font-size") == 0)
          ss->menu_font_size = parse_int(vp);
        else
          apply_decl(ss, &decl, prop, vp);
      }
    }

    // Split comma-separated selectors
    char *cs = strdup(selector);
    if (!cs)
      continue;
    char *tok = strtok(cs, ",");
    while (tok && ss->rule_count < MANGOBAR_MAX_STYLE_RULES) {
      StyleRule *r = &ss->rules[ss->rule_count++];
      parse_selector(tok, r->module, sizeof(r->module), r->state,
                     sizeof(r->state));
      r->style = decl;
      if (strcmp(r->module, "menu") == 0 && r->state[0] == '\0') {
        apply_rule(&ss->menu, r);
        if (r->style.radius_set)
          ss->menu_radius = r->style.radius;
      } else if (strcmp(r->module, "menuitem") == 0 &&
                 r->state[0] == '\0') {
        apply_rule(&ss->menuitem, r);
      } else if (strcmp(r->module, "menuitem") == 0 &&
                 strcmp(r->state, "hover") == 0) {
        apply_rule(&ss->menuitem_hover, r);
      }
      tok = strtok(NULL, ",");
    }
    free(cs);
  }
  ss->loaded = true;
  return 0;
}

static int style_load_recursive(StyleSheet *ss, const char *path,
                                char (*stack)[PATH_MAX], int depth) {
  if (depth >= MANGOBAR_MAX_IMPORT_DEPTH) {
    fprintf(stderr, "mangobar: css @import nested too deeply: %s\n", path);
    return -1;
  }
  char real[PATH_MAX];
  expand_tilde(path, real, sizeof(real));
  char canon[PATH_MAX];
  if (!realpath(real, canon))
    snprintf(canon, sizeof(canon), "%s", real);
  for (int i = 0; i < depth; i++) {
    if (strcmp(stack[i], canon) == 0) {
      fprintf(stderr, "mangobar: css @import cycle ignored: %s\n", canon);
      return -1;
    }
  }
  snprintf(stack[depth], PATH_MAX, "%s", canon);

  char *buf = read_file(real);
  if (!buf) {
    fprintf(stderr, "mangobar: cannot read css file: %s\n", real);
    return -1;
  }
  char base_dir[PATH_MAX];
  snprintf(base_dir, sizeof(base_dir), "%s", real);
  char *slash = strrchr(base_dir, '/');
  if (slash)
    *slash = '\0';
  else
    snprintf(base_dir, sizeof(base_dir), ".");

  int ret = style_parse_internal(ss, buf, base_dir, stack, depth);
  free(buf);
  return ret;
}

int style_sheet_parse(StyleSheet *ss, const char *buf) {
  char stack[MANGOBAR_MAX_IMPORT_DEPTH][PATH_MAX];
  return style_parse_internal(ss, buf, NULL, stack, 0);
}

int style_sheet_load(StyleSheet *ss, const char *path) {
  char stack[MANGOBAR_MAX_IMPORT_DEPTH][PATH_MAX];
  return style_load_recursive(ss, path, stack, 0);
}

static void apply_rule(Style *dst, const StyleRule *r) {
  const Style *s = &r->style;
  if (s->color_set)
    dst->color = s->color;
  if (s->background_set) {
    dst->background = s->background;
    dst->gradient_end = s->gradient_end;
    dst->gradient_dir = s->gradient_dir;
    dst->bg_gradient = s->bg_gradient;
  }
  if (s->border_color_set)
    dst->border_color = s->border_color;
  if (s->padding_left_set)
    dst->padding_left = s->padding_left;
  if (s->padding_right_set)
    dst->padding_right = s->padding_right;
  if (s->margin_top_set)
    dst->margin_top = s->margin_top;
  if (s->margin_left_set)
    dst->margin_left = s->margin_left;
  if (s->margin_right_set)
    dst->margin_right = s->margin_right;
  if (s->radius_set)
    dst->radius = s->radius;
  if (s->min_width_set)
    dst->min_width = s->min_width;
  dst->color_set |= s->color_set;
  dst->background_set |= s->background_set;
  dst->border_color_set |= s->border_color_set;
  dst->padding_left_set |= s->padding_left_set;
  dst->padding_right_set |= s->padding_right_set;
  dst->margin_top_set |= s->margin_top_set;
  dst->margin_left_set |= s->margin_left_set;
  dst->margin_right_set |= s->margin_right_set;
  dst->radius_set |= s->radius_set;
  dst->min_width_set |= s->min_width_set;
}

Style style_overlay(const Style *base, const Style *over) {
  Style d = *base;
  StyleRule fake = {.style = *over};
  apply_rule(&d, &fake);
  return d;
}

Style style_resolve(const StyleSheet *ss, const char *module,
                    const char *state) {
  Style s = {0};
  // 1. "*" rules
  for (int i = 0; i < ss->rule_count; i++) {
    const StyleRule *r = &ss->rules[i];
    if (strcmp(r->module, "*") == 0 &&
        (r->state[0] == '\0' ||
         (state && strcmp(r->state, state) == 0)))
      apply_rule(&s, r);
  }
  // 2. "#module"
  for (int i = 0; i < ss->rule_count; i++) {
    const StyleRule *r = &ss->rules[i];
    if (strcmp(r->module, module) == 0 && r->state[0] == '\0')
      apply_rule(&s, r);
  }
  // 3. "#module.state"
  if (state && state[0]) {
    for (int i = 0; i < ss->rule_count; i++) {
      const StyleRule *r = &ss->rules[i];
      if (strcmp(r->module, module) == 0 &&
          strcmp(r->state, state) == 0)
        apply_rule(&s, r);
    }
  }
  return s;
}

Style style_resolve_module_only(const StyleSheet *ss, const char *module,
                                const char *state) {
  Style s = {0};
  for (int i = 0; i < ss->rule_count; i++) {
    const StyleRule *r = &ss->rules[i];
    if (strcmp(r->module, module) == 0 && r->state[0] == '\0')
      apply_rule(&s, r);
  }
  if (state && state[0]) {
    for (int i = 0; i < ss->rule_count; i++) {
      const StyleRule *r = &ss->rules[i];
      if (strcmp(r->module, module) == 0 && strcmp(r->state, state) == 0)
        apply_rule(&s, r);
    }
  }
  return s;
}

const char *style_font_string(const StyleSheet *ss, const char *fallback) {
  if (!ss->font_set || !ss->font_family[0])
    return fallback;
  static char buf[256];
  const char *weight = ss->font_weight[0] ? ss->font_weight : "Regular";
  int size = ss->font_size > 0 ? ss->font_size : 12;
  // fcft format: family:style=weight:size=N
  snprintf(buf, sizeof(buf), "%s:style=%s:size=%d", ss->font_family, weight,
           size);
  return buf;
}

int style_find_default_path(char *out, size_t outsz) {
  // 1. $MANGOBAR_CSS
  const char *env = getenv("MANGOBAR_CSS");
  if (env && *env && access(env, R_OK) == 0) {
    strncpy(out, env, outsz - 1);
    out[outsz - 1] = '\0';
    return 0;
  }
  // 2. $XDG_CONFIG_HOME/mangobar/style.css
  const char *xdg = getenv("XDG_CONFIG_HOME");
  char path[512];
  if (xdg && *xdg) {
    snprintf(path, sizeof(path), "%s/mangobar/style.css", xdg);
    if (access(path, R_OK) == 0) {
      strncpy(out, path, outsz - 1);
      out[outsz - 1] = '\0';
      return 0;
    }
  }
  // 3. ~/.config/mangobar/style.css
  const char *home = getenv("HOME");
  if (home && *home) {
    snprintf(path, sizeof(path), "%s/.config/mangobar/style.css", home);
    if (access(path, R_OK) == 0) {
      strncpy(out, path, outsz - 1);
      out[outsz - 1] = '\0';
      return 0;
    }
  }
  return -1;
}

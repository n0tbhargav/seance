// seance: minimal GTK3 terminal emulator on libghostty-vt.
// libghostty-vt = VT parsing/state/key encoding; this file adds PTY + GTK/Cairo/Pango.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ghostty/vt.h>

#define DEFAULT_FONT "Monospace 11"
#define SCROLLBACK_LINES 10000

typedef struct {
  GtkWidget *win, *area;
  GhosttyTerminal term;
  GhosttyRenderState rs;
  GhosttyRenderStateRowIterator rows;
  GhosttyRenderStateRowCells cells;
  GhosttyKeyEncoder kenc;
  GhosttyKeyEvent kev;
  GhosttyMouseEncoder menc;
  GhosttyMouseEvent mev;
  bool dragging, have_sel;
  int sel_x0, sel_y0, sel_x1, sel_y1;
  int btn_down;   // GDK button currently held, 0 if none
  PangoLayout *layout;
  int cw, ch;          // cell size px
  uint16_t ncols, nrows;
  int pty;
  pid_t child;
  GIOChannel *chan;
} App;

static App A;
static cairo_surface_t *back;      // persistent back buffer; only dirty rows are re-rendered into it
static int back_w, back_h, prev_cy = -1;
static bool force_full = true;
static gint64 st_write_us, st_draw_us, st_reads, st_draws, st_bytes;   // SEANCE_STATS=1 -> printed at exit

typedef struct {
  char *font_family;
  double font_size;
  char *theme;
  bool has_fg, has_bg, has_cursor;
  GhosttyColorRgb fg, bg, cursor;
  bool pal_set[256];
  GhosttyColorRgb pal[256];
  int scrollback_lines;
} Config;
static Config C = {.font_size = 11, .scrollback_lines = SCROLLBACK_LINES};

static bool mouse_tracking(void);
static void send_mouse(GhosttyMouseAction act, guint button, double x, double y, guint st);
static void copy_selection_to(GdkAtom which);
static void apply_font(void);

static void pty_write(const void *buf, size_t len) {
  const char *p = buf;
  while (len > 0) {
    ssize_t n = write(A.pty, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) { g_usleep(500); continue; }
      return;
    }
    p += n; len -= n;
  }
}

// Terminal -> PTY replies (DA, DSR, mode reports...).
static void on_write_pty(GhosttyTerminal t, void *ud, const uint8_t *d, size_t n) {
  (void)t; (void)ud; pty_write(d, n);
}
static void on_bell(GhosttyTerminal t, void *ud) {
  (void)t; (void)ud; gdk_window_beep(gtk_widget_get_window(A.win));
}
static void on_title(GhosttyTerminal t, void *ud) {
  (void)ud;
  GhosttyString s = {0};
  if (ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_TITLE, &s) == GHOSTTY_SUCCESS && s.len) {
    char *title = g_strndup((const char *)s.ptr, s.len);
    gtk_window_set_title(GTK_WINDOW(A.win), title);
    g_free(title);
  }
}

static guint draw_timer;
static gint64 last_draw_end_us, avg_draw_us = 2000;

static gboolean draw_timer_cb(gpointer ud) {
  (void)ud; draw_timer = 0; gtk_widget_queue_draw(A.area); return G_SOURCE_REMOVE;
}

// Under output floods, cap drawing to roughly a quarter of the time (>= 3x the last frame cost);
// otherwise draw immediately. Parsing is ~100x cheaper than drawing, so this maximizes throughput.
static void request_draw(void) {
  if (draw_timer) return;
  gint64 wait = last_draw_end_us + MAX(8000, avg_draw_us * 3) - g_get_monotonic_time();
  if (wait <= 0) gtk_widget_queue_draw(A.area);
  else draw_timer = g_timeout_add(MAX(1, (guint)((wait + 999) / 1000)), draw_timer_cb, NULL);
}

static gboolean on_pty_readable(GIOChannel *src, GIOCondition cond, gpointer ud) {
  (void)src; (void)ud;
  static uint8_t buf[65536];
  if (cond & G_IO_IN) {
    // drain up to ~1 MB per wake-up before yielding to drawing
    for (int i = 0; i < 16; i++) {
      ssize_t n = read(A.pty, buf, sizeof buf);
      if (n > 0) {
        gint64 t0 = g_get_monotonic_time();
        ghostty_terminal_vt_write(A.term, buf, (size_t)n);
        st_write_us += g_get_monotonic_time() - t0; st_reads++; st_bytes += n;
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
      gtk_main_quit();  // child exited / EIO
      return G_SOURCE_REMOVE;
    }
    request_draw();
    return G_SOURCE_CONTINUE;
  }
  gtk_main_quit();
  return G_SOURCE_REMOVE;
}

static void measure_cell(const char *font) {
  PangoFontDescription *fd = pango_font_description_from_string(font);
  pango_layout_set_font_description(A.layout, fd);
  pango_font_description_free(fd);
  pango_layout_set_text(A.layout, "M", 1);
  PangoRectangle r;
  pango_layout_get_pixel_extents(A.layout, NULL, &r);
  A.cw = r.width > 0 ? r.width : 8;
  PangoFontMetrics *m = pango_context_get_metrics(
      pango_layout_get_context(A.layout),
      pango_layout_get_font_description(A.layout), NULL);
  A.ch = PANGO_PIXELS(pango_font_metrics_get_height(m));
  pango_font_metrics_unref(m);
  if (A.ch <= 0) A.ch = r.height;
}

static void do_resize(int w, int h) {
  GhosttyMouseEncoderSize ms = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
  ms.screen_width = w; ms.screen_height = h; ms.cell_width = A.cw; ms.cell_height = A.ch;
  ghostty_mouse_encoder_setopt(A.menc, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &ms);
  uint16_t c = (uint16_t)MAX(2, w / A.cw), r = (uint16_t)MAX(1, h / A.ch);
  if (c == A.ncols && r == A.nrows) return;
  A.ncols = c; A.nrows = r;
  ghostty_terminal_resize(A.term, c, r, A.cw, A.ch);
  struct winsize ws = {.ws_row = r, .ws_col = c,
                       .ws_xpixel = c * A.cw, .ws_ypixel = r * A.ch};
  ioctl(A.pty, TIOCSWINSZ, &ws);
}

static void on_size(GtkWidget *w, GtkAllocation *al, gpointer ud) {
  (void)w; (void)ud; do_resize(al->width, al->height);
}

static void set_rgb(cairo_t *cr, GhosttyColorRgb c) {
  cairo_set_source_rgb(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0);
}

static GhosttyColorRgb resolve(GhosttyStyleColor c, const GhosttyRenderStateColors *k,
                               GhosttyColorRgb fb) {
  if (c.tag == GHOSTTY_STYLE_COLOR_RGB) return c.value.rgb;
  if (c.tag == GHOSTTY_STYLE_COLOR_PALETTE) return k->palette[c.value.palette];
  return fb;
}

typedef struct {
  char txt[512];
  int len, x0;
  bool active;
  GhosttyColorRgb fg;
  GhosttyStyle st;
} Run;

static bool run_matches(const Run *r, GhosttyColorRgb fg, const GhosttyStyle *st) {
  return r->fg.r == fg.r && r->fg.g == fg.g && r->fg.b == fg.b && r->st.bold == st->bold &&
         r->st.italic == st->italic && r->st.faint == st->faint && r->st.underline == st->underline &&
         r->st.strikethrough == st->strikethrough;
}

// Draw one run of same-style text (a single Pango call for many cells; monospace + integer
// hinted advances keep it aligned to the cell grid).
static void run_flush(cairo_t *cr, Run *r, int y) {
  if (r->active && r->len > 0) {
    PangoAttrList *al = pango_attr_list_new();
    if (r->st.bold) pango_attr_list_insert(al, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    if (r->st.italic) pango_attr_list_insert(al, pango_attr_style_new(PANGO_STYLE_ITALIC));
    if (r->st.underline) pango_attr_list_insert(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    if (r->st.strikethrough) pango_attr_list_insert(al, pango_attr_strikethrough_new(TRUE));
    pango_layout_set_attributes(A.layout, al);
    pango_attr_list_unref(al);
    pango_layout_set_text(A.layout, r->txt, r->len);
    if (r->st.faint) cairo_set_source_rgba(cr, r->fg.r / 255.0, r->fg.g / 255.0, r->fg.b / 255.0, 0.6);
    else set_rgb(cr, r->fg);
    cairo_move_to(cr, r->x0 * A.cw, y * A.ch);
    pango_cairo_show_layout(cr, A.layout);
  }
  r->active = false;
  r->len = 0;
}

static gboolean on_draw(GtkWidget *w, cairo_t *wcr, gpointer ud) {
  (void)ud;
  gint64 draw_t0 = g_get_monotonic_time();
  int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
  if (!back || W != back_w || H != back_h) {
    if (back) cairo_surface_destroy(back);
    back = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    back_w = W; back_h = H; force_full = true;
  }
  cairo_t *cr = cairo_create(back);
  ghostty_render_state_update(A.rs, A.term);

  GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
  ghostty_render_state_get(A.rs, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
  static int always_full = -1;
  if (always_full < 0) always_full = g_getenv("SEANCE_FULL_REDRAW") != NULL;   // debug: disable partial redraw
  bool full = always_full || force_full || dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL;

  GhosttyRenderStateColors k = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
  ghostty_render_state_get(A.rs, GHOSTTY_RENDER_STATE_DATA_COLORS, &k);
  if (full) { set_rgb(cr, k.background); cairo_paint(cr); }

  GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
  ghostty_render_state_get(A.rs, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur);
  GhosttyColorRgb ccol = k.cursor_has_value ? k.cursor : k.foreground;
  bool block_cursor = cur.visible && cur.viewport_has_value &&
      (cur.visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK ||
       (int)cur.visual_style > (int)GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW);

  ghostty_render_state_get(A.rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &A.rows);
  int y = 0;
  int cur_y = (cur.visible && cur.viewport_has_value) ? cur.viewport_y : -1;
  while (ghostty_render_state_row_iterator_next(A.rows)) {
    bool row_dirty = false;
    ghostty_render_state_row_get(A.rows, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
    if (!full && !row_dirty && y != cur_y && y != prev_cy) { y++; continue; }
    cairo_save(cr);
    cairo_rectangle(cr, 0, y * A.ch, W, A.ch);
    cairo_clip(cr);
    if (!full) { set_rgb(cr, k.background); cairo_paint(cr); }
    ghostty_render_state_row_get(A.rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &A.cells);
    int x = 0;
    Run run = {0};
    while (ghostty_render_state_row_cells_next(A.cells)) {
      GhosttyStyle st = GHOSTTY_INIT_SIZED(GhosttyStyle);
      ghostty_render_state_row_cells_get(A.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &st);
      GhosttyColorRgb fg = resolve(st.fg_color, &k, k.foreground);
      GhosttyColorRgb bg = resolve(st.bg_color, &k, k.background);
      if (st.inverse) { GhosttyColorRgb t = fg; fg = bg; bg = t; }
      bool sel = false;
      ghostty_render_state_row_cells_get(A.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_SELECTED, &sel);
      if (sel) { GhosttyColorRgb t = fg; fg = bg; bg = t; }
      bool at_cursor = block_cursor && x == cur.viewport_x && y == cur.viewport_y;
      if (at_cursor) { bg = ccol; fg = k.background; }

      if (st.bg_color.tag != GHOSTTY_STYLE_COLOR_NONE || st.inverse || sel || at_cursor) {
        set_rgb(cr, bg);
        cairo_rectangle(cr, x * A.cw, y * A.ch, A.cw, A.ch);
        cairo_fill(cr);
      }

      uint32_t n = 0;
      ghostty_render_state_row_cells_get(A.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &n);
      uint32_t cps[32];
      if (n > 0) {
        if (n > 32) n = 32;
        ghostty_render_state_row_cells_get(A.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, cps);
      }
      bool blank = n == 0 || (n == 1 && cps[0] == ' ' && !st.underline && !st.strikethrough);
      if (blank || st.invisible) {
        // keep an active run contiguous across blanks (no ink, but advances one cell)
        if (run.active && run.len < (int)sizeof run.txt - 1) run.txt[run.len++] = ' ';
        else run_flush(cr, &run, y);
      } else {
        bool ascii1 = n == 1 && cps[0] > 0x20 && cps[0] < 0x7f;
        if (ascii1 && run.active && run_matches(&run, fg, &st) && run.len < (int)sizeof run.txt - 1) {
          run.txt[run.len++] = (char)cps[0];
        } else {
          run_flush(cr, &run, y);
          if (ascii1) {
            run.active = true; run.x0 = x; run.fg = fg; run.st = st;
            run.txt[0] = (char)cps[0]; run.len = 1;
          } else {   // non-ASCII / clusters / wide: one cell at a time
            char utf8[32 * 4 + 1];
            int len = 0;
            for (uint32_t i = 0; i < n; i++) len += g_unichar_to_utf8(cps[i], utf8 + len);
            Run one = {.active = true, .x0 = x, .fg = fg, .st = st, .len = 0};
            // reuse flush by putting the text into a temporary run-sized buffer
            g_strlcpy(one.txt, utf8, sizeof one.txt); one.len = (int)strlen(one.txt);
            run_flush(cr, &one, y);
          }
        }
      }
      x++;
    }
    run_flush(cr, &run, y);
    cairo_restore(cr);
    y++;
  }
  pango_layout_set_attributes(A.layout, NULL);

  if (cur.visible && cur.viewport_has_value && !block_cursor) {
    set_rgb(cr, ccol);
    double cx = cur.viewport_x * A.cw, cy = cur.viewport_y * A.ch;
    switch (cur.visual_style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
      cairo_rectangle(cr, cx, cy, 2, A.ch); cairo_fill(cr); break;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
      cairo_rectangle(cr, cx, cy + A.ch - 2, A.cw, 2); cairo_fill(cr); break;
    default:  // hollow
      cairo_set_line_width(cr, 1);
      cairo_rectangle(cr, cx + .5, cy + .5, A.cw - 1, A.ch - 1); cairo_stroke(cr); break;
    }
  }
  ghostty_render_state_clean(A.rs);
  force_full = false;
  prev_cy = cur_y;
  cairo_destroy(cr);
  cairo_set_source_surface(wcr, back, 0, 0);
  cairo_paint(wcr);
  gint64 draw_end = g_get_monotonic_time();
  st_draw_us += draw_end - draw_t0; st_draws++;
  avg_draw_us = (avg_draw_us * 3 + (draw_end - draw_t0)) / 4;
  last_draw_end_us = draw_end;
  return FALSE;
}

static GhosttyKey map_key(guint kv) {
  if (kv >= GDK_KEY_a && kv <= GDK_KEY_z) return GHOSTTY_KEY_A + (kv - GDK_KEY_a);
  if (kv >= GDK_KEY_A && kv <= GDK_KEY_Z) return GHOSTTY_KEY_A + (kv - GDK_KEY_A);
  if (kv >= GDK_KEY_F1 && kv <= GDK_KEY_F12) return GHOSTTY_KEY_F1 + (kv - GDK_KEY_F1);
  switch (kv) {
  case GDK_KEY_Return: case GDK_KEY_KP_Enter: return GHOSTTY_KEY_ENTER;
  case GDK_KEY_BackSpace: return GHOSTTY_KEY_BACKSPACE;
  case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return GHOSTTY_KEY_TAB;
  case GDK_KEY_Escape: return GHOSTTY_KEY_ESCAPE;
  case GDK_KEY_space: return GHOSTTY_KEY_SPACE;
  case GDK_KEY_Up: return GHOSTTY_KEY_ARROW_UP;
  case GDK_KEY_Down: return GHOSTTY_KEY_ARROW_DOWN;
  case GDK_KEY_Left: return GHOSTTY_KEY_ARROW_LEFT;
  case GDK_KEY_Right: return GHOSTTY_KEY_ARROW_RIGHT;
  case GDK_KEY_Home: return GHOSTTY_KEY_HOME;
  case GDK_KEY_End: return GHOSTTY_KEY_END;
  case GDK_KEY_Insert: return GHOSTTY_KEY_INSERT;
  case GDK_KEY_Delete: return GHOSTTY_KEY_DELETE;
  case GDK_KEY_Page_Up: return GHOSTTY_KEY_PAGE_UP;
  case GDK_KEY_Page_Down: return GHOSTTY_KEY_PAGE_DOWN;
  case GDK_KEY_minus: return GHOSTTY_KEY_MINUS;
  case GDK_KEY_period: return GHOSTTY_KEY_PERIOD;
  case GDK_KEY_slash: return GHOSTTY_KEY_SLASH;
  case GDK_KEY_semicolon: return GHOSTTY_KEY_SEMICOLON;
  case GDK_KEY_apostrophe: return GHOSTTY_KEY_QUOTE;
  case GDK_KEY_equal: return GHOSTTY_KEY_EQUAL;
  }
  if (kv >= GDK_KEY_0 && kv <= GDK_KEY_9) return GHOSTTY_KEY_DIGIT_0 + (kv - GDK_KEY_0);
  return GHOSTTY_KEY_UNIDENTIFIED;
}

static void paste_text(const char *text) {
  // libghostty encodes: strips unsafe control bytes, wraps for bracketed
  // paste (mode 2004) or converts LF->CR when the app isn't bracketed.
  GhosttyTerminalModeConfig m = {.mode = GHOSTTY_MODE_BRACKETED_PASTE};
  ghostty_terminal_get(A.term, GHOSTTY_TERMINAL_DATA_MODE, &m);
  size_t len = strlen(text);
  char *data = g_strdup(text);
  size_t cap = len * 2 + 32, n = 0;
  char *out = g_malloc(cap);
  if (ghostty_paste_encode(data, len, m.value, out, cap, &n) == GHOSTTY_SUCCESS)
    pty_write(out, n);
  g_free(data); g_free(out);
}

static void on_clipboard_text(GtkClipboard *c, const gchar *t, gpointer ud) {
  (void)c; (void)ud; if (t) paste_text(t);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer ud) {
  (void)w; (void)ud;
  bool press = e->type == GDK_KEY_PRESS;
  guint mods = e->state;

  // App shortcuts: Ctrl+Shift+C / V, Shift+PgUp/PgDn for scrollback.
  if (press && (mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK)) {
    if (e->keyval == GDK_KEY_C || e->keyval == GDK_KEY_c) { copy_selection_to(GDK_SELECTION_CLIPBOARD); return TRUE; }
    if (e->keyval == GDK_KEY_V || e->keyval == GDK_KEY_v) {
      gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), on_clipboard_text, NULL);
      return TRUE;
    }
  }
  if (press && (mods & GDK_CONTROL_MASK)) {
    if (e->keyval == GDK_KEY_plus || e->keyval == GDK_KEY_equal || e->keyval == GDK_KEY_KP_Add) {
      C.font_size = MIN(C.font_size + 1, 96); apply_font(); return TRUE;
    }
    if (e->keyval == GDK_KEY_minus || e->keyval == GDK_KEY_KP_Subtract) {
      C.font_size = MAX(C.font_size - 1, 4); apply_font(); return TRUE;
    }
    if (e->keyval == GDK_KEY_0 || e->keyval == GDK_KEY_KP_0) {
      C.font_size = 11; apply_font(); return TRUE;
    }
  }
  if (press && (mods & GDK_SHIFT_MASK) &&
      (e->keyval == GDK_KEY_Page_Up || e->keyval == GDK_KEY_Page_Down)) {
    GhosttyTerminalScrollViewport sv = {.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA};
    sv.value.delta = (e->keyval == GDK_KEY_Page_Up ? -1 : 1) * (intptr_t)(A.nrows / 2);
    ghostty_terminal_scroll_viewport(A.term, sv); force_full = true;
    gtk_widget_queue_draw(A.area);
    return TRUE;
  }

  GhosttyKey key = map_key(e->keyval);
  guint32 uc = gdk_keyval_to_unicode(e->keyval);
  char utf8[8] = {0};
  int ulen = 0;
  if (uc >= 0x20 && uc != 0x7f && !(mods & GDK_CONTROL_MASK)) ulen = g_unichar_to_utf8(uc, utf8);

  GhosttyMods gm = 0;
  if (mods & GDK_SHIFT_MASK) gm |= GHOSTTY_MODS_SHIFT;
  if (mods & GDK_CONTROL_MASK) gm |= GHOSTTY_MODS_CTRL;
  if (mods & GDK_MOD1_MASK) gm |= GHOSTTY_MODS_ALT;

  if (key == GHOSTTY_KEY_UNIDENTIFIED && ulen == 0) return FALSE;  // pure modifiers etc.

  ghostty_key_encoder_setopt_from_terminal(A.kenc, A.term);
  ghostty_key_event_set_action(A.kev, press ? GHOSTTY_KEY_ACTION_PRESS : GHOSTTY_KEY_ACTION_RELEASE);
  ghostty_key_event_set_key(A.kev, key);
  ghostty_key_event_set_mods(A.kev, gm);
  ghostty_key_event_set_consumed_mods(A.kev, (mods & GDK_SHIFT_MASK) && ulen ? GHOSTTY_MODS_SHIFT : 0);
  ghostty_key_event_set_utf8(A.kev, utf8, ulen);
  if (uc) ghostty_key_event_set_unshifted_codepoint(A.kev, gdk_unicode_to_keyval(uc) ? g_unichar_tolower(uc) : uc);

  char out[128];
  size_t n = 0;
  if (ghostty_key_encoder_encode(A.kenc, A.kev, out, sizeof out, &n) == GHOSTTY_SUCCESS && n > 0) {
    // Typing snaps the viewport back to the bottom.
    GhosttyTerminalScrollViewport sv = {.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM};
    ghostty_terminal_scroll_viewport(A.term, sv); force_full = true;
    pty_write(out, n);
  }
  return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer ud) {
  (void)w; (void)ud;
  int dir = e->direction == GDK_SCROLL_UP ? 1 : e->direction == GDK_SCROLL_DOWN ? -1 : 0;
  if (!dir) return TRUE;
  if (mouse_tracking() && !(e->state & GDK_SHIFT_MASK)) {
    send_mouse(GHOSTTY_MOUSE_ACTION_PRESS, dir > 0 ? 4 : 5, e->x, e->y, e->state);
    return TRUE;
  }
  GhosttyTerminalScrollViewport sv = {.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA};
  sv.value.delta = -dir * 3;
  ghostty_terminal_scroll_viewport(A.term, sv); force_full = true;
  force_full = true;
  gtk_widget_queue_draw(A.area);
  return TRUE;
}


// ---- mouse: selection, copy/paste, reporting to applications ----
static bool mouse_tracking(void) {
  bool t = false;
  ghostty_terminal_get(A.term, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &t);
  return t;
}

static void cell_at(double px, double py, int *cx, int *cy) {
  *cx = CLAMP((int)(px / A.cw), 0, A.ncols - 1);
  *cy = CLAMP((int)(py / A.ch), 0, A.nrows - 1);
}

static GhosttyMods gdk_mods(guint st) {
  GhosttyMods m = 0;
  if (st & GDK_SHIFT_MASK) m |= GHOSTTY_MODS_SHIFT;
  if (st & GDK_CONTROL_MASK) m |= GHOSTTY_MODS_CTRL;
  if (st & GDK_MOD1_MASK) m |= GHOSTTY_MODS_ALT;
  return m;
}

static GhosttyMouseButton gdk_button(guint b) {
  switch (b) {
  case 1: return GHOSTTY_MOUSE_BUTTON_LEFT;
  case 2: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
  case 3: return GHOSTTY_MOUSE_BUTTON_RIGHT;
  case 4: return GHOSTTY_MOUSE_BUTTON_FOUR;
  case 5: return GHOSTTY_MOUSE_BUTTON_FIVE;
  }
  return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
}

static void send_mouse(GhosttyMouseAction act, guint button, double x, double y, guint st) {
  ghostty_mouse_encoder_setopt_from_terminal(A.menc, A.term);
  bool any = A.btn_down != 0;
  ghostty_mouse_encoder_setopt(A.menc, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &any);
  ghostty_mouse_event_set_action(A.mev, act);
  if (button) ghostty_mouse_event_set_button(A.mev, gdk_button(button));
  else ghostty_mouse_event_clear_button(A.mev);
  ghostty_mouse_event_set_mods(A.mev, gdk_mods(st));
  ghostty_mouse_event_set_position(A.mev, (GhosttyMousePosition){(float)x, (float)y});
  char out[128]; size_t n = 0;
  if (ghostty_mouse_encoder_encode(A.menc, A.mev, out, sizeof out, &n) == GHOSTTY_SUCCESS && n > 0)
    pty_write(out, n);
}

static void apply_selection(void) {
  force_full = true;
  if (!A.have_sel) { ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_SELECTION, NULL); return; }
  GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
  sel.start = GHOSTTY_INIT_SIZED(GhosttyGridRef);
  sel.end = GHOSTTY_INIT_SIZED(GhosttyGridRef);
  GhosttyPoint a = {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = {A.sel_x0, A.sel_y0}}};
  GhosttyPoint b = {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = {A.sel_x1, A.sel_y1}}};
  if (ghostty_terminal_grid_ref(A.term, a, &sel.start) != GHOSTTY_SUCCESS) return;
  if (ghostty_terminal_grid_ref(A.term, b, &sel.end) != GHOSTTY_SUCCESS) return;
  ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_SELECTION, &sel);
}

// Returns a g_malloc'd NUL-terminated copy of the selection, or NULL.
static char *selection_text(void) {
  GhosttyTerminalSelectionFormatOptions o = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
  o.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN; o.unwrap = true; o.trim = true; o.selection = NULL;
  uint8_t *p = NULL; size_t len = 0;
  if (ghostty_terminal_selection_format_alloc(A.term, NULL, o, &p, &len) != GHOSTTY_SUCCESS || !p || !len) {
    if (p) ghostty_free(NULL, p, len);
    return NULL;
  }
  char *r = g_strndup((const char *)p, len);
  ghostty_free(NULL, p, len);
  return r;
}

static void copy_selection_to(GdkAtom which) {
  char *t = selection_text();
  if (!t) return;
  gtk_clipboard_set_text(gtk_clipboard_get(which), t, -1);
  g_free(t);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer ud) {
  (void)w; (void)ud;
  gtk_widget_grab_focus(A.area);
  bool press = e->type == GDK_BUTTON_PRESS;
  bool app = mouse_tracking() && !(e->state & GDK_SHIFT_MASK);
  if ((e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) && e->button == 1 && !app) {
    // double click = word, triple click = line
    int cx, cy; cell_at(e->x, e->y, &cx, &cy);
    GhosttyPoint pt = {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value = {.coordinate = {cx, cy}}};
    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_terminal_grid_ref(A.term, pt, &ref) == GHOSTTY_SUCCESS) {
      GhosttyResult r;
      if (e->type == GDK_2BUTTON_PRESS) {
        GhosttyTerminalSelectWordOptions o = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
        o.ref = ref;
        r = ghostty_terminal_select_word(A.term, &o, &sel);
      } else {
        GhosttyTerminalSelectLineOptions o = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
        o.ref = ref;
        r = ghostty_terminal_select_line(A.term, &o, &sel);
      }
      if (r == GHOSTTY_SUCCESS) {
        ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_SELECTION, &sel);
        A.dragging = false; A.have_sel = false;   // selection now owned by the terminal
        copy_selection_to(GDK_SELECTION_PRIMARY);
        gtk_widget_queue_draw(A.area);
      }
    }
    return TRUE;
  }
  if (e->type != GDK_BUTTON_PRESS && e->type != GDK_BUTTON_RELEASE) return TRUE;
  if (app) {
    A.btn_down = press ? (int)e->button : 0;
    send_mouse(press ? GHOSTTY_MOUSE_ACTION_PRESS : GHOSTTY_MOUSE_ACTION_RELEASE, e->button, e->x, e->y, e->state);
    return TRUE;
  }
  int cx, cy; cell_at(e->x, e->y, &cx, &cy);
  if (e->button == 1) {
    if (press) {
      A.dragging = true; A.have_sel = false; A.sel_x0 = A.sel_x1 = cx; A.sel_y0 = A.sel_y1 = cy;
      apply_selection();
    } else if (A.dragging) {
      A.dragging = false;
      if (A.have_sel) copy_selection_to(GDK_SELECTION_PRIMARY);
    }
  } else if (e->button == 2 && press) {
    gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), on_clipboard_text, NULL);
  }
  gtk_widget_queue_draw(A.area);
  return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer ud) {
  (void)w; (void)ud;
  bool app = mouse_tracking() && !(e->state & GDK_SHIFT_MASK);
  if (app) {
    send_mouse(GHOSTTY_MOUSE_ACTION_MOTION, A.btn_down, e->x, e->y, e->state);
    return TRUE;
  }
  if (A.dragging) {
    int cx, cy; cell_at(e->x, e->y, &cx, &cy);
    if (cx != A.sel_x1 || cy != A.sel_y1) {
      A.sel_x1 = cx; A.sel_y1 = cy;
      A.have_sel = (cx != A.sel_x0 || cy != A.sel_y0);
      apply_selection();
      gtk_widget_queue_draw(A.area);
    }
  }
  return TRUE;
}

// ---- config: Ghostty-style "key = value" file + Ghostty-format themes ----

static bool parse_hex(const char *v, GhosttyColorRgb *out) {
  if (*v == '#') v++;
  if (strlen(v) < 6) return false;
  unsigned r, g, b;
  if (sscanf(v, "%2x%2x%2x", &r, &g, &b) != 3) return false;
  out->r = r; out->g = g; out->b = b;
  return true;
}

static void cfg_apply_kv(const char *k, const char *v) {
  if (!strcmp(k, "font-family")) { g_free(C.font_family); C.font_family = g_strdup(v); }
  else if (!strcmp(k, "font-size")) { double d = g_ascii_strtod(v, NULL); if (d >= 4 && d <= 96) C.font_size = d; }
  else if (!strcmp(k, "theme")) { g_free(C.theme); C.theme = g_strdup(v); }
  else if (!strcmp(k, "foreground")) C.has_fg = parse_hex(v, &C.fg);
  else if (!strcmp(k, "background")) C.has_bg = parse_hex(v, &C.bg);
  else if (!strcmp(k, "cursor-color")) C.has_cursor = parse_hex(v, &C.cursor);
  else if (!strcmp(k, "scrollback-lines")) C.scrollback_lines = atoi(v);
  else if (!strcmp(k, "palette")) {
    int n; char hex[64];
    if (sscanf(v, "%d=%63s", &n, hex) == 2 && n >= 0 && n < 256 && parse_hex(hex, &C.pal[n])) C.pal_set[n] = true;
  }
  // unknown keys are ignored (Ghostty configs contain many we don't implement yet)
}

static void cfg_parse_file(const char *path, bool skip_theme) {
  gchar *text = NULL;
  if (!g_file_get_contents(path, &text, NULL, NULL)) return;
  gchar **lines = g_strsplit(text, "\n", -1);
  for (gchar **l = lines; *l; l++) {
    gchar *line = g_strstrip(*l);
    if (!*line || *line == '#') continue;
    gchar *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    gchar *k = g_strstrip(line), *v = g_strstrip(eq + 1);
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') { v[n - 1] = 0; v++; }
    if (skip_theme && !strcmp(k, "theme")) continue;
    cfg_apply_kv(k, v);
  }
  g_strfreev(lines);
  g_free(text);
}

static char *find_theme(const char *name) {
  if (strchr(name, '/')) return g_file_test(name, G_FILE_TEST_IS_REGULAR) ? g_strdup(name) : NULL;
  const char *dirs[] = {NULL, NULL, "/usr/share/seance/themes", "/usr/local/share/seance/themes",
                        "/usr/share/ghostty/themes", NULL};
  char *d0 = g_build_filename(g_get_user_config_dir(), "seance", "themes", NULL);
  char *d1 = g_build_filename(g_get_user_config_dir(), "ghostty", "themes", NULL);
  dirs[0] = d0; dirs[1] = d1;
  char *found = NULL;
  for (int i = 0; dirs[i] && !found; i++) {
    char *p = g_build_filename(dirs[i], name, NULL);
    if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) found = p; else g_free(p);
  }
  g_free(d0); g_free(d1);
  return found;
}

static void config_load(void) {
  const char *env = g_getenv("SEANCE_CONFIG");
  char *path = env ? g_strdup(env) : g_build_filename(g_get_user_config_dir(), "seance", "config", NULL);
  cfg_parse_file(path, false);
  if (C.theme) {
    char *tp = find_theme(C.theme);
    if (tp) { cfg_parse_file(tp, true); cfg_parse_file(path, true); }  // explicit config wins over theme
    else g_printerr("seance: theme '%s' not found\n", C.theme);
    g_free(tp);
  }
  g_free(path);
}

static void apply_colors(void) {
  if (C.has_fg) ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &C.fg);
  if (C.has_bg) ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &C.bg);
  if (C.has_cursor) ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &C.cursor);
  bool any = false;
  for (int i = 0; i < 256; i++) any |= C.pal_set[i];
  if (any) {
    GhosttyColorRgb pal[256];
    if (ghostty_terminal_get(A.term, GHOSTTY_TERMINAL_DATA_COLOR_PALETTE_DEFAULT, pal) == GHOSTTY_SUCCESS) {
      for (int i = 0; i < 256; i++) if (C.pal_set[i]) pal[i] = C.pal[i];
      ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, pal);
    }
  }
}

static void apply_font(void) {
  force_full = true;
  char *desc = g_strdup_printf("%s %g", C.font_family ? C.font_family : "Monospace", C.font_size);
  measure_cell(desc);
  g_free(desc);
  int w = gtk_widget_get_allocated_width(A.area), h = gtk_widget_get_allocated_height(A.area);
  if (w > 1 && h > 1) {
    A.ncols = 0;   // force resize propagation with the new cell size
    do_resize(w, h);
  }
  gtk_widget_queue_draw(A.area);
}

static void spawn(char **argv) {
  struct winsize ws = {.ws_row = A.nrows, .ws_col = A.ncols};
  A.child = forkpty(&A.pty, NULL, NULL, &ws);
  if (A.child == 0) {
    setenv("TERM", "xterm-256color", 1);   // SLES has no ghostty terminfo entry
    setenv("COLORTERM", "truecolor", 1);
    execvp(argv[0], argv);
    _exit(127);
  }
  fcntl(A.pty, F_SETFL, fcntl(A.pty, F_GETFL) | O_NONBLOCK);
  A.chan = g_io_channel_unix_new(A.pty);
  g_io_add_watch(A.chan, G_IO_IN | G_IO_HUP | G_IO_ERR, on_pty_readable, NULL);
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  const char *shell = g_getenv("SHELL");
  if (!shell) shell = "/bin/bash";
  char *default_argv[] = {(char *)shell, NULL};
  char **cmd = argc > 1 ? &argv[1] : default_argv;

  A.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(A.win), "seance");
  A.area = gtk_drawing_area_new();
  gtk_widget_set_can_focus(A.area, TRUE);
  gtk_widget_add_events(A.area, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_SCROLL_MASK |
                  GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  gtk_container_add(GTK_CONTAINER(A.win), A.area);

  A.layout = gtk_widget_create_pango_layout(A.area, NULL);
  {
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
    pango_cairo_context_set_font_options(pango_layout_get_context(A.layout), fo);
    cairo_font_options_destroy(fo);
  }
  config_load();
  if (g_getenv("SEANCE_FONT")) { g_free(C.font_family); C.font_family = g_strdup(g_getenv("SEANCE_FONT")); }
  {
    char *desc = g_strdup_printf("%s %g", C.font_family ? C.font_family : "Monospace", C.font_size);
    measure_cell(desc);
    g_free(desc);
  }
  A.ncols = 100; A.nrows = 30;
  gtk_window_set_default_size(GTK_WINDOW(A.win), A.ncols * A.cw, A.nrows * A.ch);

  ghostty_terminal_new(NULL, &A.term, A.ncols, A.nrows);
  ghostty_terminal_resize(A.term, A.ncols, A.nrows, A.cw, A.ch);
  ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_WRITE_PTY, on_write_pty);
  ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_BELL, on_bell);
  ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, on_title);
  apply_colors();
  size_t sb = C.scrollback_lines;
  ghostty_terminal_set(A.term, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &sb);
  ghostty_render_state_new(NULL, &A.rs);
  ghostty_render_state_row_iterator_new(NULL, &A.rows);
  ghostty_render_state_row_cells_new(NULL, &A.cells);
  ghostty_key_encoder_new(NULL, &A.kenc);
  ghostty_key_event_new(NULL, &A.kev);
  ghostty_mouse_encoder_new(NULL, &A.menc);
  ghostty_mouse_event_new(NULL, &A.mev);

  spawn(cmd);

  g_signal_connect(A.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  g_signal_connect(A.area, "draw", G_CALLBACK(on_draw), NULL);
  g_signal_connect(A.area, "size-allocate", G_CALLBACK(on_size), NULL);
  g_signal_connect(A.win, "key-press-event", G_CALLBACK(on_key), NULL);
  g_signal_connect(A.win, "key-release-event", G_CALLBACK(on_key), NULL);
  g_signal_connect(A.area, "scroll-event", G_CALLBACK(on_scroll), NULL);
  g_signal_connect(A.area, "button-press-event", G_CALLBACK(on_button), NULL);
  g_signal_connect(A.area, "button-release-event", G_CALLBACK(on_button), NULL);
  g_signal_connect(A.area, "motion-notify-event", G_CALLBACK(on_motion), NULL);

  gtk_widget_show_all(A.win);
  gtk_widget_grab_focus(A.area);
  gtk_main();

  if (g_getenv("SEANCE_STATS"))
    g_printerr("stats: %ld bytes in %ld reads (avg %ld B); vt_write %.2fs; %ld draws %.2fs\n", (long)st_bytes, (long)st_reads,
               st_reads ? (long)(st_bytes / st_reads) : 0, st_write_us / 1e6, (long)st_draws, st_draw_us / 1e6);
  kill(A.child, SIGHUP);
  waitpid(A.child, NULL, 0);
  ghostty_mouse_event_free(A.mev);
  ghostty_mouse_encoder_free(A.menc);
  ghostty_key_event_free(A.kev);
  ghostty_key_encoder_free(A.kenc);
  ghostty_render_state_row_cells_free(A.cells);
  ghostty_render_state_row_iterator_free(A.rows);
  ghostty_render_state_free(A.rs);
  ghostty_terminal_free(A.term);
  return 0;
}

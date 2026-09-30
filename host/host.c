// seance-host: GTK3 host for the full Ghostty core (libghostty embedded API, Linux platform patch).
// The core owns terminal, fonts, config, keybinds and rendering (offscreen EGL / Mesa llvmpipe).
// This host owns windows, tabs, splits, input, clipboard, and paints CPU frames the core hands back.
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <signal.h>
#include <gdk/gdkkeysyms.h>
#include <stdlib.h>
#include <string.h>
#include <ghostty.h>
#define SEANCE_VERSION "0.1.2"
#include <stdarg.h>
#include <execinfo.h>
#include <glib/gstdio.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gio/gunixsocketaddress.h>

typedef struct Pane {
  GtkWidget *area;
  ghostty_surface_t surface;
  cairo_surface_t *img;     // last frame (ARGB32, device pixels)
  int img_w, img_h;
  GtkIMContext *im;         // dead keys / compose / IMEs
  char *title;
  gboolean closing;
  gboolean swallow_rclick;
  int id;                   // stable id used by the control socket
  guint64 sb_total, sb_off, sb_len;   // scrollbar state reported by the core
} Pane;

typedef struct {
  GtkWidget *win, *nb;
  ghostty_app_t app;
  Pane *focus;
  ghostty_config_t cfg;     // current config (replaced on reload)
  long frames, frame_us;    // SEANCE_STATS=1
} Host;

static Host H;
static GList *all_panes;      // Pane*
static int next_pane_id = 1;
static char *sock_path;       // control socket (exported to children as SEANCE_SOCKET)
static void emit_event(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static char *next_pane_command;   // one-shot command for the next pane_new (e.g. open the config in $EDITOR)
static Pane *pane_new(Pane *inherit_from, ghostty_surface_context_e ctx);
static void pane_close_now(Pane *p);

// ---------------------------------------------------------------- app wakeup / ticks
static volatile gint tick_pending;

static gboolean tick_cb(gpointer ud) {
  (void)ud;
  g_atomic_int_set(&tick_pending, 0);
  if (H.app) ghostty_app_tick(H.app);
  return G_SOURCE_REMOVE;
}

// Called from any thread. Ticks run at idle priority (lower than GDK's redraw priority) and are
// coalesced, so a flood of output can't starve repaints; input events (default priority) still win.
static void on_wakeup(void *ud) {
  (void)ud;
  if (g_atomic_int_compare_and_exchange(&tick_pending, 0, 1))
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, tick_cb, NULL, NULL);
}

static int scale_factor_of(GtkWidget *w) {
  int sf = w ? gtk_widget_get_scale_factor(w) : 1;
  return sf > 0 ? sf : 1;
}

// Never dereference a surface pointer from a core callback: messages queued before a pane was freed can still name it.
// Match against the live pane list instead.
static Pane *pane_of_surface(ghostty_surface_t s) {
  if (!s) return NULL;
  for (GList *l = all_panes; l; l = l->next) if (((Pane *)l->data)->surface == s && !((Pane *)l->data)->closing) return l->data;
  return NULL;
}
static Pane *live_pane(void *ud) { return (ud && g_list_find(all_panes, ud)) ? (Pane *)ud : NULL; }

// Make p the keyboard-focus pane. Done explicitly (not only from GTK focus events) so it also works
// without a window manager / X input focus.
static GtkWidget *tab_root_of(Pane *p);
static void set_focus_pane(Pane *p) {
  if (!p || p->closing) return;
  if (H.focus && H.focus != p && !H.focus->closing) ghostty_surface_set_focus(H.focus->surface, false);
  H.focus = p;
  { GtkWidget *rt = tab_root_of(p); if (rt) g_object_set_data(G_OBJECT(rt), "last-focus", p); }
  emit_event("focus pane=%d", p->id);
  ghostty_surface_set_focus(p->surface, true);
  gtk_widget_grab_focus(p->area);
  if (p->title) gtk_window_set_title(GTK_WINDOW(H.win), p->title);
}

// ---------------------------------------------------------------- tree helpers
static void collect_panes(GtkWidget *w, GList **out) {
  if (!w) return;
  if (GTK_IS_PANED(w)) {
    collect_panes(gtk_paned_get_child1(GTK_PANED(w)), out);
    collect_panes(gtk_paned_get_child2(GTK_PANED(w)), out);
  } else {
    Pane *p = g_object_get_data(G_OBJECT(w), "pane");
    if (p) *out = g_list_append(*out, p);
  }
}

static GtkWidget *tab_root_of(Pane *p) {
  GtkWidget *w = p->area;
  while (w && gtk_widget_get_parent(w) != H.nb) w = gtk_widget_get_parent(w);
  return w;
}

static void update_tabs_visible(void) {
  const char *mode = "auto";   // Ghostty option window-show-tab-bar: auto | always | never
  const char *v = NULL;
  if (H.cfg && ghostty_config_get(H.cfg, &v, "window-show-tab-bar", 19) && v) mode = v;
  gboolean show = !strcmp(mode, "always") || (!strcmp(mode, "auto") && gtk_notebook_get_n_pages(GTK_NOTEBOOK(H.nb)) > 1);
  gtk_notebook_set_show_tabs(GTK_NOTEBOOK(H.nb), show);
}

// ---------------------------------------------------------------- theme (colors come from the Ghostty config)
static double th_bg[3] = {0.157, 0.173, 0.204}, th_fg[3] = {0.86, 0.86, 0.86}, th_accent[3] = {0.48, 0.64, 0.97};
static double th_opacity = 1.0;
static gboolean composited_ok;          // RGBA visual + running compositor: translucency can show
static GtkCssProvider *css_provider;

static gboolean cfg_color(const char *name, double out[3]) {
  ghostty_config_color_s c;
  if (!H.cfg || !ghostty_config_get(H.cfg, &c, name, strlen(name))) return FALSE;
  out[0] = c.r / 255.0; out[1] = c.g / 255.0; out[2] = c.b / 255.0;
  return TRUE;
}

static void mix3(const double a[3], const double b[3], double t, double out[3]) {
  for (int i = 0; i < 3; i++) out[i] = a[i] * (1 - t) + b[i] * t;
}

static void css_rgba(const double c[3], double a, char *out, size_t n) {
  g_snprintf(out, n, "rgba(%d,%d,%d,%.3f)", (int)(c[0] * 255 + .5), (int)(c[1] * 255 + .5), (int)(c[2] * 255 + .5), a);
}

static void apply_theme(void) {
  cfg_color("background", th_bg);
  cfg_color("foreground", th_fg);
  double op = 1.0;
  if (H.cfg && ghostty_config_get(H.cfg, &op, "background-opacity", 18)) th_opacity = CLAMP(op, 0.0, 1.0);
  ghostty_config_palette_s pal;
  if (H.cfg && ghostty_config_get(H.cfg, &pal, "palette", 7)) {   // accent = ANSI blue
    th_accent[0] = pal.colors[4].r / 255.0; th_accent[1] = pal.colors[4].g / 255.0; th_accent[2] = pal.colors[4].b / 255.0;
  }
  gboolean translucent = composited_ok && th_opacity < 1.0;
  double lum = 0.2126 * th_bg[0] + 0.7152 * th_bg[1] + 0.0722 * th_bg[2];
  double bar_c[3], hover_c[3], div_c[3], dim_c[3];
  mix3(th_bg, th_fg, lum < 0.5 ? 0.07 : 0.06, bar_c);      // tab strip: slightly lifted / dimmed background
  mix3(th_bg, th_fg, 0.14, hover_c);
  mix3(th_bg, th_fg, 0.22, div_c);
  mix3(th_bg, th_fg, 0.62, dim_c);
  double bar_a = translucent ? 0.55 + 0.45 * th_opacity : 1.0;
  char bg[48], fg[48], bar[48], hover[48], div[48], dim[48], acc[48], winbg[48];
  css_rgba(th_bg, translucent ? th_opacity : 1.0, bg, sizeof bg);
  css_rgba(th_fg, 1.0, fg, sizeof fg);
  css_rgba(bar_c, bar_a, bar, sizeof bar);
  css_rgba(hover_c, bar_a, hover, sizeof hover);
  css_rgba(div_c, 1.0, div, sizeof div);
  css_rgba(dim_c, 1.0, dim, sizeof dim);
  css_rgba(th_accent, 1.0, acc, sizeof acc);
  g_snprintf(winbg, sizeof winbg, "%s", translucent ? "transparent" : bg);
  gchar *css = g_strdup_printf(
      "window, .background { background-color: %s; }\n"
      "notebook, notebook > stack { background-color: transparent; }\n"
      "notebook > header { background-color: %s; border: none; box-shadow: none; padding: 0; }\n"
      "notebook > header > tabs > tab { background: none; background-color: %s; color: %s; border: none; border-radius: 0;"
      " padding: 2px 6px; min-height: 24px; min-width: 104px; margin: 0; box-shadow: none; outline: none; }\n"
      "notebook > header > tabs > tab:hover { background-color: %s; color: %s; }\n"
      "notebook > header > tabs > tab:checked { background-color: %s; color: %s; box-shadow: inset 0 -2px %s; }\n"
      "notebook > header > tabs > tab label { color: inherit; }\n"
      "button.seance-tab-close, button.seance-newtab { background: none; border: none; box-shadow: none; text-shadow: none;"
      " padding: 0 6px; min-height: 0; min-width: 0; color: %s; border-radius: 4px; }\n"
      "button.seance-tab-close:hover, button.seance-newtab:hover { background-color: %s; color: %s; }\n"
      "paned > separator { background-image: none; background-color: %s; min-width: 1px; min-height: 1px; }\n"
      "paned.horizontal > separator { min-width: 5px; background-color: transparent;"
      " background-image: linear-gradient(to right, transparent 2px, %s 2px, %s 3px, transparent 3px); }\n"
      "paned.vertical > separator { min-height: 5px; background-color: transparent;"
      " background-image: linear-gradient(to bottom, transparent 2px, %s 2px, %s 3px, transparent 3px); }\n",
      winbg, bar, bar, dim, hover, fg, translucent ? bg : bg, fg, acc, dim, hover, fg, div, div, div, div, div);
  if (!css_provider) {
    css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  if (!g_getenv("SEANCE_NO_CSS")) gtk_css_provider_load_from_data(css_provider, css, -1, NULL);   // debug switch
  g_free(css);
  g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", lum < 0.5, NULL);
  for (GList *l = all_panes; l; l = l->next) gtk_widget_queue_draw(((Pane *)l->data)->area);
}

// ---------------------------------------------------------------- tab widgets
static gboolean deferred_close_cb(gpointer ud);
static void add_tab(Pane *p);

static char *tab_title_for(GtkWidget *root) {
  const char *custom = g_object_get_data(G_OBJECT(root), "custom-title");
  if (custom && *custom) return g_strdup(custom);
  GList *panes = NULL;
  collect_panes(root, &panes);
  Pane *first = panes ? panes->data : NULL;   // an unnamed tab shows its first pane's title
  g_list_free(panes);
  return g_strdup(first && first->title && *first->title ? first->title : "seance");
}

static void refresh_tab_label(GtkWidget *root) {
  GtkWidget *lbl = g_object_get_data(G_OBJECT(root), "title-label");
  if (!lbl) return;
  char *t = tab_title_for(root);
  gtk_label_set_text(GTK_LABEL(lbl), t);
  gtk_widget_set_tooltip_text(lbl, t);
  g_free(t);
}

static void set_tab_title(Pane *p) {
  GtkWidget *root = tab_root_of(p);
  if (root) refresh_tab_label(root);
}

// When the user switches tabs (click, drag, shortcut), keyboard focus must follow to that tab's pane.
static gboolean switch_focus_idle(gpointer root) {
  GtkWidget *r = GTK_WIDGET(root);
  if (gtk_notebook_page_num(GTK_NOTEBOOK(H.nb), r) >= 0) {
    GList *panes = NULL;
    collect_panes(r, &panes);
    Pane *last = g_object_get_data(G_OBJECT(r), "last-focus");
    Pane *target = (last && g_list_find(panes, last)) ? last : (panes ? panes->data : NULL);
    g_list_free(panes);
    if (target && target != H.focus) set_focus_pane(target);
  }
  g_object_unref(r);
  return G_SOURCE_REMOVE;
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud) {
  (void)nb; (void)num; (void)ud;
  g_idle_add(switch_focus_idle, g_object_ref(page));
}

static void close_root(GtkWidget *root) {
  GList *panes = NULL;
  collect_panes(root, &panes);
  for (GList *l = panes; l; l = l->next) ((Pane *)l->data)->closing = TRUE;
  for (GList *l = panes; l; l = l->next) g_idle_add(deferred_close_cb, l->data);
  g_list_free(panes);
}

static void tab_move(GtkWidget *root, int delta) {
  int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(H.nb));
  int cur = gtk_notebook_page_num(GTK_NOTEBOOK(H.nb), root);
  if (n < 2 || cur < 0) return;
  gtk_notebook_reorder_child(GTK_NOTEBOOK(H.nb), root, ((cur + delta) % n + n) % n);
}

static void rename_tab_dialog(GtkWidget *root) {
  GtkWidget *d = gtk_dialog_new_with_buttons("Rename tab", GTK_WINDOW(H.win), GTK_DIALOG_MODAL, "_Cancel",
                                             GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
  GtkWidget *e = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
  const char *cur = g_object_get_data(G_OBJECT(root), "custom-title");
  gtk_entry_set_text(GTK_ENTRY(e), cur ? cur : "");
  gtk_entry_set_placeholder_text(GTK_ENTRY(e), "empty = follow the shell title");
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), e, TRUE, TRUE, 8);
  gtk_widget_show_all(d);
  if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
    const char *t = gtk_entry_get_text(GTK_ENTRY(e));
    g_object_set_data_full(G_OBJECT(root), "custom-title", *t ? g_strdup(t) : NULL, g_free);
    refresh_tab_label(root);
  }
  gtk_widget_destroy(d);
  if (gtk_notebook_page_num(GTK_NOTEBOOK(H.nb), root) >= 0) {
    GList *panes = NULL; collect_panes(root, &panes);
    if (panes) gtk_widget_grab_focus(((Pane *)panes->data)->area);
    g_list_free(panes);
  }
}

static gboolean rename_idle(gpointer root) {
  if (gtk_notebook_page_num(GTK_NOTEBOOK(H.nb), GTK_WIDGET(root)) >= 0) rename_tab_dialog(GTK_WIDGET(root));
  g_object_unref(root);
  return G_SOURCE_REMOVE;
}

static void on_menu_new(GtkMenuItem *mi, gpointer ud) { (void)mi; (void)ud; Pane *q = pane_new(H.focus, GHOSTTY_SURFACE_CONTEXT_TAB); if (q) add_tab(q); }
static void on_menu_rename(GtkMenuItem *mi, gpointer root) { (void)mi; g_idle_add(rename_idle, g_object_ref(root)); }
static void on_menu_left(GtkMenuItem *mi, gpointer root) { (void)mi; tab_move(GTK_WIDGET(root), -1); }
static void on_menu_right(GtkMenuItem *mi, gpointer root) { (void)mi; tab_move(GTK_WIDGET(root), +1); }
static void on_menu_close(GtkMenuItem *mi, gpointer root) { (void)mi; close_root(GTK_WIDGET(root)); }
static void on_menu_close_others(GtkMenuItem *mi, gpointer root) {
  (void)mi;
  int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(H.nb));
  for (int i = 0; i < n; i++) {
    GtkWidget *r = gtk_notebook_get_nth_page(GTK_NOTEBOOK(H.nb), i);
    if (r != GTK_WIDGET(root)) close_root(r);
  }
}

static gboolean destroy_idle(gpointer w) { gtk_widget_destroy(GTK_WIDGET(w)); g_object_unref(w); return G_SOURCE_REMOVE; }

// Destroy the menu after it closes, but not inside "deactivate" (item "activate" fires after it).
static void on_menu_deactivate(GtkMenuShell *m, gpointer ud) { (void)ud; g_idle_add(destroy_idle, g_object_ref(m)); }

static void tab_menu_popup(GtkWidget *anchor, GtkWidget *root, GdkEventButton *e) {
  GtkWidget *m = gtk_menu_new();
  struct { const char *label; GCallback cb; } items[] = {
      {"New Tab", G_CALLBACK(on_menu_new)}, {"Rename Tab…", G_CALLBACK(on_menu_rename)}, {NULL, NULL},
      {"Move Left", G_CALLBACK(on_menu_left)}, {"Move Right", G_CALLBACK(on_menu_right)}, {NULL, NULL},
      {"Close Tab", G_CALLBACK(on_menu_close)}, {"Close Other Tabs", G_CALLBACK(on_menu_close_others)}};
  for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
    GtkWidget *mi = items[i].label ? gtk_menu_item_new_with_label(items[i].label) : gtk_separator_menu_item_new();
    if (items[i].cb) g_signal_connect(mi, "activate", items[i].cb, root);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mi);
  }
  g_signal_connect(m, "deactivate", G_CALLBACK(on_menu_deactivate), NULL);
  gtk_widget_show_all(m);
  // Anchored below the tab, so it doesn't depend on pointer-device lookups (robust without a WM / with XTest events).
  gtk_menu_popup_at_widget(GTK_MENU(m), anchor, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, (GdkEvent *)e);
}

static gboolean on_tab_press(GtkWidget *w, GdkEventButton *e, gpointer root) {
  if (e->type != GDK_BUTTON_PRESS) return FALSE;
  if (e->button == 2) { close_root(GTK_WIDGET(root)); return TRUE; }        // middle-click closes
  if (e->button == 3) { tab_menu_popup(w, GTK_WIDGET(root), e); return TRUE; } // right-click menu
  return FALSE;                                                              // left: notebook switches/drags
}

static void on_tab_close_clicked(GtkButton *b, gpointer root) { (void)b; close_root(GTK_WIDGET(root)); }

static GtkWidget *make_tab_label(GtkWidget *root) {
  GtkWidget *ev = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(ev), FALSE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  GtkWidget *lbl = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(lbl), 24);
  GtkWidget *btn = gtk_button_new_with_label("×");
  gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click(btn, FALSE);
  gtk_widget_set_tooltip_text(btn, "Close tab");
  gtk_style_context_add_class(gtk_widget_get_style_context(btn), "seance-tab-close");
  gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(ev), box);
  g_object_set_data(G_OBJECT(root), "title-label", lbl);
  g_signal_connect(ev, "button-press-event", G_CALLBACK(on_tab_press), root);
  g_signal_connect(btn, "clicked", G_CALLBACK(on_tab_close_clicked), root);
  gtk_widget_show_all(ev);
  return ev;
}

// Put `root` (a pane area or a paned tree) into the notebook at idx (-1 = append) with a themed tab label.
static int install_tab(GtkWidget *root, int idx, char *custom_title /* takes ownership, may be NULL */) {
  if (custom_title) g_object_set_data_full(G_OBJECT(root), "custom-title", custom_title, g_free);
  GtkWidget *lab = make_tab_label(root);
  int i = idx < 0 ? gtk_notebook_append_page(GTK_NOTEBOOK(H.nb), root, lab)
                  : gtk_notebook_insert_page(GTK_NOTEBOOK(H.nb), root, lab, idx);
  gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(H.nb), root, TRUE);
  refresh_tab_label(root);
  return i;
}

// ---------------------------------------------------------------- split management (resize / equalize / zoom)
static GtkWidget *ancestor_paned(GtkWidget *w, GtkOrientation o) {
  for (GtkWidget *x = gtk_widget_get_parent(w); x && x != H.nb; x = gtk_widget_get_parent(x))
    if (GTK_IS_PANED(x) && gtk_orientable_get_orientation(GTK_ORIENTABLE(x)) == o) return x;
  return NULL;
}

static void resize_split(Pane *p, ghostty_action_resize_split_direction_e dir, int amount) {
  GtkOrientation o = (dir == GHOSTTY_RESIZE_SPLIT_LEFT || dir == GHOSTTY_RESIZE_SPLIT_RIGHT) ? GTK_ORIENTATION_HORIZONTAL
                                                                                              : GTK_ORIENTATION_VERTICAL;
  GtkWidget *pd = ancestor_paned(p->area, o);
  if (!pd) return;
  int d = (dir == GHOSTTY_RESIZE_SPLIT_RIGHT || dir == GHOSTTY_RESIZE_SPLIT_DOWN) ? amount : -amount;
  gtk_paned_set_position(GTK_PANED(pd), MAX(20, gtk_paned_get_position(GTK_PANED(pd)) + d));
}

static int leaf_count(GtkWidget *w) {
  if (!w) return 0;
  if (GTK_IS_PANED(w)) return leaf_count(gtk_paned_get_child1(GTK_PANED(w))) + leaf_count(gtk_paned_get_child2(GTK_PANED(w)));
  return 1;
}

static void equalize_tree(GtkWidget *w) {
  if (!w || !GTK_IS_PANED(w)) return;
  GtkWidget *a = gtk_paned_get_child1(GTK_PANED(w)), *b = gtk_paned_get_child2(GTK_PANED(w));
  int na = leaf_count(a), nb = leaf_count(b);
  gboolean horiz = gtk_orientable_get_orientation(GTK_ORIENTABLE(w)) == GTK_ORIENTATION_HORIZONTAL;
  int total = horiz ? gtk_widget_get_allocated_width(w) : gtk_widget_get_allocated_height(w);
  if (na + nb > 0 && total > 40) gtk_paned_set_position(GTK_PANED(w), total * na / (na + nb));
  equalize_tree(a);
  equalize_tree(b);
}

static void show_tree(GtkWidget *w) {
  if (!w) return;
  gtk_widget_show(w);
  if (GTK_IS_PANED(w)) { show_tree(gtk_paned_get_child1(GTK_PANED(w))); show_tree(gtk_paned_get_child2(GTK_PANED(w))); }
}

static void unzoom_root(GtkWidget *root) {
  if (root && g_object_get_data(G_OBJECT(root), "zoomed")) {
    show_tree(root);
    g_object_set_data(G_OBJECT(root), "zoomed", NULL);
  }
}

static void toggle_zoom(Pane *p) {
  GtkWidget *root = tab_root_of(p);
  if (!root) return;
  if (g_object_get_data(G_OBJECT(root), "zoomed")) { unzoom_root(root); return; }
  if (!GTK_IS_PANED(root)) return;
  for (GtkWidget *w = p->area, *par = gtk_widget_get_parent(w); par && par != H.nb; w = par, par = gtk_widget_get_parent(par)) {
    if (!GTK_IS_PANED(par)) continue;
    GtkWidget *other = gtk_paned_get_child1(GTK_PANED(par)) == w ? gtk_paned_get_child2(GTK_PANED(par)) : gtk_paned_get_child1(GTK_PANED(par));
    if (other) gtk_widget_hide(other);
  }
  g_object_set_data(G_OBJECT(root), "zoomed", p);
}

static void focus_pane(Pane *p) {
  if (!p) return;
  GtkWidget *root = tab_root_of(p);
  if (root) gtk_notebook_set_current_page(GTK_NOTEBOOK(H.nb), gtk_notebook_page_num(GTK_NOTEBOOK(H.nb), root));
  set_focus_pane(p);
}

// ---------------------------------------------------------------- frames -> cairo
static void take_frame(Pane *p) {
  ghostty_frame_s f;
  if (!ghostty_surface_take_frame(p->surface, &f)) return;
  gint64 t0 = g_get_monotonic_time();
  if (!p->img || p->img_w != (int)f.width || p->img_h != (int)f.height) {
    if (p->img) cairo_surface_destroy(p->img);
    p->img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, f.width, f.height);
    p->img_w = f.width; p->img_h = f.height;
    cairo_surface_set_device_scale(p->img, scale_factor_of(p->area), scale_factor_of(p->area));
  }
  cairo_surface_flush(p->img);
  uint8_t *dst = cairo_image_surface_get_data(p->img);
  int dstride = cairo_image_surface_get_stride(p->img);
  for (uint32_t y = 0; y < f.height; y++) {
    const uint32_t *src = (const uint32_t *)(f.pixels + (size_t)(f.height - 1 - y) * f.stride);   // GL is bottom-up
    uint32_t *out = (uint32_t *)(dst + (size_t)y * dstride);
    for (uint32_t x = 0; x < f.width; x++) {
      uint32_t px = src[x];                                                      // R,G,B,A bytes
      out[x] = (px & 0xFF00FF00u) | ((px & 0xFFu) << 16) | ((px >> 16) & 0xFFu);   // -> B,G,R,A
    }
  }
  cairo_surface_mark_dirty(p->img);
  ghostty_surface_release_frame(p->surface, f.handle);
  H.frames++; H.frame_us += g_get_monotonic_time() - t0;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
  Pane *p = ud;
  if (p->closing) return FALSE;
  take_frame(p);
  gboolean translucent = th_opacity < 1.0;
  if (translucent && composited_ok) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);      // replace pixels so alpha reaches the compositor
  } else if (translucent) {                             // no compositor: flatten over the theme background
    cairo_set_source_rgb(cr, th_bg[0], th_bg[1], th_bg[2]);
    cairo_paint(cr);
  }
  if (p->img) cairo_set_source_surface(cr, p->img, 0, 0);
  else cairo_set_source_rgba(cr, th_bg[0], th_bg[1], th_bg[2], composited_ok ? th_opacity : 1.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  // overlay scrollbar (thumb only; wheel/keys scroll)
  if (p->sb_len && p->sb_total > p->sb_len) {
    double W = gtk_widget_get_allocated_width(w), Hh = gtk_widget_get_allocated_height(w);
    double th = MAX(24.0, Hh * (double)p->sb_len / (double)p->sb_total);
    double range = (double)(p->sb_total - p->sb_len);
    double ty = (Hh - th) * (range > 0 ? (double)p->sb_off / range : 0.0);
    double x = W - 7, wdt = 4, r = 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + r, ty + r, r, G_PI, 1.5 * G_PI);
    cairo_arc(cr, x + wdt - r, ty + r, r, 1.5 * G_PI, 0);
    cairo_arc(cr, x + wdt - r, ty + th - r, r, 0, 0.5 * G_PI);
    cairo_arc(cr, x + r, ty + th - r, r, 0.5 * G_PI, G_PI);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, th_fg[0], th_fg[1], th_fg[2], 0.28);
    cairo_fill(cr);
  }
  return FALSE;
}

static void on_size(GtkWidget *w, GtkAllocation *al, gpointer ud) {
  Pane *p = ud;
  int sf = scale_factor_of(w);
  if (p->surface && al->width > 0 && al->height > 0) ghostty_surface_set_size(p->surface, al->width * sf, al->height * sf);
}

// ---------------------------------------------------------------- actions
static const char *cursor_name(ghostty_action_mouse_shape_e sh) {
  switch (sh) {
  case GHOSTTY_MOUSE_SHAPE_TEXT: return "text";
  case GHOSTTY_MOUSE_SHAPE_VERTICAL_TEXT: return "vertical-text";
  case GHOSTTY_MOUSE_SHAPE_POINTER: return "pointer";
  case GHOSTTY_MOUSE_SHAPE_CROSSHAIR: return "crosshair";
  case GHOSTTY_MOUSE_SHAPE_WAIT: return "wait";
  case GHOSTTY_MOUSE_SHAPE_PROGRESS: return "progress";
  case GHOSTTY_MOUSE_SHAPE_HELP: return "help";
  case GHOSTTY_MOUSE_SHAPE_CELL: return "cell";
  case GHOSTTY_MOUSE_SHAPE_MOVE: return "move";
  case GHOSTTY_MOUSE_SHAPE_COPY: return "copy";
  case GHOSTTY_MOUSE_SHAPE_ALIAS: return "alias";
  case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED: case GHOSTTY_MOUSE_SHAPE_NO_DROP: return "not-allowed";
  case GHOSTTY_MOUSE_SHAPE_GRAB: return "grab";
  case GHOSTTY_MOUSE_SHAPE_GRABBING: return "grabbing";
  case GHOSTTY_MOUSE_SHAPE_COL_RESIZE: case GHOSTTY_MOUSE_SHAPE_EW_RESIZE: return "col-resize";
  case GHOSTTY_MOUSE_SHAPE_ROW_RESIZE: case GHOSTTY_MOUSE_SHAPE_NS_RESIZE: return "row-resize";
  default: return "default";
  }
}

static void notify_desktop(const char *title, const char *body) {
  // no GApplication here; use notify-send if the VM has it (libnotify-tools). Best effort.
  gchar *argv[] = {"notify-send", "-a", "seance", (gchar *)(title && *title ? title : "seance"), (gchar *)(body ? body : ""), NULL};
  g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                NULL, NULL, NULL, NULL);
}

static void paned_half_once(GtkWidget *w, GtkAllocation *al, gpointer ud) {
  (void)ud;
  if (g_object_get_data(G_OBJECT(w), "halved")) return;
  gboolean horiz = gtk_orientable_get_orientation(GTK_ORIENTABLE(w)) == GTK_ORIENTATION_HORIZONTAL;
  int total = horiz ? al->width : al->height;
  if (total < 20) return;
  gtk_paned_set_position(GTK_PANED(w), total / 2);
  g_object_set_data(G_OBJECT(w), "halved", GINT_TO_POINTER(1));
}

static void add_tab(Pane *p) {
  int idx = install_tab(p->area, -1, NULL);
  gtk_widget_show_all(p->area);
  update_tabs_visible();
  gtk_notebook_set_current_page(GTK_NOTEBOOK(H.nb), idx);
  set_focus_pane(p);
}

static void split_pane(Pane *p, ghostty_action_split_direction_e dir) {
  Pane *q = pane_new(p, GHOSTTY_SURFACE_CONTEXT_SPLIT);
  if (!q) return;
  gboolean horiz = dir == GHOSTTY_SPLIT_DIRECTION_RIGHT || dir == GHOSTTY_SPLIT_DIRECTION_LEFT;
  gboolean q_first = dir == GHOSTTY_SPLIT_DIRECTION_LEFT || dir == GHOSTTY_SPLIT_DIRECTION_UP;
  GtkWidget *paned = gtk_paned_new(horiz ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
  g_signal_connect(paned, "size-allocate", G_CALLBACK(paned_half_once), NULL);

  GtkWidget *parent = gtk_widget_get_parent(p->area);
  g_object_ref(p->area);
  if (GTK_IS_NOTEBOOK(parent)) {
    gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(parent), p->area);
    char *custom = g_strdup(g_object_get_data(G_OBJECT(p->area), "custom-title"));
    gtk_notebook_remove_page(GTK_NOTEBOOK(parent), idx);
    install_tab(paned, idx, custom);
  } else {  // parent is a paned: keep p's slot
    gboolean first = gtk_paned_get_child1(GTK_PANED(parent)) == p->area;
    gtk_container_remove(GTK_CONTAINER(parent), p->area);
    if (first) gtk_paned_pack1(GTK_PANED(parent), paned, TRUE, TRUE);
    else gtk_paned_pack2(GTK_PANED(parent), paned, TRUE, TRUE);
  }
  if (q_first) { gtk_paned_pack1(GTK_PANED(paned), q->area, TRUE, TRUE); gtk_paned_pack2(GTK_PANED(paned), p->area, TRUE, TRUE); }
  else { gtk_paned_pack1(GTK_PANED(paned), p->area, TRUE, TRUE); gtk_paned_pack2(GTK_PANED(paned), q->area, TRUE, TRUE); }
  g_object_unref(p->area);
  gtk_widget_show_all(paned);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(H.nb), gtk_notebook_page_num(GTK_NOTEBOOK(H.nb), tab_root_of(q)));
  set_focus_pane(q);
  set_tab_title(q);
}

static void goto_split(Pane *p, ghostty_action_goto_split_e where) {
  GtkWidget *root = tab_root_of(p);
  GList *panes = NULL;
  collect_panes(root, &panes);
  int n = g_list_length(panes);
  if (n < 2) { g_list_free(panes); return; }
  Pane *target = NULL;
  int idx = g_list_index(panes, p);
  if (where == GHOSTTY_GOTO_SPLIT_NEXT) target = g_list_nth_data(panes, (idx + 1) % n);
  else if (where == GHOSTTY_GOTO_SPLIT_PREVIOUS) target = g_list_nth_data(panes, (idx + n - 1) % n);
  else {
    // directional: nearest pane whose center lies in that direction
    gint px, py, qx, qy;
    gtk_widget_translate_coordinates(p->area, root, gtk_widget_get_allocated_width(p->area) / 2,
                                     gtk_widget_get_allocated_height(p->area) / 2, &px, &py);
    double best = 1e18;
    for (GList *l = panes; l; l = l->next) {
      Pane *q = l->data;
      if (q == p) continue;
      gtk_widget_translate_coordinates(q->area, root, gtk_widget_get_allocated_width(q->area) / 2,
                                       gtk_widget_get_allocated_height(q->area) / 2, &qx, &qy);
      int dx = qx - px, dy = qy - py;
      gboolean ok = (where == GHOSTTY_GOTO_SPLIT_LEFT && dx < 0) || (where == GHOSTTY_GOTO_SPLIT_RIGHT && dx > 0) ||
                    (where == GHOSTTY_GOTO_SPLIT_UP && dy < 0) || (where == GHOSTTY_GOTO_SPLIT_DOWN && dy > 0);
      double d = (double)dx * dx + (double)dy * dy;
      if (ok && d < best) { best = d; target = q; }
    }
  }
  g_list_free(panes);
  if (target) focus_pane(target);
}

static gboolean deferred_close_cb(gpointer ud) { pane_close_now((Pane *)ud); return G_SOURCE_REMOVE; }

static void close_tab_of(Pane *p) {
  GtkWidget *root = tab_root_of(p);
  if (root) close_root(root);
}

// Load the user's Ghostty config. Without a compositor translucency can't show, so ask the core for an opaque
// background instead (keeps the terminal exactly the theme color, matching the tab strip).
static ghostty_config_t load_config(void) {
  ghostty_config_t c = ghostty_config_new();
  {
    // Séance defaults (loaded BEFORE the user's config, so anything the user sets wins): keep the top bar (tabs + ☰ menu) always visible.
    const char *rt0 = g_get_user_runtime_dir();
    gchar *dir0 = rt0 && g_file_test(rt0, G_FILE_TEST_IS_DIR) ? g_strdup(rt0) : g_strdup(g_get_tmp_dir());
    gchar *dpath = g_build_filename(dir0, "seance-defaults.conf", NULL);
    if (g_file_set_contents(dpath, "window-show-tab-bar = always\n", -1, NULL)) ghostty_config_load_file(c, dpath);
    g_free(dpath); g_free(dir0);
  }
  ghostty_config_load_default_files(c);
  if (!composited_ok) {
    const char *rt = g_get_user_runtime_dir();
    gchar *dir = rt && g_file_test(rt, G_FILE_TEST_IS_DIR) ? g_strdup(rt) : g_strdup(g_get_tmp_dir());
    gchar *path = g_build_filename(dir, "seance-opaque.conf", NULL);
    if (g_file_set_contents(path, "background-opacity = 1\n", -1, NULL)) ghostty_config_load_file(c, path);
    g_free(path); g_free(dir);
  }
  ghostty_config_finalize(c);
  return c;
}

// Re-read ~/.config/ghostty/config.ghostty (+ themes) and apply live. The previous config is intentionally not
// freed until exit: surfaces may still hold pointers into it.
static gboolean reload_config_cb(gpointer ud) {
  (void)ud;
  ghostty_config_t nc = load_config();
  ghostty_app_update_config(H.app, nc);
  H.cfg = nc;
  apply_theme();
  emit_event("config_reloaded");
  return G_SOURCE_REMOVE;
}

static bool on_action(ghostty_app_t app, ghostty_target_s target, ghostty_action_s action) {
  (void)app;
  Pane *p = target.tag == GHOSTTY_TARGET_SURFACE ? pane_of_surface(target.target.surface) : NULL;
  switch (action.tag) {
  case GHOSTTY_ACTION_RENDER:
    if (p) gtk_widget_queue_draw(p->area);
    return true;
  case GHOSTTY_ACTION_SET_TITLE:
    if (p && action.action.set_title.title) {
      g_free(p->title);
      p->title = g_strdup(action.action.set_title.title);
      { gchar *e = g_strescape(p->title, NULL); emit_event("title pane=%d title=\"%s\"", p->id, e); g_free(e); }
      set_tab_title(p);
      if (p == H.focus) gtk_window_set_title(GTK_WINDOW(H.win), p->title);
    }
    return true;
  case GHOSTTY_ACTION_MOUSE_SHAPE: {
    GdkWindow *w = p ? gtk_widget_get_window(p->area) : NULL;
    if (w) {
      GdkCursor *c = gdk_cursor_new_from_name(gdk_window_get_display(w), cursor_name(action.action.mouse_shape));
      gdk_window_set_cursor(w, c);
      if (c) g_object_unref(c);
    }
    return true;
  }
  case GHOSTTY_ACTION_OPEN_URL: {
    gchar *url = g_strndup(action.action.open_url.url, action.action.open_url.len);
    gtk_show_uri_on_window(GTK_WINDOW(H.win), url, GDK_CURRENT_TIME, NULL);
    g_free(url);
    return true;
  }
  case GHOSTTY_ACTION_RING_BELL:
    gdk_window_beep(gtk_widget_get_window(H.win));
    emit_event("bell pane=%d", p ? p->id : 0);
    return true;
  case GHOSTTY_ACTION_DESKTOP_NOTIFICATION: {
    const char *t = action.action.desktop_notification.title, *b = action.action.desktop_notification.body;
    gchar *et = g_strescape(t ? t : "", NULL), *eb = g_strescape(b ? b : "", NULL);
    emit_event("notification pane=%d title=\"%s\" body=\"%s\"", p ? p->id : 0, et, eb);
    g_free(et); g_free(eb);
    notify_desktop(t, b);
    return true;
  }
  case GHOSTTY_ACTION_COMMAND_FINISHED:
    emit_event("command_finished pane=%d exit=%d duration_ms=%llu", p ? p->id : 0,
               action.action.command_finished.exit_code,
               (unsigned long long)(action.action.command_finished.duration / 1000000ull));
    return true;
  case GHOSTTY_ACTION_NEW_TAB: {
    Pane *q = pane_new(p, GHOSTTY_SURFACE_CONTEXT_TAB);
    if (q) add_tab(q);
    return true;
  }
  case GHOSTTY_ACTION_NEW_SPLIT:
    if (p) split_pane(p, action.action.new_split);
    return true;
  case GHOSTTY_ACTION_GOTO_SPLIT:
    if (p) goto_split(p, action.action.goto_split);
    return true;
  case GHOSTTY_ACTION_GOTO_TAB: {
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(H.nb));
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(H.nb));
    int want = action.action.goto_tab;
    int idx = want == GHOSTTY_GOTO_TAB_PREVIOUS ? (cur + n - 1) % n
              : want == GHOSTTY_GOTO_TAB_NEXT ? (cur + 1) % n
              : want == GHOSTTY_GOTO_TAB_LAST ? n - 1
              : want - 1;   // 1-based
    if (idx >= 0 && idx < n) {
      gtk_notebook_set_current_page(GTK_NOTEBOOK(H.nb), idx);
      GtkWidget *root = gtk_notebook_get_nth_page(GTK_NOTEBOOK(H.nb), idx);
      GList *panes = NULL;
      collect_panes(root, &panes);
      if (panes) set_focus_pane((Pane *)panes->data);
      g_list_free(panes);
    }
    return true;
  }
  case GHOSTTY_ACTION_CLOSE_TAB:
    if (p) close_tab_of(p);
    return true;
  case GHOSTTY_ACTION_MOVE_TAB:
    if (p) { GtkWidget *root = tab_root_of(p); if (root) tab_move(root, (int)action.action.move_tab.amount); }
    return true;
  case GHOSTTY_ACTION_SET_TAB_TITLE:
    if (p && action.action.set_tab_title.title) {
      GtkWidget *root = tab_root_of(p);
      if (root) { g_object_set_data_full(G_OBJECT(root), "custom-title", g_strdup(action.action.set_tab_title.title), g_free); refresh_tab_label(root); }
    }
    return true;
  case GHOSTTY_ACTION_PROMPT_TITLE:
    if (p) { GtkWidget *root = tab_root_of(p); if (root) g_idle_add(rename_idle, g_object_ref(root)); }
    return true;
  case GHOSTTY_ACTION_RESIZE_SPLIT:
    if (p) resize_split(p, action.action.resize_split.direction, action.action.resize_split.amount);
    return true;
  case GHOSTTY_ACTION_EQUALIZE_SPLITS:
    if (p) equalize_tree(tab_root_of(p));
    return true;
  case GHOSTTY_ACTION_TOGGLE_SPLIT_ZOOM:
    if (p) toggle_zoom(p);
    return true;
  case GHOSTTY_ACTION_SCROLLBAR:
    if (p) {
      p->sb_total = action.action.scrollbar.total; p->sb_off = action.action.scrollbar.offset; p->sb_len = action.action.scrollbar.len;
      gtk_widget_queue_draw(p->area);
    }
    return true;
  case GHOSTTY_ACTION_RELOAD_CONFIG:
    // "soft" reloads are the core re-evaluating conditional config (e.g. after an app queries the color scheme):
    // nothing for the host to do (and swapping configs from inside a core callback crashed vim/tmux sessions).
    if (action.action.reload_config.soft) return false;
    g_idle_add(reload_config_cb, NULL);   // real reload (keybind): run outside the core's tick
    return true;
  case GHOSTTY_ACTION_NEW_WINDOW: {
    gchar *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
      gchar *argv[] = {exe, NULL};
      g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
      g_free(exe);
    }
    return true;
  }
  case GHOSTTY_ACTION_TOGGLE_FULLSCREEN:
  case GHOSTTY_ACTION_TOGGLE_MAXIMIZE: {
    GdkWindow *gw = gtk_widget_get_window(H.win);
    gboolean full = gw && (gdk_window_get_state(gw) & (action.tag == GHOSTTY_ACTION_TOGGLE_FULLSCREEN ? GDK_WINDOW_STATE_FULLSCREEN : GDK_WINDOW_STATE_MAXIMIZED));
    if (action.tag == GHOSTTY_ACTION_TOGGLE_FULLSCREEN) { if (full) gtk_window_unfullscreen(GTK_WINDOW(H.win)); else gtk_window_fullscreen(GTK_WINDOW(H.win)); }
    else { if (full) gtk_window_unmaximize(GTK_WINDOW(H.win)); else gtk_window_maximize(GTK_WINDOW(H.win)); }
    return true;
  }
  case GHOSTTY_ACTION_CLOSE_WINDOW:
  case GHOSTTY_ACTION_QUIT:
    gtk_main_quit();
    return true;
  default:
    return false;   // unimplemented actions are declined
  }
}

// ---------------------------------------------------------------- clipboard (surface userdata = Pane*)
static GtkClipboard *clipboard_for(ghostty_clipboard_e loc) {
  return gtk_clipboard_get(loc == GHOSTTY_CLIPBOARD_SELECTION || loc == GHOSTTY_CLIPBOARD_PRIMARY
                               ? GDK_SELECTION_PRIMARY : GDK_SELECTION_CLIPBOARD);
}

static ghostty_clipboard_read_result_e on_read_clipboard(void *ud, ghostty_clipboard_e loc, void *state,
                                                         const char *const *avail, size_t navail, bool confirm) {
  (void)avail; (void)navail; (void)confirm;
  Pane *p = live_pane(ud);
  if (!p) return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
  gchar *text = gtk_clipboard_wait_for_text(clipboard_for(loc));   // synchronous: fine for now
  if (!text) return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
  ghostty_clipboard_content_s c = {.mime = "text/plain", .data = text, .len = strlen(text)};
  ghostty_clipboard_complete_s done = {.contents = &c, .contents_len = 1, .confirmed = true};
  ghostty_surface_complete_clipboard_request(p->surface, &done, state);
  g_free(text);
  return GHOSTTY_CLIPBOARD_READ_STARTED;
}

static void on_confirm_read(void *ud, const ghostty_clipboard_confirm_s *c, void *state, ghostty_clipboard_request_e r) {
  (void)c; (void)r;
  Pane *p = live_pane(ud);
  if (p) ghostty_surface_deny_clipboard_request(p->surface, state);   // no permission UI yet
}

static void on_write_clipboard(void *ud, ghostty_clipboard_e loc, const ghostty_clipboard_content_s *c, size_t n,
                               bool confirm) {
  (void)ud; (void)confirm;
  for (size_t i = 0; i < n; i++) {
    if (c[i].mime && strncmp(c[i].mime, "text/plain", 10) != 0) continue;
    gtk_clipboard_set_text(clipboard_for(loc), c[i].data, (gint)c[i].len);
    return;
  }
}

// Child exited / surface asked to close. Never free inside a core callback: defer.
static void on_close_surface(void *ud, bool confirm) {
  (void)confirm;
  Pane *p = live_pane(ud);
  if (!p || p->closing) return;
  p->closing = TRUE;
  g_idle_add(deferred_close_cb, p);
}

// ---------------------------------------------------------------- input
static ghostty_input_mods_e mods_of(guint st) {
  int m = GHOSTTY_MODS_NONE;
  if (st & GDK_SHIFT_MASK) m |= GHOSTTY_MODS_SHIFT;
  if (st & GDK_CONTROL_MASK) m |= GHOSTTY_MODS_CTRL;
  if (st & GDK_MOD1_MASK) m |= GHOSTTY_MODS_ALT;
  if (st & GDK_SUPER_MASK) m |= GHOSTTY_MODS_SUPER;
  if (st & GDK_LOCK_MASK) m |= GHOSTTY_MODS_CAPS;
  if (st & GDK_MOD2_MASK) m |= GHOSTTY_MODS_NUM;
  return (ghostty_input_mods_e)m;
}

static void on_im_commit(GtkIMContext *c, gchar *str, gpointer ud) {
  (void)c;
  Pane *p = ud;
  ghostty_surface_text(p->surface, str, strlen(str));
}

static void on_im_preedit(GtkIMContext *c, gpointer ud) {
  Pane *p = ud;
  gchar *str = NULL;
  gtk_im_context_get_preedit_string(c, &str, NULL, NULL);
  ghostty_surface_preedit(p->surface, str ? str : "", str ? strlen(str) : 0);
  g_free(str);
}

static void on_scale_changed(GObject *o, GParamSpec *ps, gpointer ud) {
  (void)ps;
  Pane *p = ud;
  int sf = scale_factor_of(GTK_WIDGET(o));
  ghostty_surface_set_content_scale(p->surface, sf, sf);
  GtkAllocation al;
  gtk_widget_get_allocation(p->area, &al);
  ghostty_surface_set_size(p->surface, al.width * sf, al.height * sf);
}

// Window-level so it runs before GTK's own accelerator handling; routes to the focused pane.
static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer ud) {
  (void)w; (void)ud;
  Pane *p = H.focus;
  if (!p || p->closing) return FALSE;
  if (p->im && gtk_im_context_filter_keypress(p->im, e)) return TRUE;   // dead keys, compose, IME
  char text[8] = {0};
  guint32 uc = gdk_keyval_to_unicode(e->keyval);
  if (e->type == GDK_KEY_PRESS && uc >= 0x20 && uc != 0x7f && !(e->state & GDK_CONTROL_MASK))
    g_unichar_to_utf8(uc, text);

  ghostty_input_key_s k = {0};
  k.action = e->type == GDK_KEY_PRESS ? GHOSTTY_ACTION_PRESS : GHOSTTY_ACTION_RELEASE;
  k.mods = mods_of(e->state);
  k.consumed_mods = (e->state & GDK_SHIFT_MASK) && text[0] ? GHOSTTY_MODS_SHIFT : GHOSTTY_MODS_NONE;
  k.keycode = e->hardware_keycode;          // X11/evdev+8 keycode, same as Ghostty's GTK runtime passes
  k.text = text[0] ? text : NULL;
  k.unshifted_codepoint = gdk_keyval_to_unicode(gdk_keyval_to_lower(e->keyval));
  k.composing = false;
  ghostty_surface_key(p->surface, k);
  return TRUE;
}

// ---------------------------------------------------------------- menus (right-click context menu, hamburger menu)
static void menu_binding(GtkMenuItem *mi, gpointer ud) {   // simple core actions on the pane: copy/paste/select-all
  (void)mi;
  Pane *p = live_pane(g_object_get_data(G_OBJECT(mi), "pane"));
  const char *act = ud;
  if (p) ghostty_surface_binding_action(p->surface, act, strlen(act));
}
static void menu_new_tab(GtkMenuItem *mi, gpointer ud) { (void)mi; (void)ud; Pane *q = pane_new(H.focus, GHOSTTY_SURFACE_CONTEXT_TAB); if (q) add_tab(q); }
static void menu_new_window(GtkMenuItem *mi, gpointer ud) {
  (void)mi; (void)ud;
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  if (exe) { gchar *argv[] = {exe, NULL}; g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL); g_free(exe); }
}
static void menu_split(GtkMenuItem *mi, gpointer ud) { (void)mi; if (H.focus) split_pane(H.focus, (ghostty_action_split_direction_e)GPOINTER_TO_INT(ud)); }
static void menu_close_pane(GtkMenuItem *mi, gpointer ud) { (void)mi; (void)ud; if (H.focus && !H.focus->closing) ghostty_surface_request_close(H.focus->surface); }
static void menu_reload(GtkMenuItem *mi, gpointer ud) { (void)mi; (void)ud; g_idle_add(reload_config_cb, NULL); }
static void menu_fullscreen(GtkMenuItem *mi, gpointer ud) {
  (void)mi; (void)ud;
  GdkWindow *gw = gtk_widget_get_window(H.win);
  if (gw && (gdk_window_get_state(gw) & GDK_WINDOW_STATE_FULLSCREEN)) gtk_window_unfullscreen(GTK_WINDOW(H.win));
  else gtk_window_fullscreen(GTK_WINDOW(H.win));
}
static void menu_open_config(GtkMenuItem *mi, gpointer ud) {   // open the config in $EDITOR inside a new tab (works on headless-ish VMs too)
  (void)mi; (void)ud;
  gchar *path = g_build_filename(g_get_user_config_dir(), "ghostty", "config.ghostty", NULL);
  gchar *q = g_shell_quote(path);
  g_free(next_pane_command);
  next_pane_command = g_strdup_printf("${EDITOR:-vi} %s", q);
  Pane *np = pane_new(H.focus, GHOSTTY_SURFACE_CONTEXT_TAB);
  if (np) add_tab(np);
  g_free(q); g_free(path);
}
static void menu_about(GtkMenuItem *mi, gpointer ud) {
  (void)mi; (void)ud;
  GtkWidget *d = gtk_about_dialog_new();
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(d), "Séance");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(d), SEANCE_VERSION);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(d), "A GTK3 terminal for SLES 15 SP4, built on the Ghostty terminal core.\\nSoftware-rendered: no GPU required.");
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(d), "https://github.com/n0tbhargav/seance");
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(d), GTK_LICENSE_MIT_X11);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(d), "Séance is independent of the Ghostty project. Ghostty © Mitchell Hashimoto and contributors (MIT).");
  gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(H.win));
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}
static void menu_quit(GtkMenuItem *mi, gpointer ud) { (void)mi; (void)ud; gtk_main_quit(); }

static void menu_add(GtkWidget *menu, const char *label, GCallback cb, gpointer data, Pane *p, gboolean enabled) {
  GtkWidget *mi = label ? gtk_menu_item_new_with_label(label) : gtk_separator_menu_item_new();
  if (label) {
    g_object_set_data(G_OBJECT(mi), "pane", p);
    g_signal_connect(mi, "activate", cb, data);
    gtk_widget_set_sensitive(mi, enabled);
  }
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

// Terminal right-click menu (edit + window actions). Programs that capture the mouse still get right-clicks unless Shift is held.
static void show_context_menu(Pane *p, GdkEvent *ev) {
  GtkWidget *m = gtk_menu_new();
  gboolean sel = ghostty_surface_has_selection(p->surface);
  menu_add(m, "Copy", G_CALLBACK(menu_binding), "copy_to_clipboard", p, sel);
  menu_add(m, "Paste", G_CALLBACK(menu_binding), "paste_from_clipboard", p, TRUE);
  menu_add(m, "Select All", G_CALLBACK(menu_binding), "select_all", p, TRUE);
  menu_add(m, NULL, NULL, NULL, p, TRUE);
  menu_add(m, "New Tab", G_CALLBACK(menu_new_tab), NULL, p, TRUE);
  menu_add(m, "New Window", G_CALLBACK(menu_new_window), NULL, p, TRUE);
  menu_add(m, "Split Right", G_CALLBACK(menu_split), GINT_TO_POINTER(GHOSTTY_SPLIT_DIRECTION_RIGHT), p, TRUE);
  menu_add(m, "Split Down", G_CALLBACK(menu_split), GINT_TO_POINTER(GHOSTTY_SPLIT_DIRECTION_DOWN), p, TRUE);
  menu_add(m, NULL, NULL, NULL, p, TRUE);
  menu_add(m, "Close Pane", G_CALLBACK(menu_close_pane), NULL, p, TRUE);
  menu_add(m, NULL, NULL, NULL, p, TRUE);
  menu_add(m, "Reload Configuration", G_CALLBACK(menu_reload), NULL, p, TRUE);
  menu_add(m, "Open Configuration…", G_CALLBACK(menu_open_config), NULL, p, TRUE);
  g_signal_connect(m, "deactivate", G_CALLBACK(on_menu_deactivate), NULL);
  gtk_widget_show_all(m);
  gtk_menu_popup_at_pointer(GTK_MENU(m), ev);
}

// "☰" button menu in the tab strip.
static void on_hamburger_clicked(GtkButton *b, gpointer ud) {
  (void)ud;
  GtkWidget *m = gtk_menu_new();
  Pane *p = H.focus;
  menu_add(m, "New Tab", G_CALLBACK(menu_new_tab), NULL, p, TRUE);
  menu_add(m, "New Window", G_CALLBACK(menu_new_window), NULL, p, TRUE);
  menu_add(m, NULL, NULL, NULL, p, TRUE);
  menu_add(m, "Split Right", G_CALLBACK(menu_split), GINT_TO_POINTER(GHOSTTY_SPLIT_DIRECTION_RIGHT), p, p != NULL);
  menu_add(m, "Split Down", G_CALLBACK(menu_split), GINT_TO_POINTER(GHOSTTY_SPLIT_DIRECTION_DOWN), p, p != NULL);
  menu_add(m, NULL, NULL, NULL, p, TRUE);
  menu_add(m, "Reload Configuration", G_CALLBACK(menu_reload), NULL, p, TRUE);
  menu_add(m, "Open Configuration…", G_CALLBACK(menu_open_config), NULL, p, TRUE);
  menu_add(m, "Toggle Fullscreen", G_CALLBACK(menu_fullscreen), NULL, p, TRUE);
  menu_add(m, NULL, NULL, NULL, p, TRUE);
  menu_add(m, "About Séance", G_CALLBACK(menu_about), NULL, p, TRUE);
  menu_add(m, "Quit", G_CALLBACK(menu_quit), NULL, p, TRUE);
  g_signal_connect(m, "deactivate", G_CALLBACK(on_menu_deactivate), NULL);
  gtk_widget_show_all(m);
  gtk_menu_popup_at_widget(GTK_MENU(m), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *e, gpointer ud) {
  (void)w;
  Pane *p = ud;
  if (H.focus != p) set_focus_pane(p);
  if (e->type != GDK_BUTTON_PRESS && e->type != GDK_BUTTON_RELEASE) return TRUE;
  if (e->button == 3) {   // right-click: our context menu, unless a program captured the mouse (Shift overrides)
    if (e->type == GDK_BUTTON_PRESS && (!ghostty_surface_mouse_captured(p->surface) || (e->state & GDK_SHIFT_MASK))) {
      p->swallow_rclick = TRUE;
      show_context_menu(p, (GdkEvent *)e);
      return TRUE;
    }
    if (e->type == GDK_BUTTON_RELEASE && p->swallow_rclick) { p->swallow_rclick = FALSE; return TRUE; }
  }
  ghostty_input_mouse_button_e b = e->button == 1 ? GHOSTTY_MOUSE_LEFT
                                   : e->button == 2 ? GHOSTTY_MOUSE_MIDDLE
                                   : e->button == 3 ? GHOSTTY_MOUSE_RIGHT : GHOSTTY_MOUSE_UNKNOWN;
  int sf = scale_factor_of(p->area);
  ghostty_surface_mouse_pos(p->surface, e->x * sf, e->y * sf, mods_of(e->state));
  ghostty_surface_mouse_button(p->surface, e->type == GDK_BUTTON_PRESS ? GHOSTTY_MOUSE_PRESS : GHOSTTY_MOUSE_RELEASE,
                               b, mods_of(e->state));
  return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer ud) {
  (void)w;
  Pane *p = ud;
  int sf = scale_factor_of(p->area);
  ghostty_surface_mouse_pos(p->surface, e->x * sf, e->y * sf, mods_of(e->state));
  return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer ud) {
  (void)w;
  Pane *p = ud;
  double dx = 0, dy = 0;
  switch (e->direction) {
  case GDK_SCROLL_UP: dy = 1; break;
  case GDK_SCROLL_DOWN: dy = -1; break;
  case GDK_SCROLL_LEFT: dx = 1; break;
  case GDK_SCROLL_RIGHT: dx = -1; break;
  case GDK_SCROLL_SMOOTH: gdk_event_get_scroll_deltas((GdkEvent *)e, &dx, &dy); dx = -dx; dy = -dy; break;
  default: return TRUE;
  }
  ghostty_surface_mouse_scroll(p->surface, dx, dy, 0);
  return TRUE;
}

static gboolean on_pane_focus(GtkWidget *w, GdkEventFocus *e, gpointer ud) {
  (void)w;
  Pane *p = ud;
  if (e->in) {
    H.focus = p;
    if (p->im) gtk_im_context_focus_in(p->im);
    if (p->title) gtk_window_set_title(GTK_WINDOW(H.win), p->title);
  } else if (p->im) gtk_im_context_focus_out(p->im);
  ghostty_surface_set_focus(p->surface, e->in);
  return FALSE;
}

static gboolean on_win_focus(GtkWidget *w, GdkEventFocus *e, gpointer ud) {
  (void)w; (void)ud;
  ghostty_app_set_focus(H.app, e->in);
  return FALSE;
}

static void on_realize(GtkWidget *w, gpointer ud) {
  Pane *p = ud;
  gtk_im_context_set_client_window(p->im, gtk_widget_get_window(w));
}

// ---------------------------------------------------------------- pane lifecycle
static Pane *pane_new(Pane *inherit_from, ghostty_surface_context_e ctx) {
  Pane *p = g_new0(Pane, 1);
  p->area = gtk_drawing_area_new();
  gtk_widget_set_can_focus(p->area, TRUE);
  gtk_widget_set_hexpand(p->area, TRUE);
  gtk_widget_set_vexpand(p->area, TRUE);
  gtk_widget_add_events(p->area, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK |
                                     GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_FOCUS_CHANGE_MASK);
  g_object_set_data(G_OBJECT(p->area), "pane", p);

  p->id = next_pane_id++;
  ghostty_surface_config_s sc = inherit_from && inherit_from->surface
                                    ? ghostty_surface_inherited_config(inherit_from->surface, ctx)
                                    : ghostty_surface_config_new();
  sc.platform_tag = GHOSTTY_PLATFORM_LINUX;
  sc.platform.linux_.reserved = NULL;
  sc.userdata = p;
  sc.scale_factor = scale_factor_of(H.win);
  sc.context = ctx;
  // Let programs inside the pane (e.g. coding agents) find and address their own terminal.
  gchar *pane_id_str = g_strdup_printf("%d", p->id);
  ghostty_env_var_s envs[2] = {{.key = "SEANCE_SOCKET", .value = sock_path ? sock_path : ""},
                               {.key = "SEANCE_PANE", .value = pane_id_str}};
  sc.env_vars = envs;
  sc.env_var_count = 2;
  if (next_pane_command) sc.command = next_pane_command;
  else if (!inherit_from) sc.command = g_getenv("SEANCE_CMD");   // run a command instead of the login shell (scripting/benchmarks)
  p->surface = ghostty_surface_new(H.app, &sc);
  g_free(pane_id_str);
  g_free(next_pane_command);
  next_pane_command = NULL;
  if (!p->surface) { g_printerr("ghostty_surface_new failed\n"); gtk_widget_destroy(p->area); g_free(p); return NULL; }

  p->im = gtk_im_multicontext_new();
  g_signal_connect(p->im, "commit", G_CALLBACK(on_im_commit), p);
  g_signal_connect(p->im, "preedit-changed", G_CALLBACK(on_im_preedit), p);
  g_signal_connect(p->area, "realize", G_CALLBACK(on_realize), p);
  g_signal_connect(p->area, "draw", G_CALLBACK(on_draw), p);
  g_signal_connect(p->area, "size-allocate", G_CALLBACK(on_size), p);
  g_signal_connect(p->area, "button-press-event", G_CALLBACK(on_button), p);
  g_signal_connect(p->area, "button-release-event", G_CALLBACK(on_button), p);
  g_signal_connect(p->area, "motion-notify-event", G_CALLBACK(on_motion), p);
  g_signal_connect(p->area, "scroll-event", G_CALLBACK(on_scroll), p);
  g_signal_connect(p->area, "focus-in-event", G_CALLBACK(on_pane_focus), p);
  g_signal_connect(p->area, "focus-out-event", G_CALLBACK(on_pane_focus), p);
  g_signal_connect(p->area, "notify::scale-factor", G_CALLBACK(on_scale_changed), p);
  all_panes = g_list_append(all_panes, p);
  emit_event("pane_opened pane=%d", p->id);
  return p;
}

// Freeing a surface stops its threads and kills the child process; it can block for a moment. Done after the UI update.
static GList *pending_free;   // Pane* whose widgets are gone but whose core surface is not freed yet
static void free_pane_now(Pane *p) {
  pending_free = g_list_remove(pending_free, p);
  gint64 t0 = g_get_monotonic_time();
  if (p->surface) ghostty_surface_free(p->surface);
  if (g_getenv("SEANCE_STATS")) g_printerr("surface free took %.0f ms\n", (g_get_monotonic_time() - t0) / 1000.0);
  if (p->img) cairo_surface_destroy(p->img);
  if (p->im) g_object_unref(p->im);
  g_free(p->title);
  g_free(p);
}
static gboolean free_pane_cb(gpointer ud) { if (g_list_find(pending_free, ud)) free_pane_now((Pane *)ud); return G_SOURCE_REMOVE; }

// Remove the pane from the widget tree (collapsing splits / dropping the tab) and free it.
static void pane_close_now(Pane *p) {
  GtkWidget *area = p->area;
  unzoom_root(tab_root_of(p));   // a zoomed tab has hidden siblings: restore before restructuring
  GtkWidget *parent = gtk_widget_get_parent(area);
  p->closing = TRUE;
  if (H.focus == p) H.focus = NULL;
  all_panes = g_list_remove(all_panes, p);
  emit_event("pane_closed pane=%d", p->id);

  Pane *next_focus = NULL;
  if (GTK_IS_PANED(parent)) {
    gboolean first = gtk_paned_get_child1(GTK_PANED(parent)) == area;
    GtkWidget *sibling = first ? gtk_paned_get_child2(GTK_PANED(parent)) : gtk_paned_get_child1(GTK_PANED(parent));
    GtkWidget *grand = gtk_widget_get_parent(parent);
    g_object_ref(sibling);
    gtk_container_remove(GTK_CONTAINER(parent), sibling);
    if (GTK_IS_NOTEBOOK(grand)) {
      gint idx = gtk_notebook_page_num(GTK_NOTEBOOK(grand), parent);
      char *custom = g_strdup(g_object_get_data(G_OBJECT(parent), "custom-title"));
      gtk_notebook_remove_page(GTK_NOTEBOOK(grand), idx);     // destroys paned (and p->area)
      install_tab(sibling, idx, custom);
      gtk_notebook_set_current_page(GTK_NOTEBOOK(grand), idx);
    } else {
      gboolean pfirst = gtk_paned_get_child1(GTK_PANED(grand)) == parent;
      gtk_container_remove(GTK_CONTAINER(grand), parent);
      if (pfirst) gtk_paned_pack1(GTK_PANED(grand), sibling, TRUE, TRUE);
      else gtk_paned_pack2(GTK_PANED(grand), sibling, TRUE, TRUE);
    }
    g_object_unref(sibling);
    GList *panes = NULL;
    collect_panes(sibling, &panes);
    if (panes) next_focus = panes->data;
    g_list_free(panes);
    gtk_widget_show_all(sibling);
  } else if (GTK_IS_NOTEBOOK(parent)) {
    gtk_notebook_remove_page(GTK_NOTEBOOK(parent), gtk_notebook_page_num(GTK_NOTEBOOK(parent), area));
  }

  // The pane is already out of the widget tree. Update the UI (tab bar, focus) first, and only THEN free the core surface:
  // freeing waits for the child process to die, which can take a moment, and must not delay the tab vanishing.
  update_tabs_visible();
  gboolean last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(H.nb)) == 0;
  if (!last) {
    if (!next_focus) {
      GtkWidget *root = gtk_notebook_get_nth_page(GTK_NOTEBOOK(H.nb), gtk_notebook_get_current_page(GTK_NOTEBOOK(H.nb)));
      GList *panes = NULL;
      collect_panes(root, &panes);
      if (panes) next_focus = panes->data;
      g_list_free(panes);
    }
    if (next_focus) set_focus_pane(next_focus);
    pending_free = g_list_append(pending_free, p);
    g_idle_add_full(G_PRIORITY_LOW, free_pane_cb, p, NULL);   // lower than GDK's redraw priority: repaint happens first
  } else {
    free_pane_now(p);      // last pane: we are quitting, free before the app goes away
    gtk_main_quit();
  }
}

// ---------------------------------------------------------------- control socket (agents / scripting)
// Line protocol over a Unix socket ($SEANCE_SOCKET, exported to every pane). One command per line;
// replies are "OK <nbytes>\n<payload>" or "ERR <message>\n". "SUBSCRIBE" turns the connection into an
// event stream ("EVENT <type> key=val ...").
//   LIST                      panes: id, tab, focus, size, title
//   READ <pane> [scrollback]  visible screen (or whole scrollback) as text
//   SEND <pane> <text>        paste-style text insert (C escapes: \n \r \t \e \xHH \\); does not press Enter
//   KEY <pane> <key>...       real key events: enter esc tab bs up down left right home end pgup pgdn del a..z 0..9 space f1..f12,
//                             with modifiers e.g. ctrl-c alt-x shift-tab
//   EXEC <pane> <text>        type <text> verbatim (no escapes) + Enter
//   FOCUS <pane> | SPLIT <pane> right|down|left|up | NEWTAB [pane] | CLOSE <pane>
//   SUBSCRIBE                 stream events: command_finished bell notification title focus pane_opened pane_closed
typedef struct {
  GSocketConnection *conn;
  GDataInputStream *in;
  GOutputStream *out;
  gboolean subscribed;
} Client;

static GList *clients;

static void client_free(Client *c) {
  clients = g_list_remove(clients, c);
  g_object_unref(c->in);
  g_object_unref(c->conn);
  g_free(c);
}

static void emit_event(const char *fmt, ...) {
  if (!clients) return;
  va_list ap;
  va_start(ap, fmt);
  gchar *body = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  gchar *line = g_strdup_printf("EVENT %s\n", body);
  for (GList *l = clients; l; l = l->next) {
    Client *c = l->data;
    if (c->subscribed) g_output_stream_write_all(c->out, line, strlen(line), NULL, NULL, NULL);
  }
  g_free(line); g_free(body);
}

static void reply(Client *c, const char *data, gsize len) {
  gchar *hdr = g_strdup_printf("OK %" G_GSIZE_FORMAT "\n", len);
  g_output_stream_write_all(c->out, hdr, strlen(hdr), NULL, NULL, NULL);
  if (len) g_output_stream_write_all(c->out, data, len, NULL, NULL, NULL);
  g_free(hdr);
}

static void reply_err(Client *c, const char *msg) {
  gchar *l = g_strdup_printf("ERR %s\n", msg);
  g_output_stream_write_all(c->out, l, strlen(l), NULL, NULL, NULL);
  g_free(l);
}

static Pane *find_pane_tok(const char *tok);
static Pane *find_pane(int id) {
  for (GList *l = all_panes; l; l = l->next) if (((Pane *)l->data)->id == id && !((Pane *)l->data)->closing) return l->data;
  return NULL;
}

// C-style escapes -> raw bytes (returns g_malloc'd buffer, sets *len)
static char *unescape(const char *in, gsize *len) {
  GString *o = g_string_new(NULL);
  for (const char *p = in; *p; p++) {
    if (*p != '\\' || !p[1]) { g_string_append_c(o, *p); continue; }
    p++;
    switch (*p) {
    case 'n': g_string_append_c(o, '\n'); break;
    case 'r': g_string_append_c(o, '\r'); break;
    case 't': g_string_append_c(o, '\t'); break;
    case 'e': g_string_append_c(o, 0x1b); break;
    case '\\': g_string_append_c(o, '\\'); break;
    case 'x': {
      int v;
      if (p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1) { g_string_append_c(o, (char)v); p += 2; }
      break;
    }
    default: g_string_append_c(o, *p);
    }
  }
  *len = o->len;
  return g_string_free(o, FALSE);
}

static int keycode_for(const char *name, char *ch) {   // X11 keycodes (evdev + 8)
  static const struct { const char *n; int code; } named[] = {
      {"enter", 36}, {"return", 36}, {"esc", 9}, {"escape", 9}, {"tab", 23}, {"bs", 22}, {"backspace", 22},
      {"space", 65}, {"up", 111}, {"down", 116}, {"left", 113}, {"right", 114}, {"home", 110}, {"end", 115},
      {"pgup", 112}, {"pgdn", 117}, {"del", 119}, {"delete", 119}, {"ins", 118},
      {"f1", 67}, {"f2", 68}, {"f3", 69}, {"f4", 70}, {"f5", 71}, {"f6", 72}, {"f7", 73}, {"f8", 74}, {"f9", 75},
      {"f10", 76}, {"f11", 95}, {"f12", 96}};
  static const int letters[26] = {38, 56, 54, 40, 26, 41, 42, 43, 31, 44, 45, 46, 58, 57, 32, 33, 24, 27, 39, 53, 30, 55, 25, 52, 29, 51};
  *ch = 0;
  for (size_t i = 0; i < G_N_ELEMENTS(named); i++) if (!strcmp(name, named[i].n)) { if (named[i].code == 65) *ch = ' '; return named[i].code; }
  if (strlen(name) == 1) {
    char c = g_ascii_tolower(name[0]);
    if (c >= 'a' && c <= 'z') { *ch = c; return letters[c - 'a']; }
    if (c >= '1' && c <= '9') { *ch = c; return 10 + (c - '1'); }
    if (c == '0') { *ch = c; return 19; }
  }
  return 0;
}

// US-layout mapping for single printable characters -> (X11 keycode, needs Shift)
static int char_key(char c, gboolean *shift) {
  static const struct { char ch; int code; gboolean sh; } t[] = {
      {';', 47, 0}, {':', 47, 1}, {'\'', 48, 0}, {'"', 48, 1}, {',', 59, 0}, {'<', 59, 1}, {'.', 60, 0}, {'>', 60, 1},
      {'/', 61, 0}, {'?', 61, 1}, {'-', 20, 0}, {'_', 20, 1}, {'=', 21, 0}, {'+', 21, 1}, {'[', 34, 0}, {'{', 34, 1},
      {']', 35, 0}, {'}', 35, 1}, {'\\', 51, 0}, {'|', 51, 1}, {'`', 49, 0}, {'~', 49, 1},
      {'!', 10, 1}, {'@', 11, 1}, {'#', 12, 1}, {'$', 13, 1}, {'%', 14, 1}, {'^', 15, 1}, {'&', 16, 1}, {'*', 17, 1},
      {'(', 18, 1}, {')', 19, 1}};
  *shift = FALSE;
  for (size_t i = 0; i < G_N_ELEMENTS(t); i++) if (t[i].ch == c) { *shift = t[i].sh; return t[i].code; }
  return 0;
}

static gboolean send_key_spec(Pane *p, const char *spec) {   // e.g. "ctrl-c", "enter", "alt-shift-x"
  gchar **parts = g_strsplit(spec, "-", -1);
  int mods = 0;
  const char *key = NULL;
  for (int i = 0; parts[i]; i++) {
    if (!strcmp(parts[i], "ctrl")) mods |= GHOSTTY_MODS_CTRL;
    else if (!strcmp(parts[i], "alt")) mods |= GHOSTTY_MODS_ALT;
    else if (!strcmp(parts[i], "shift")) mods |= GHOSTTY_MODS_SHIFT;
    else if (!strcmp(parts[i], "super")) mods |= GHOSTTY_MODS_SUPER;
    else key = parts[i];
  }
  char ch = 0;
  int code = key ? keycode_for(key, &ch) : 0;
  char text[2] = {0, 0};
  char unshifted = 0;
  if (!code && key && strlen(key) == 1) {      // punctuation / shifted symbols: "key %" "key :" "key A"
    gboolean sh = FALSE;
    char c = key[0];
    if (g_ascii_isupper(c)) { code = keycode_for((char[]){(char)g_ascii_tolower(c), 0}, &ch); sh = TRUE; text[0] = c; unshifted = (char)g_ascii_tolower(c); }
    else if ((code = char_key(c, &sh))) { text[0] = c; unshifted = c; }
    if (sh) mods |= GHOSTTY_MODS_SHIFT;
  } else if (code && ch) {                       // named/lowercase key, maybe with shift held
    unshifted = ch;
    text[0] = (mods & GHOSTTY_MODS_SHIFT) ? (char)g_ascii_toupper(ch) : ch;   // (digits: use the symbol keys instead)
  }
  if (!code) { g_strfreev(parts); return FALSE; }
  ghostty_input_key_s k = {0};
  k.action = GHOSTTY_ACTION_PRESS;
  k.mods = (ghostty_input_mods_e)mods;
  k.keycode = code;
  k.text = (text[0] && !(mods & (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT))) ? text : NULL;
  k.unshifted_codepoint = (unsigned char)unshifted;
  ghostty_surface_key(p->surface, k);
  k.action = GHOSTTY_ACTION_RELEASE;
  k.text = NULL;
  ghostty_surface_key(p->surface, k);
  g_strfreev(parts);
  return TRUE;
}

static Pane *find_pane_tok(const char *tok) {   // "focused" or a numeric id
  if (!g_ascii_strcasecmp(tok, "focused")) return H.focus && !H.focus->closing ? H.focus : NULL;
  return find_pane(atoi(tok));
}

static void handle_line(Client *c, char *line) {
  g_strstrip(line);
  if (!*line) return;
  gchar *rest = strchr(line, ' ');
  if (rest) *rest++ = 0;
  const char *cmd = line;

  if (!g_ascii_strcasecmp(cmd, "LIST")) {
    GString *o = g_string_new(NULL);
    for (GList *l = all_panes; l; l = l->next) {
      Pane *p = l->data;
      if (p->closing) continue;
      GtkWidget *root = tab_root_of(p);
      int tab = root ? gtk_notebook_page_num(GTK_NOTEBOOK(H.nb), root) : -1;
      ghostty_surface_size_s sz = ghostty_surface_size(p->surface);
      gchar *t = g_strescape(p->title ? p->title : "", NULL);
      g_string_append_printf(o, "pane=%d tab=%d focused=%d cols=%u rows=%u title=\"%s\"\n", p->id, tab,
                             p == H.focus, sz.columns, sz.rows, t);
      g_free(t);
    }
    reply(c, o->str, o->len);
    g_string_free(o, TRUE);
    return;
  }
  if (!g_ascii_strcasecmp(cmd, "SUBSCRIBE")) { c->subscribed = TRUE; reply(c, "", 0); return; }
  if (!g_ascii_strcasecmp(cmd, "NEWTAB")) {
    Pane *from = rest && *rest ? find_pane_tok(rest) : H.focus;
    Pane *q = pane_new(from, GHOSTTY_SURFACE_CONTEXT_TAB);
    if (!q) { reply_err(c, "cannot create tab"); return; }
    add_tab(q);
    gchar *id = g_strdup_printf("%d\n", q->id);
    reply(c, id, strlen(id));
    g_free(id);
    return;
  }

  // commands taking a pane id
  if (!rest) { reply_err(c, "usage: <CMD> <pane> ..."); return; }
  gchar *arg = strchr(rest, ' ');
  if (arg) *arg++ = 0;
  Pane *p = find_pane_tok(rest);
  if (!p) { reply_err(c, "no such pane"); return; }

  if (!g_ascii_strcasecmp(cmd, "READ")) {
    ghostty_selection_s sel = {0};
    gboolean all = arg && g_str_has_prefix(arg, "scrollback");
    ghostty_point_tag_e tag = all ? GHOSTTY_POINT_SCREEN : GHOSTTY_POINT_VIEWPORT;
    sel.top_left = (ghostty_point_s){.tag = tag, .coord = GHOSTTY_POINT_COORD_TOP_LEFT, .x = 0, .y = 0};
    sel.bottom_right = (ghostty_point_s){.tag = tag, .coord = GHOSTTY_POINT_COORD_BOTTOM_RIGHT, .x = 0, .y = 0};
    ghostty_text_s t = {0};
    if (ghostty_surface_read_text(p->surface, sel, &t)) {
      reply(c, t.text, t.text_len);
      ghostty_surface_free_text(p->surface, &t);
    } else reply_err(c, "read failed");
  } else if (!g_ascii_strcasecmp(cmd, "SEND") || !g_ascii_strcasecmp(cmd, "EXEC")) {
    gsize n = 0;
    gboolean is_exec = !g_ascii_strcasecmp(cmd, "EXEC");
    // SEND interprets C escapes; EXEC types the text verbatim (commands often contain backslashes) then presses Enter.
    char *raw = is_exec ? g_strdup(arg ? arg : "") : unescape(arg ? arg : "", &n);
    if (is_exec) n = strlen(raw);
    if (n) ghostty_surface_text(p->surface, raw, n);
    g_free(raw);
    if (is_exec) send_key_spec(p, "enter");
    reply(c, "", 0);
  } else if (!g_ascii_strcasecmp(cmd, "KEY")) {
    gchar **keys = g_strsplit(arg ? arg : "", " ", -1);
    gboolean ok = TRUE;
    for (int i = 0; keys[i]; i++) if (*keys[i] && !send_key_spec(p, keys[i])) ok = FALSE;
    g_strfreev(keys);
    if (ok) reply(c, "", 0); else reply_err(c, "unknown key");
  } else if (!g_ascii_strcasecmp(cmd, "FOCUS")) {
    focus_pane(p);
    reply(c, "", 0);
  } else if (!g_ascii_strcasecmp(cmd, "SPLIT")) {
    ghostty_action_split_direction_e d = !arg ? GHOSTTY_SPLIT_DIRECTION_RIGHT
        : !g_ascii_strcasecmp(arg, "down") ? GHOSTTY_SPLIT_DIRECTION_DOWN
        : !g_ascii_strcasecmp(arg, "left") ? GHOSTTY_SPLIT_DIRECTION_LEFT
        : !g_ascii_strcasecmp(arg, "up") ? GHOSTTY_SPLIT_DIRECTION_UP : GHOSTTY_SPLIT_DIRECTION_RIGHT;
    split_pane(p, d);
    reply(c, "", 0);
  } else if (!g_ascii_strcasecmp(cmd, "CLOSE")) {
    if (!p->closing) { p->closing = TRUE; g_idle_add(deferred_close_cb, p); }
    reply(c, "", 0);
  } else {
    reply_err(c, "unknown command");
  }
}

static void client_read_cb(GObject *src, GAsyncResult *res, gpointer ud) {
  Client *c = ud;
  gsize len = 0;
  GError *err = NULL;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &err);
  if (!line) { g_clear_error(&err); client_free(c); return; }
  handle_line(c, line);
  g_free(line);
  g_data_input_stream_read_line_async(c->in, G_PRIORITY_DEFAULT, NULL, client_read_cb, c);
}

static gboolean on_incoming(GSocketService *svc, GSocketConnection *conn, GObject *src, gpointer ud) {
  (void)svc; (void)src; (void)ud;
  Client *c = g_new0(Client, 1);
  c->conn = g_object_ref(conn);
  c->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
  c->out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  clients = g_list_append(clients, c);
  g_data_input_stream_read_line_async(c->in, G_PRIORITY_DEFAULT, NULL, client_read_cb, c);
  return TRUE;
}

static void start_control_socket(void) {
  const char *rt = g_get_user_runtime_dir();
  gchar *dir = rt && g_file_test(rt, G_FILE_TEST_IS_DIR) ? g_strdup(rt) : g_strdup_printf("/tmp/seance-%u", (unsigned)getuid());
  g_mkdir_with_parents(dir, 0700);
  sock_path = g_strdup_printf("%s/seance-%d.sock", dir, (int)getpid());
  g_free(dir);
  unlink(sock_path);
  GSocketService *svc = g_socket_service_new();
  GError *err = NULL;
  GSocketAddress *addr = g_unix_socket_address_new(sock_path);
  if (!g_socket_listener_add_address(G_SOCKET_LISTENER(svc), addr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err)) {
    g_printerr("control socket disabled: %s\n", err->message);
    g_clear_error(&err);
    g_free(sock_path); sock_path = NULL;
  } else {
    chmod(sock_path, 0600);
    g_signal_connect(svc, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(svc);
    g_setenv("SEANCE_SOCKET", sock_path, TRUE);
  }
  g_object_unref(addr);
}

// ---------------------------------------------------------------- startup
// Ghostty finds terminfo/shell-integration/themes via GHOSTTY_RESOURCES_DIR (prefix/share/ghostty, with
// prefix/share/terminfo beside it). Point it at our install if the user hasn't.
static void locate_resources(void) {
  if (g_getenv("GHOSTTY_RESOURCES_DIR")) return;
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  gchar *dir = exe ? g_path_get_dirname(exe) : NULL;
  gchar *cands[] = {
      g_getenv("SEANCE_RESOURCES") ? g_strdup(g_getenv("SEANCE_RESOURCES")) : NULL,
      dir ? g_build_filename(dir, "..", "share", "ghostty", NULL) : NULL,          // installed layout
      dir ? g_build_filename(dir, "..", "full", "share", "ghostty", NULL) : NULL,  // dev tree (host/ next to full/)
      g_strdup("/usr/share/seance/ghostty"),
      NULL,
  };
  for (int i = 0; i < 4; i++) {
    if (cands[i] && g_file_test(cands[i], G_FILE_TEST_IS_DIR)) {
      char *rp = realpath(cands[i], NULL);
      gchar *real = g_strdup(rp ? rp : cands[i]);
      free(rp);
      g_setenv("GHOSTTY_RESOURCES_DIR", real, FALSE);
      g_free(real);
      break;
    }
  }
  for (int i = 0; i < 4; i++) g_free(cands[i]);
  g_free(dir); g_free(exe);
}

static void usage(void) {
  puts("seance " SEANCE_VERSION " - GTK3 terminal on the Ghostty core\n"
       "usage: seance [-e COMMAND...] [--size WxH] [--version] [--help]\n"
       "  -e, --command CMD        run CMD instead of the login shell (rest of the line)\n"
       "  --size WxH               initial window size in pixels (default 1000x640)\n"
       "  --install-desktop        add a launcher + icon for THIS install to ~/.local/share (menu entry); --uninstall-desktop removes it\n"
       "  --shell-hook tcsh        print the line to add to ~/.tcshrc for prompt/cwd/command-finished integration\n"
       "config: ~/.config/ghostty/config.ghostty   control: seancectl (see $SEANCE_SOCKET)");
}

// ---------------------------------------------------------------- app icon, desktop entry, shell hook (no display needed)
static gchar *exe_dir_path(void) {
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  if (!exe) return NULL;
  gchar *d = g_path_get_dirname(exe);
  g_free(exe);
  return d;
}

// Find a bundled file in the installed layout (<prefix>/bin/../share/...) or the source tree (host/../packaging/...).
static gchar *find_bundled(const char *installed_rel, const char *devtree_rel) {
  gchar *dir = exe_dir_path();
  if (!dir) return NULL;
  gchar *a = g_build_filename(dir, "..", installed_rel, NULL), *b = g_build_filename(dir, "..", devtree_rel, NULL);
  gchar *hit = NULL;
  if (g_file_test(a, G_FILE_TEST_EXISTS)) { hit = a; a = NULL; } else if (g_file_test(b, G_FILE_TEST_EXISTS)) { hit = b; b = NULL; }
  g_free(a); g_free(b); g_free(dir);
  if (hit) { char *rp = realpath(hit, NULL); if (rp) { g_free(hit); hit = g_strdup(rp); free(rp); } }
  return hit;
}

static gchar *icon_file(int size) {
  gchar *inst = g_strdup_printf("share/icons/hicolor/%dx%d/apps/seance.png", size, size);
  gchar *dev = g_strdup_printf("packaging/icons/seance-%d.png", size);
  gchar *r = find_bundled(inst, dev);
  g_free(inst); g_free(dev);
  return r;
}

static void load_app_icons(void) {
  GList *icons = NULL;
  static const int sizes[] = {48, 64, 128, 256};
  for (size_t i = 0; i < G_N_ELEMENTS(sizes); i++) {
    gchar *f = icon_file(sizes[i]);
    if (!f) continue;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(f, NULL);
    if (pb) icons = g_list_append(icons, pb);
    g_free(f);
  }
  if (icons) { gtk_window_set_default_icon_list(icons); g_list_free_full(icons, g_object_unref); }
}

static int install_desktop(gboolean remove_it) {
  gchar *exe = g_file_read_link("/proc/self/exe", NULL);
  if (!exe) { g_printerr("cannot resolve the executable path\n"); return 1; }
  gchar *apps = g_build_filename(g_get_user_data_dir(), "applications", NULL);
  gchar *desk = g_build_filename(apps, "seance.desktop", NULL);
  gchar *icondir = g_build_filename(g_get_user_data_dir(), "icons", "hicolor", "256x256", "apps", NULL);
  gchar *icon = g_build_filename(icondir, "seance.png", NULL);
  int rc = 0;
  if (remove_it) {
    g_unlink(desk); g_unlink(icon);
    g_print("removed %s and %s\n", desk, icon);
  } else {
    g_mkdir_with_parents(apps, 0755);
    g_mkdir_with_parents(icondir, 0755);
    gchar *src = icon_file(256), *data = NULL;
    gsize len = 0;
    if (src && g_file_get_contents(src, &data, &len, NULL)) g_file_set_contents(icon, data, (gssize)len, NULL);
    else g_printerr("note: bundled icon not found, using the generic terminal icon\n");
    g_free(src); g_free(data);
    gboolean have_icon = g_file_test(icon, G_FILE_TEST_EXISTS);
    gchar *content = g_strdup_printf(
        "[Desktop Entry]\nType=Application\nName=Séance\nGenericName=Terminal\n"
        "Comment=Terminal built on the Ghostty core, with agent control\n"
        "Exec=\"%s\"\nTryExec=%s\nIcon=%s\nTerminal=false\nCategories=System;TerminalEmulator;\n"
        "Keywords=shell;prompt;command;commandline;cmd;\nStartupWMClass=seance\n",
        exe, exe, have_icon ? "seance" : "utilities-terminal");
    if (!g_file_set_contents(desk, content, -1, NULL)) { g_printerr("cannot write %s\n", desk); rc = 1; }
    else g_print("installed %s (Exec=%s)\n", desk, exe);
    g_free(content);
  }
  // best-effort refresh of desktop/icon caches (ignore failures)
  gchar *icroot = g_build_filename(g_get_user_data_dir(), "icons", "hicolor", NULL);
  gchar *a1[] = {"update-desktop-database", apps, NULL};
  gchar *a2[] = {"gtk-update-icon-cache", "-q", "-f", "-t", icroot, NULL};
  g_spawn_sync(NULL, a1, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  g_spawn_sync(NULL, a2, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(icroot); g_free(apps); g_free(desk); g_free(icondir); g_free(icon); g_free(exe);
  return rc;
}

// Print the line to add to a shell rc file so that shell integration works from a custom install path.
static int print_shell_hook(const char *shell) {
  if (strcmp(shell, "tcsh") != 0 && strcmp(shell, "csh") != 0) {
    g_printerr("Ghostty's own integration already covers bash, zsh, fish, elvish and nushell (automatic).\nSupported here: tcsh\n");
    return 2;
  }
  gchar *f = find_bundled("share/seance/shell/seance.tcsh", "packaging/shell/seance.tcsh");
  if (!f) { g_printerr("seance.tcsh not found next to this installation\n"); return 1; }
  g_print("if ($?SEANCE_PANE && -f \"%s\") source \"%s\"\n", f, f);
  g_free(f);
  return 0;
}

// On a crash, print a backtrace (host frames are symbolized when linked with -rdynamic) and die normally.
static void crash_handler(int sig, siginfo_t *si, void *uctx) {
  void *frames[64];
  int n = backtrace(frames, 64);
  fprintf(stderr, "\nseance: fatal signal %d, fault addr %p", sig, si ? si->si_addr : NULL);
#ifdef REG_RIP
  if (uctx) {
    void *rip = (void *)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
    Dl_info di;
    fprintf(stderr, ", rip %p", rip);
    if (dladdr(rip, &di)) fprintf(stderr, " (%s+0x%lx %s)", di.dli_fname, (unsigned long)((char *)rip - (char *)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
    if (!rip) {   // call through a NULL pointer: the return address (caller) is at the top of the stack
      void **sp = (void **)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RSP];
      for (int i = 0; i < 6; i++) {
        void *v = sp[i];
        if (dladdr(v, &di) && di.dli_fname)
          fprintf(stderr, "\n  stack[%d]=%p (%s+0x%lx %s)", i, v, di.dli_fname, (unsigned long)((char *)v - (char *)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
      }
    }
  }
#endif
  fprintf(stderr, ", backtrace:\n");
  backtrace_symbols_fd(frames, n, 2);
  signal(sig, SIG_DFL);
  raise(sig);
}

int main(int argc, char **argv) {
  {   // run the crash handler on its own stack so stack overflows still produce a backtrace
    static char altstack[64 * 1024];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {.sa_sigaction = crash_handler, .sa_flags = SA_ONSTACK | SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
  }
  for (int i = 1; i < argc; i++) {   // no display needed for these
    if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--command")) break;
    if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) { puts("seance " SEANCE_VERSION); return 0; }
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
    if (!strcmp(argv[i], "--install-desktop")) return install_desktop(FALSE);
    if (!strcmp(argv[i], "--uninstall-desktop")) return install_desktop(TRUE);
    if (!strcmp(argv[i], "--shell-hook")) return print_shell_hook(i + 1 < argc ? argv[i + 1] : "");
  }
  g_set_prgname("seance");
  gdk_set_program_class("seance");   // WM_CLASS, matches StartupWMClass in the .desktop entry
  gtk_init(&argc, &argv);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--size") && i + 1 < argc) { g_setenv("SEANCE_SIZE", argv[++i], TRUE); continue; }
    if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--command")) {   // rest of argv is the command line
      GString *cmd = g_string_new(NULL);
      for (int k = i + 1; k < argc; k++) { gchar *q = g_shell_quote(argv[k]); g_string_append_printf(cmd, "%s%s", k > i + 1 ? " " : "", q); g_free(q); }
      g_setenv("SEANCE_CMD", cmd->str, TRUE);
      g_string_free(cmd, TRUE);
      argc = i;       // don't hand the command to ghostty_init
      break;
    }
  }
  locate_resources();
  // ALL setenv() calls must happen before ghostty_init: the core captures a slice of the environ block
  // at init, and a later setenv can reallocate it (dangling pointer -> crash in Surface.init).
  start_control_socket();
  if (ghostty_init((uintptr_t)1, argv) != 0) { g_printerr("ghostty_init failed\n"); return 1; }

  {   // decide translucency support before loading config (default screen: compositor + RGBA visual)
    GdkScreen *scr0 = gdk_screen_get_default();
    composited_ok = scr0 && gdk_screen_get_rgba_visual(scr0) && (gdk_screen_is_composited(scr0) || g_getenv("SEANCE_FORCE_RGBA"));
  }
  H.cfg = load_config();

  ghostty_runtime_config_s rt = {
      .userdata = &H,
      .supports_selection_clipboard = true,
      .wakeup_cb = on_wakeup,
      .action_cb = on_action,
      .read_clipboard_cb = on_read_clipboard,
      .confirm_read_clipboard_cb = on_confirm_read,
      .write_clipboard_cb = on_write_clipboard,
      .close_surface_cb = on_close_surface,
  };
  H.app = ghostty_app_new(&rt, H.cfg);
  if (!H.app) { g_printerr("ghostty_app_new failed\n"); return 1; }

  H.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(H.win), "seance");
  {
    int w = 1000, h = 640;   // SEANCE_SIZE=WxH overrides (benchmarks / scripting)
    if (g_getenv("SEANCE_SIZE")) sscanf(g_getenv("SEANCE_SIZE"), "%dx%d", &w, &h);
    gtk_window_set_default_size(GTK_WINDOW(H.win), w, h);
  }
  H.nb = gtk_notebook_new();
  gtk_notebook_set_show_border(GTK_NOTEBOOK(H.nb), FALSE);
  gtk_notebook_set_show_tabs(GTK_NOTEBOOK(H.nb), FALSE);
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(H.nb), TRUE);
  gtk_container_add(GTK_CONTAINER(H.win), H.nb);
  g_signal_connect(H.nb, "switch-page", G_CALLBACK(on_switch_page), NULL);

  g_signal_connect(H.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  g_signal_connect(H.win, "key-press-event", G_CALLBACK(on_key), NULL);
  g_signal_connect(H.win, "key-release-event", G_CALLBACK(on_key), NULL);
  g_signal_connect(H.win, "focus-in-event", G_CALLBACK(on_win_focus), NULL);
  g_signal_connect(H.win, "focus-out-event", G_CALLBACK(on_win_focus), NULL);
  g_unix_signal_add(SIGTERM, (GSourceFunc)gtk_main_quit, NULL);

  {
    // Translucency needs an RGBA visual (before the window is realized) and a running compositor.
    GdkScreen *scr = gtk_widget_get_screen(H.win);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (composited_ok && vis) {
      gtk_widget_set_visual(H.win, vis);
      gtk_widget_set_app_paintable(H.win, TRUE);
    } else {
      composited_ok = FALSE;
    }
    if (g_getenv("SEANCE_STATS")) g_printerr("translucency: rgba visual=%d composited=%d\n", vis != NULL, composited_ok);
  }
  apply_theme();
  load_app_icons();
  {
    GtkWidget *plus = gtk_button_new_with_label("+");
    gtk_button_set_relief(GTK_BUTTON(plus), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(plus, FALSE);
    gtk_widget_set_tooltip_text(plus, "New tab");
    gtk_style_context_add_class(gtk_widget_get_style_context(plus), "seance-newtab");
    g_signal_connect(plus, "clicked", G_CALLBACK(on_menu_new), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(H.nb), plus, GTK_PACK_END);
    gtk_widget_show(plus);
    GtkWidget *burger = gtk_button_new_with_label("☰");
    gtk_button_set_relief(GTK_BUTTON(burger), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(burger, FALSE);
    gtk_widget_set_tooltip_text(burger, "Menu");
    gtk_style_context_add_class(gtk_widget_get_style_context(burger), "seance-newtab");
    g_signal_connect(burger, "clicked", G_CALLBACK(on_hamburger_clicked), NULL);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(H.nb), burger, GTK_PACK_START);
    gtk_widget_show(burger);
  }
  gtk_widget_show_all(H.win);          // realize the window first: pane_new reads its scale factor
  Pane *first = pane_new(NULL, GHOSTTY_SURFACE_CONTEXT_WINDOW);
  if (!first) return 1;
  add_tab(first);
  ghostty_app_set_focus(H.app, true);
  gtk_main();

  if (g_getenv("SEANCE_STATS"))
    g_printerr("host stats: %ld frames taken, %.1f ms avg host-side copy/swizzle\n", H.frames,
               H.frames ? H.frame_us / 1000.0 / H.frames : 0.0);
  if (sock_path) unlink(sock_path);
  // Free every surface before the app: freeing the app with live surfaces crashed in the core.
  for (GList *l = all_panes; l; l = l->next) { Pane *p = l->data; p->closing = TRUE; ghostty_surface_free(p->surface); p->surface = NULL; }
  g_list_free(all_panes);
  all_panes = NULL;
  while (pending_free) free_pane_now((Pane *)pending_free->data);
  ghostty_app_free(H.app);
  return 0;
}

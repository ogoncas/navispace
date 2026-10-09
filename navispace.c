/*
 * NaviSpace - Indicador de espaços de trabalho (XFCE)
 */
#define WNCK_I_KNOW_THIS_IS_UNSTABLE
#include <math.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>
#include <libwnck/libwnck.h>

#define MAX_WORKSPACES   32
#define DEFAULT_ACCENT   "#7aa2f7"
#define ALERT_COLOR      "#e5484d"
#define DEFAULT_ANIM_MS  240
#define MAX_ANIM_MS      800
#define EPSILON          0.002
#define PX_EPSILON       0.05
#define PADDING          4

enum { STYLE_NUMBERS = 0, STYLE_NAMES = 1, STYLE_DOTS = 2 };

typedef struct {
    gint         index;
    gchar       *label;
    gchar       *name;
    gint         n_windows;
    PangoLayout *layout;
    gint         text_w, text_h;

    // Alvos
    gboolean     exists;
    gboolean     want_visible;
    gboolean     is_active;
    gboolean     occupied;
    gboolean     urgent;
    gboolean     hovered;
    gboolean     sized;

    // Valores animados
    gdouble      present;      // 0..1 (fade de visibilidade)
    gdouble      active_t;
    gdouble      occupied_t;
    gdouble      urgent_t;
    gdouble      hover_t;
    gdouble      nlen, ncross;

    // Geometria calculada
    gdouble      pos, len, cross;
} Item;

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget       *area;
    WnckScreen      *screen;
    GPtrArray       *items;

    guint            sync_id;
    guint            tick_id;
    gint64           last_time;
    gdouble          scroll_acc;
    gboolean         ready;
    gboolean         snap_next;
    gint             hover_index;

    // Estado animado global
    gdouble          hl_pos, hl_len, hl_cross, hl_alpha;
    gboolean         hl_ready;
    gint             hl_track_index;
    gdouble          hl_track_pos, hl_track_len;
    gdouble          dots_t;
    gdouble          radius_a, gap_a;
    gdouble          total_main;
    GdkRGBA          fg;
    GdkRGBA          accent,    accent_cur;
    GdkRGBA          accent_fg, accent_fg_cur;
    GdkRGBA          alert;

    // UI de Configuração
    GtkWidget       *dialog;
    GtkWidget       *count_spin;
    GtkWidget       *color_button;

    // Preferências
    gint             style;
    gboolean         hide_empty;
    gboolean         wrap_scroll;
    gboolean         use_theme_color;
    gchar           *accent_hex;
    gint             radius;
    gint             gap;
    gint             anim_ms;
} NaviSpace;

static void navispace_queue_sync(NaviSpace *a, gboolean snap);
static void navispace_animate(NaviSpace *a);

// --- 1. Configurações ---
static void navispace_load(NaviSpace *a) {
    a->style           = STYLE_NUMBERS;
    a->hide_empty      = FALSE;
    a->wrap_scroll     = TRUE;
    a->use_theme_color = FALSE;
    a->accent_hex      = g_strdup(DEFAULT_ACCENT);
    a->radius          = 6;
    a->gap             = 2;
    a->anim_ms         = DEFAULT_ANIM_MS;

    gchar *file = xfce_panel_plugin_lookup_rc_file(a->plugin);
    if (file == NULL)
        return;

    XfceRc *rc = xfce_rc_simple_open(file, TRUE);
    if (rc != NULL) {
        a->style           = xfce_rc_read_int_entry(rc, "style", a->style);
        a->hide_empty      = xfce_rc_read_bool_entry(rc, "hide_empty", a->hide_empty);
        a->wrap_scroll     = xfce_rc_read_bool_entry(rc, "wrap_scroll", a->wrap_scroll);
        a->use_theme_color = xfce_rc_read_bool_entry(rc, "use_theme_color", a->use_theme_color);
        a->radius          = xfce_rc_read_int_entry(rc, "radius", a->radius);
        a->gap             = xfce_rc_read_int_entry(rc, "gap", a->gap);
        a->anim_ms         = xfce_rc_read_int_entry(rc, "anim_ms", a->anim_ms);

        const gchar *color = xfce_rc_read_entry(rc, "accent", DEFAULT_ACCENT);
        g_free(a->accent_hex);
        a->accent_hex = g_strdup(color ? color : DEFAULT_ACCENT);
        xfce_rc_close(rc);
    }
    g_free(file);

    a->style   = CLAMP(a->style, STYLE_NUMBERS, STYLE_DOTS);
    a->radius  = CLAMP(a->radius, 0, 20);
    a->gap     = CLAMP(a->gap, 0, 12);
    a->anim_ms = CLAMP(a->anim_ms, 0, MAX_ANIM_MS);
}

static void navispace_save(XfcePanelPlugin *plugin, NaviSpace *a) {
    gchar *file = xfce_panel_plugin_save_location(plugin, TRUE);
    if (file == NULL)
        return;

    XfceRc *rc = xfce_rc_simple_open(file, FALSE);
    if (rc != NULL) {
        xfce_rc_write_int_entry(rc, "style", a->style);
        xfce_rc_write_bool_entry(rc, "hide_empty", a->hide_empty);
        xfce_rc_write_bool_entry(rc, "wrap_scroll", a->wrap_scroll);
        xfce_rc_write_bool_entry(rc, "use_theme_color", a->use_theme_color);
        xfce_rc_write_entry(rc, "accent", a->accent_hex);
        xfce_rc_write_int_entry(rc, "radius", a->radius);
        xfce_rc_write_int_entry(rc, "gap", a->gap);
        xfce_rc_write_int_entry(rc, "anim_ms", a->anim_ms);
        xfce_rc_close(rc);
    }
    g_free(file);
}

// --- 2. Utilidades ---
static gboolean is_vertical(NaviSpace *a) {
    return xfce_panel_plugin_get_orientation(a->plugin) == GTK_ORIENTATION_VERTICAL;
}

// Força o uso de números em painéis verticais
static gint effective_style(NaviSpace *a) {
    if (a->style == STYLE_NAMES && is_vertical(a))
        return STYLE_NUMBERS;
    return a->style;
}

static gdouble thickness(NaviSpace *a) {
    return CLAMP(xfce_panel_plugin_get_size(a->plugin) - 10, 14, 40);
}

static gdouble dot_size(gdouble thick) {
    return CLAMP((gint)thick / 2, 8, 12);
}

static GdkRGBA rgba_mix(GdkRGBA x, GdkRGBA y, gdouble t) {
    GdkRGBA r;

    t = CLAMP(t, 0.0, 1.0);
    r.red   = x.red   + (y.red   - x.red)   * t;
    r.green = x.green + (y.green - x.green) * t;
    r.blue  = x.blue  + (y.blue  - x.blue)  * t;
    r.alpha = x.alpha + (y.alpha - x.alpha) * t;
    return r;
}

static GdkRGBA rgba_with_alpha(GdkRGBA c, gdouble alpha) {
    c.alpha = alpha;
    return c;
}

static void set_source(cairo_t *cr, GdkRGBA c, gdouble alpha_mul) {
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, CLAMP(c.alpha * alpha_mul, 0.0, 1.0));
}

static gboolean rgba_is_light(const GdkRGBA *c) {
    return (0.2126 * c->red + 0.7152 * c->green + 0.0722 * c->blue) > 0.6;
}

static void rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r) {
    r = MAX(MIN(r, MIN(w, h) / 2.0), 0.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,   G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI,       3 * G_PI / 2);
    cairo_close_path(cr);
}

// Retorna duração em segundos respeitando opções do GTK
static gdouble anim_seconds(NaviSpace *a) {
    gboolean enabled = TRUE;
    GtkSettings *settings = gtk_settings_get_default();

    if (settings != NULL)
        g_object_get(settings, "gtk-enable-animations", &enabled, NULL);
    return enabled ? a->anim_ms / 1000.0 : 0.0;
}

// Suavização exponencial para alcance de alvos animados
static gboolean approach(gdouble *v, gdouble target, gdouble k, gdouble dt, gdouble eps) {
    gdouble diff = target - *v;

    if (k <= 0.0 || fabs(diff) <= eps) {
        *v = target;
        return FALSE;
    }
    *v += diff * (1.0 - exp(-k * dt));
    if (fabs(target - *v) <= eps) {
        *v = target;
        return FALSE;
    }
    return TRUE;
}

static gboolean approach_rgba(GdkRGBA *v, const GdkRGBA *t, gdouble k, gdouble dt) {
    gboolean m = FALSE;

    m |= approach(&v->red,   t->red,   k, dt, EPSILON);
    m |= approach(&v->green, t->green, k, dt, EPSILON);
    m |= approach(&v->blue,  t->blue,  k, dt, EPSILON);
    m |= approach(&v->alpha, t->alpha, k, dt, EPSILON);
    return m;
}

typedef struct {
    gboolean vertical;
    gdouble  origin;
    gdouble  avail_cross;
} Geo;

static Geo navispace_geo(NaviSpace *a) {
    Geo g;
    GtkAllocation al;

    gtk_widget_get_allocation(a->area, &al);
    g.vertical = is_vertical(a);
    gdouble avail_main = g.vertical ? al.height : al.width;
    g.avail_cross = g.vertical ? al.width : al.height;
    g.origin = (avail_main - a->total_main) / 2.0;
    return g;
}

static void geo_rect(const Geo *g, gdouble pos, gdouble len, gdouble cross,
                     gdouble *x, gdouble *y, gdouble *w, gdouble *h) {
    gdouble m = g->origin + pos;
    gdouble c = floor((g->avail_cross - cross) / 2.0);

    if (g->vertical) { *x = c; *y = m; *w = cross; *h = len; }
    else             { *x = m; *y = c; *w = len;   *h = cross; }
}

// --- 3. Cores ---
static void navispace_update_colors(NaviSpace *a, gboolean snap) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(a->area);

    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &a->fg);
    a->fg.alpha = 1.0;

    if (a->use_theme_color) {
        if (!gtk_style_context_lookup_color(ctx, "theme_selected_bg_color", &a->accent))
            gdk_rgba_parse(&a->accent, DEFAULT_ACCENT);
        if (!gtk_style_context_lookup_color(ctx, "theme_selected_fg_color", &a->accent_fg))
            gdk_rgba_parse(&a->accent_fg, "#ffffff");
    } else {
        if (!gdk_rgba_parse(&a->accent, a->accent_hex))
            gdk_rgba_parse(&a->accent, DEFAULT_ACCENT);
        gdk_rgba_parse(&a->accent_fg, rgba_is_light(&a->accent) ? "#1a1b26" : "#ffffff");
    }
    a->accent.alpha = 1.0;
    a->accent_fg.alpha = 1.0;
    gdk_rgba_parse(&a->alert, ALERT_COLOR);

    if (snap) {
        a->accent_cur = a->accent;
        a->accent_fg_cur = a->accent_fg;
    }
}

// --- 4. Itens ---
static void item_free(Item *it) {
    g_clear_object(&it->layout);
    g_free(it->label);
    g_free(it->name);
    g_slice_free(Item, it);
}

static gint item_cmp(gconstpointer pa, gconstpointer pb) {
    const Item *x = *(Item *const *)pa;
    const Item *y = *(Item *const *)pb;

    return x->index - y->index;
}

static Item *item_find(NaviSpace *a, gint idx) {
    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        if (it->index == idx)
            return it;
    }
    return NULL;
}

static void item_set_label(Item *it, const gchar *label) {
    if (g_strcmp0(it->label, label) == 0)
        return;
    g_free(it->label);
    it->label = g_strdup(label);
    g_clear_object(&it->layout);
    it->text_w = it->text_h = 0;
}

static void item_ensure_layout(NaviSpace *a, Item *it) {
    if (it->layout != NULL || it->label == NULL || it->label[0] == '\0')
        return;

    it->layout = gtk_widget_create_pango_layout(a->area, it->label);

    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD));
    pango_layout_set_attributes(it->layout, attrs);
    pango_attr_list_unref(attrs);

    pango_layout_get_pixel_size(it->layout, &it->text_w, &it->text_h);
}

// --- 5. Motor de Animação ---
static void navispace_layout(NaviSpace *a) {
    gdouble pos = 0.0, last_gap = 0.0;

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        gdouble gap = a->gap_a * it->present;

        it->len   = it->nlen * it->present;
        it->cross = it->ncross;
        it->pos   = pos;
        pos += it->len + gap;
        if (it->present > 0.001)
            last_gap = gap;
    }
    a->total_main = MAX(pos - last_gap, 0.0);
}

// Avança quadros e recalcula posições
static gboolean navispace_step(NaviSpace *a, gdouble dt, gdouble k) {
    gboolean moving = FALSE;
    gint style = effective_style(a);
    gdouble thick = thickness(a);
    gdouble dot = dot_size(thick);
    gdouble pill = dot * 2 + 4;

    moving |= approach(&a->dots_t, style == STYLE_DOTS ? 1.0 : 0.0, k, dt, EPSILON);
    moving |= approach(&a->radius_a, a->radius, k, dt, 0.02);
    moving |= approach(&a->gap_a, a->gap, k, dt, 0.02);
    moving |= approach_rgba(&a->accent_cur, &a->accent, k, dt);
    moving |= approach_rgba(&a->accent_fg_cur, &a->accent_fg, k, dt);

    for (guint i = 0; i < a->items->len; ) {
        Item *it = g_ptr_array_index(a->items, i);
        gdouble tlen, tcross;

        item_ensure_layout(a, it);

        if (style == STYLE_DOTS) {
            tlen = it->is_active ? pill : dot;
            tcross = dot;
        } else {
            gdouble pad = (style == STYLE_NAMES) ? 10 : 6;
            tlen = MAX(thick, it->text_w + 2 * pad);
            tcross = thick;
        }
        if (!it->sized) {
            it->nlen = tlen;
            it->ncross = tcross;
            it->sized = TRUE;
        }

        moving |= approach(&it->nlen,   tlen,   k, dt, PX_EPSILON);
        moving |= approach(&it->ncross, tcross, k, dt, PX_EPSILON);
        moving |= approach(&it->present,    it->want_visible ? 1.0 : 0.0, k, dt, EPSILON);
        moving |= approach(&it->active_t,   it->is_active    ? 1.0 : 0.0, k, dt, EPSILON);
        moving |= approach(&it->occupied_t, it->occupied     ? 1.0 : 0.0, k, dt, EPSILON);
        moving |= approach(&it->urgent_t,   it->urgent       ? 1.0 : 0.0, k, dt, EPSILON);
        moving |= approach(&it->hover_t,    it->hovered      ? 1.0 : 0.0, k, dt, EPSILON);

        if (!it->exists && it->present <= 0.0) {
            g_ptr_array_remove_index(a->items, i);
            continue;
        }
        i++;
    }

    navispace_layout(a);

    // Rastrea o item ativo mantendo coesão de layout
    Item *act = NULL;
    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        if (it->exists && it->is_active) {
            act = it;
            break;
        }
    }
    if (act != NULL) {
        if (!a->hl_ready) {
            a->hl_pos = act->pos;
            a->hl_len = act->len;
            a->hl_ready = TRUE;
        } else if (act->index == a->hl_track_index) {
            a->hl_pos += act->pos - a->hl_track_pos;
            a->hl_len += act->len - a->hl_track_len;
        }
        a->hl_track_index = act->index;
        a->hl_track_pos   = act->pos;
        a->hl_track_len   = act->len;
        moving |= approach(&a->hl_pos, act->pos, k, dt, PX_EPSILON);
        moving |= approach(&a->hl_len, act->len, k, dt, PX_EPSILON);
        a->hl_cross = act->cross;
    }
    moving |= approach(&a->hl_alpha, (act != NULL && style != STYLE_DOTS) ? 1.0 : 0.0,
                       k, dt, EPSILON);
    return moving;
}

static void navispace_apply_size(NaviSpace *a) {
    gint size = xfce_panel_plugin_get_size(a->plugin);
    gint main = (gint)ceil(a->total_main) + 2 * PADDING;

    if (is_vertical(a))
        gtk_widget_set_size_request(a->area, size, main);
    else
        gtk_widget_set_size_request(a->area, main, size);
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
    NaviSpace *a = data;

    if (a->area == NULL)
        return G_SOURCE_REMOVE;

    gint64 now = gdk_frame_clock_get_frame_time(clock);
    gdouble dt = a->last_time ? (now - a->last_time) / 1000000.0 : 1.0 / 60.0;
    a->last_time = now;
    dt = CLAMP(dt, 0.0, 0.1);

    gdouble secs = anim_seconds(a);
    gboolean moving = navispace_step(a, dt, secs > 0.0 ? 3.0 / secs : 0.0);

    navispace_apply_size(a);
    gtk_widget_queue_draw(w);

    if (!moving) {
        a->tick_id = 0;
        a->last_time = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void navispace_animate(NaviSpace *a) {
    if (a->area == NULL)
        return;

    // Redireciona renderização sem cálculo interpolado se animações estiverem nativamente inativas
    if (anim_seconds(a) <= 0.0 || !gtk_widget_get_mapped(a->area)) {
        navispace_step(a, 0.0, 0.0);
        navispace_apply_size(a);
        gtk_widget_queue_draw(a->area);
        return;
    }
    if (a->tick_id == 0) {
        a->last_time = 0;
        a->tick_id = gtk_widget_add_tick_callback(a->area, on_tick, a, NULL);
    }
    gtk_widget_queue_draw(a->area);
}

// --- 6. Sincronização WNCK ---
static void on_workspace_name_changed(WnckWorkspace *ws, gpointer data) {
    navispace_queue_sync(data, FALSE);
}

static gboolean window_counts(WnckWindow *w) {
    WnckWindowType t = wnck_window_get_window_type(w);

    if (t == WNCK_WINDOW_DESKTOP || t == WNCK_WINDOW_DOCK)
        return FALSE;
    return !wnck_window_is_skip_tasklist(w);
}

static void navispace_sync(NaviSpace *a) {
    gint style = effective_style(a);
    GList *workspaces = wnck_screen_get_workspaces(a->screen);
    gint count = (gint)g_list_length(workspaces);
    gint n = MAX(count, 1);
    gint *counts = g_new0(gint, n);
    gboolean *urgent = g_new0(gboolean, n);
    WnckWorkspace *active = wnck_screen_get_active_workspace(a->screen);

    for (GList *l = wnck_screen_get_windows(a->screen); l != NULL; l = l->next) {
        WnckWindow *win = WNCK_WINDOW(l->data);
        WnckWorkspace *ws = wnck_window_get_workspace(win);

        if (ws == NULL || !window_counts(win))
            continue;
        gint idx = wnck_workspace_get_number(ws);
        if (idx < 0 || idx >= count)
            continue;
        counts[idx]++;
        if (wnck_window_needs_attention(win))
            urgent[idx] = TRUE;
    }

    for (guint i = 0; i < a->items->len; i++)
        ((Item *)g_ptr_array_index(a->items, i))->exists = FALSE;

    for (GList *l = workspaces; l != NULL; l = l->next) {
        WnckWorkspace *ws = WNCK_WORKSPACE(l->data);
        gint idx = wnck_workspace_get_number(ws);
        Item *it = item_find(a, idx);
        gboolean created = FALSE;

        if (it == NULL) {
            it = g_slice_new0(Item);
            it->index = idx;
            g_ptr_array_add(a->items, it);
            g_signal_connect(ws, "name-changed", G_CALLBACK(on_workspace_name_changed), a);
            created = TRUE;
        }

        it->exists    = TRUE;
        it->n_windows = counts[idx];
        it->is_active = (ws == active);
        it->occupied  = counts[idx] > 0;
        it->urgent    = urgent[idx] && !it->is_active;

        g_free(it->name);
        it->name = g_strdup(wnck_workspace_get_name(ws));

        if (style == STYLE_NAMES) {
            item_set_label(it, it->name);
        } else {
            gchar *num = g_strdup_printf("%d", idx + 1);
            item_set_label(it, num);
            g_free(num);
        }

        it->want_visible = !a->hide_empty || it->occupied || it->is_active;
        if (created)
            it->present = (a->ready || !it->want_visible) ? 0.0 : 1.0;
    }

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        if (!it->exists) {
            it->want_visible = FALSE;
            it->is_active = FALSE;
            it->occupied = FALSE;
            it->urgent = FALSE;
        }
    }
    g_ptr_array_sort(a->items, item_cmp);

    g_free(counts);
    g_free(urgent);

    gboolean snap = !a->ready || a->snap_next;
    a->snap_next = FALSE;
    if (snap) {
        navispace_step(a, 0.0, 0.0);
        a->ready = TRUE;
        navispace_apply_size(a);
        gtk_widget_queue_draw(a->area);
    } else {
        navispace_animate(a);
    }
}

// Consolida múltiplos eventos em processamento de frame singular
static gboolean sync_idle(gpointer data) {
    NaviSpace *a = data;

    a->sync_id = 0;
    if (a->area != NULL)
        navispace_sync(a);
    return G_SOURCE_REMOVE;
}

static void navispace_queue_sync(NaviSpace *a, gboolean snap) {
    if (a->area == NULL)
        return;
    if (snap)
        a->snap_next = TRUE;
    if (a->sync_id == 0)
        a->sync_id = g_idle_add(sync_idle, a);
}

static void on_window_workspace_changed(WnckWindow *win, gpointer data) {
    navispace_queue_sync(data, FALSE);
}

static void on_window_state_changed(WnckWindow *win, WnckWindowState changed,
                                    WnckWindowState state, gpointer data) {
    navispace_queue_sync(data, FALSE);
}

static void watch_window(NaviSpace *a, WnckWindow *win) {
    g_signal_handlers_disconnect_by_data(win, a);
    g_signal_connect(win, "workspace-changed", G_CALLBACK(on_window_workspace_changed), a);
    g_signal_connect(win, "state-changed", G_CALLBACK(on_window_state_changed), a);
}

static void on_window_opened(WnckScreen *screen, WnckWindow *win, NaviSpace *a) {
    watch_window(a, win);
    navispace_queue_sync(a, FALSE);
}

static void on_window_closed(WnckScreen *screen, WnckWindow *win, NaviSpace *a) {
    navispace_queue_sync(a, FALSE);
}

static void on_active_workspace_changed(WnckScreen *screen, WnckWorkspace *prev, NaviSpace *a) {
    navispace_queue_sync(a, FALSE);
}

static void on_workspace_count_changed(WnckScreen *screen, WnckWorkspace *ws, NaviSpace *a) {
    if (a->count_spin != NULL)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->count_spin),
                                  wnck_screen_get_workspace_count(screen));
    navispace_queue_sync(a, FALSE);
}

// --- 7. Renderização ---
static gboolean on_draw(GtkWidget *w, cairo_t *cr, NaviSpace *a) {
    Geo g = navispace_geo(a);
    gdouble text_vis = 1.0 - a->dots_t;
    gdouble dot_vis  = a->dots_t;
    gdouble x, y, wd, ht;

    if (a->hl_alpha > 0.005 && a->hl_len > 0.5) {
        geo_rect(&g, a->hl_pos, a->hl_len, a->hl_cross, &x, &y, &wd, &ht);
        rounded_rect(cr, x, y, wd, ht, a->radius_a);
        set_source(cr, a->accent_cur, a->hl_alpha);
        cairo_fill(cr);
    }

    GdkRGBA white = { 1, 1, 1, 1 };

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);

        if (it->present < 0.01 || it->len < 0.5)
            continue;

        geo_rect(&g, it->pos, it->len, it->cross, &x, &y, &wd, &ht);

        gdouble cov = 0.0;
        gdouble o0 = MAX(it->pos, a->hl_pos);
        gdouble o1 = MIN(it->pos + it->len, a->hl_pos + a->hl_len);
        if (o1 > o0)
            cov = CLAMP((o1 - o0) / it->len, 0.0, 1.0) * a->hl_alpha;

        if (text_vis > 0.01) {
            gdouble vis = text_vis * it->present;

            if (it->hover_t > 0.01) {
                rounded_rect(cr, x, y, wd, ht, a->radius_a);
                set_source(cr, rgba_with_alpha(a->fg, 0.12), it->hover_t * (1.0 - cov) * vis);
                cairo_fill(cr);
            }
            if (it->urgent_t > 0.01) {
                rounded_rect(cr, x, y, wd, ht, a->radius_a);
                set_source(cr, a->alert, it->urgent_t * vis);
                cairo_fill(cr);
            }

            gdouble bar = it->occupied_t * (1.0 - cov) * (1.0 - it->urgent_t);
            if (bar > 0.01) {
                gdouble bw = wd * 0.4 * bar;
                gdouble bx = x + (wd - bw) / 2.0, by = y + ht - 4;
                rounded_rect(cr, bx, by, bw, 2, 1);
                set_source(cr, rgba_with_alpha(a->accent_cur, 0.75), vis);
                cairo_fill(cr);
            }

            if (it->layout != NULL) {
                GdkRGBA dim  = rgba_with_alpha(a->fg, 0.45);
                GdkRGBA full = rgba_with_alpha(a->fg, 0.95);
                GdkRGBA c = rgba_mix(dim, full, it->occupied_t);
                c = rgba_mix(c, rgba_with_alpha(a->fg, 1.0), it->hover_t);
                c = rgba_mix(c, a->accent_fg_cur, cov);
                c = rgba_mix(c, white, it->urgent_t);

                cairo_save(cr);
                cairo_rectangle(cr, x, y, wd, ht);
                cairo_clip(cr);
                set_source(cr, c, vis);
                cairo_move_to(cr, floor(x + (wd - it->text_w) / 2.0),
                                  floor(y + (ht - it->text_h) / 2.0));
                pango_cairo_show_layout(cr, it->layout);
                cairo_restore(cr);
            }
        }

        if (dot_vis > 0.01) {
            GdkRGBA c = rgba_mix(rgba_with_alpha(a->fg, 0.30),
                                 rgba_with_alpha(a->fg, 0.65), it->occupied_t);
            c = rgba_mix(c, rgba_with_alpha(a->fg, 0.90), it->hover_t * (1.0 - it->active_t));
            c = rgba_mix(c, a->accent_cur, it->active_t);
            c = rgba_mix(c, a->alert, it->urgent_t);

            rounded_rect(cr, x, y, wd, ht, MIN(wd, ht) / 2.0);
            set_source(cr, c, dot_vis * it->present);
            cairo_fill(cr);
        }
    }
    return FALSE;
}

// --- 8. Eventos de UI ---
static gint item_at(NaviSpace *a, gdouble ex, gdouble ey) {
    Geo g = navispace_geo(a);
    gdouble m = (g.vertical ? ey : ex) - g.origin;
    gdouble half = a->gap_a / 2.0;

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);

        if (!it->exists || it->present < 0.5)
            continue;
        if (m >= it->pos - half && m < it->pos + it->len + half)
            return it->index;
    }
    return -1;
}

static void set_hover(NaviSpace *a, gint idx) {
    if (a->hover_index == idx)
        return;
    a->hover_index = idx;
    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        it->hovered = (it->index == idx);
    }
    navispace_animate(a);
    gtk_tooltip_trigger_tooltip_query(gtk_widget_get_display(a->area));
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, NaviSpace *a) {
    set_hover(a, item_at(a, ev->x, ev->y));
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, NaviSpace *a) {
    set_hover(a, -1);
    return FALSE;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, NaviSpace *a) {
    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS)
        return FALSE;

    gint idx = item_at(a, ev->x, ev->y);
    if (idx < 0)
        return FALSE;

    WnckWorkspace *ws = wnck_screen_get_workspace(a->screen, idx);
    if (ws != NULL)
        wnck_workspace_activate(ws, ev->time);
    return TRUE;
}

static gboolean on_query_tooltip(GtkWidget *w, gint x, gint y, gboolean keyboard,
                                 GtkTooltip *tooltip, NaviSpace *a) {
    Item *it = item_find(a, item_at(a, x, y));
    gchar *text;

    if (it == NULL)
        return FALSE;

    if (it->n_windows == 0)
        text = g_strdup_printf("%s — vazio", it->name);
    else if (it->n_windows == 1)
        text = g_strdup_printf("%s — 1 janela", it->name);
    else
        text = g_strdup_printf("%s — %d janelas", it->name, it->n_windows);

    gtk_tooltip_set_text(tooltip, text);
    g_free(text);
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, NaviSpace *a) {
    gint dir = 0;

    switch (ev->direction) {
    case GDK_SCROLL_UP:
    case GDK_SCROLL_LEFT:
        dir = -1;
        break;
    case GDK_SCROLL_DOWN:
    case GDK_SCROLL_RIGHT:
        dir = 1;
        break;
    case GDK_SCROLL_SMOOTH:
        a->scroll_acc += ev->delta_x + ev->delta_y;
        if (a->scroll_acc <= -1.0)     { dir = -1; a->scroll_acc = 0; }
        else if (a->scroll_acc >= 1.0) { dir = 1;  a->scroll_acc = 0; }
        break;
    default:
        return FALSE;
    }
    if (dir == 0)
        return TRUE;

    WnckWorkspace *active = wnck_screen_get_active_workspace(a->screen);
    if (active == NULL)
        return TRUE;

    gint count = wnck_screen_get_workspace_count(a->screen);
    gint cur = wnck_workspace_get_number(active);
    gint next = cur + dir;

    if (next < 0)
        next = a->wrap_scroll ? count - 1 : cur;
    else if (next >= count)
        next = a->wrap_scroll ? 0 : cur;

    if (next != cur) {
        WnckWorkspace *ws = wnck_screen_get_workspace(a->screen, next);
        if (ws != NULL)
            wnck_workspace_activate(ws, ev->time);
    }
    return TRUE;
}

// --- 9. Callbacks do Painel ---
static void on_style_updated(GtkWidget *w, NaviSpace *a) {
    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        g_clear_object(&it->layout);
        it->text_w = it->text_h = 0;
    }
    navispace_update_colors(a, TRUE);
    navispace_queue_sync(a, TRUE);
}

static void on_realize(GtkWidget *w, NaviSpace *a) {
    navispace_update_colors(a, TRUE);
    navispace_queue_sync(a, TRUE);
}

static gboolean on_size_changed(XfcePanelPlugin *plugin, gint size, NaviSpace *a) {
    navispace_queue_sync(a, FALSE);
    navispace_apply_size(a);
    return TRUE;
}

static void on_orientation_changed(XfcePanelPlugin *plugin, GtkOrientation o, NaviSpace *a) {
    navispace_queue_sync(a, TRUE);
    navispace_apply_size(a);
}

static void on_area_destroy(GtkWidget *w, NaviSpace *a) {
    if (a->sync_id != 0) {
        g_source_remove(a->sync_id);
        a->sync_id = 0;
    }
    a->tick_id = 0;
    a->area = NULL;
}

// --- 10. Menu de Configuração ---
static void apply_and_save(NaviSpace *a) {
    navispace_update_colors(a, FALSE);
    navispace_queue_sync(a, FALSE);
    navispace_save(a->plugin, a);
}

static void on_count_changed(GtkSpinButton *b, NaviSpace *a) {
    gint wanted = gtk_spin_button_get_value_as_int(b);

    if (wanted != wnck_screen_get_workspace_count(a->screen))
        wnck_screen_change_workspace_count(a->screen, wanted);
}

static void on_style_changed(GtkComboBox *c, NaviSpace *a) {
    a->style = gtk_combo_box_get_active(c);
    apply_and_save(a);
}

static void on_hide_empty_toggled(GtkToggleButton *b, NaviSpace *a) {
    a->hide_empty = gtk_toggle_button_get_active(b);
    apply_and_save(a);
}

static void on_wrap_toggled(GtkToggleButton *b, NaviSpace *a) {
    a->wrap_scroll = gtk_toggle_button_get_active(b);
    navispace_save(a->plugin, a);
}

static void on_theme_color_toggled(GtkToggleButton *b, NaviSpace *a) {
    a->use_theme_color = gtk_toggle_button_get_active(b);
    if (a->color_button != NULL)
        gtk_widget_set_sensitive(a->color_button, !a->use_theme_color);
    apply_and_save(a);
}

static void on_color_set(GtkColorButton *b, NaviSpace *a) {
    GdkRGBA rgba;

    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b), &rgba);
    g_free(a->accent_hex);
    a->accent_hex = gdk_rgba_to_string(&rgba);
    apply_and_save(a);
}

static void on_radius_changed(GtkSpinButton *b, NaviSpace *a) {
    a->radius = gtk_spin_button_get_value_as_int(b);
    apply_and_save(a);
}

static void on_gap_changed(GtkSpinButton *b, NaviSpace *a) {
    a->gap = gtk_spin_button_get_value_as_int(b);
    apply_and_save(a);
}

static void on_anim_changed(GtkSpinButton *b, NaviSpace *a) {
    a->anim_ms = gtk_spin_button_get_value_as_int(b);
    navispace_save(a->plugin, a);
}

static void on_dialog_response(GtkWidget *dialog, gint response, NaviSpace *a) {
    gtk_widget_destroy(dialog);
}

static void on_dialog_destroyed(GtkWidget *dialog, NaviSpace *a) {
    a->dialog       = NULL;
    a->count_spin   = NULL;
    a->color_button = NULL;
    xfce_panel_plugin_unblock_menu(a->plugin);
}

static GtkWidget *section_title(const gchar *text) {
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);

    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    g_free(markup);
    return label;
}

static GtkWidget *row_label(const gchar *text) {
    GtkWidget *label = gtk_label_new_with_mnemonic(text);

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 12);
    return label;
}

static GtkWidget *indented(GtkWidget *w) {
    gtk_widget_set_margin_start(w, 12);
    return w;
}

static void navispace_configure(XfcePanelPlugin *plugin, NaviSpace *a) {
    if (a->dialog != NULL) {
        gtk_window_present(GTK_WINDOW(a->dialog));
        return;
    }

    xfce_panel_plugin_block_menu(plugin);

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Configurações do NaviSpace",
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin))),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Fechar", GTK_RESPONSE_CLOSE, NULL);
    a->dialog = dialog;

    gtk_window_set_icon_name(GTK_WINDOW(dialog), "preferences-desktop-workspaces");
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    xfce_panel_plugin_take_window(plugin, GTK_WINDOW(dialog));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 18);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       grid, TRUE, TRUE, 0);

    gint row = 0;

    gtk_grid_attach(GTK_GRID(grid), section_title("Espaços de trabalho"), 0, row++, 2, 1);

    GtkWidget *l_count = row_label("_Quantidade:");
    a->count_spin = gtk_spin_button_new_with_range(1, MAX_WORKSPACES, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->count_spin),
                              wnck_screen_get_workspace_count(a->screen));
    gtk_label_set_mnemonic_widget(GTK_LABEL(l_count), a->count_spin);
    g_signal_connect(a->count_spin, "value-changed", G_CALLBACK(on_count_changed), a);
    gtk_grid_attach(GTK_GRID(grid), l_count, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->count_spin, 1, row++, 1, 1);

    GtkWidget *c_empty = indented(gtk_check_button_new_with_mnemonic("Ocultar espaços _vazios"));
    gtk_widget_set_tooltip_text(c_empty, "Mostra só o espaço ativo e os que têm janelas");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_empty), a->hide_empty);
    g_signal_connect(c_empty, "toggled", G_CALLBACK(on_hide_empty_toggled), a);
    gtk_grid_attach(GTK_GRID(grid), c_empty, 0, row++, 2, 1);

    GtkWidget *c_wrap = indented(gtk_check_button_new_with_mnemonic("Roda do mouse dá a _volta (último ↔ primeiro)"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_wrap), a->wrap_scroll);
    g_signal_connect(c_wrap, "toggled", G_CALLBACK(on_wrap_toggled), a);
    gtk_grid_attach(GTK_GRID(grid), c_wrap, 0, row++, 2, 1);

    GtkWidget *t_look = section_title("Aparência");
    gtk_widget_set_margin_top(t_look, 8);
    gtk_grid_attach(GTK_GRID(grid), t_look, 0, row++, 2, 1);

    GtkWidget *l_style = row_label("_Estilo:");
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Números (1 2 3)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Nomes");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Pontos (pílula)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), a->style);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l_style), combo);
    g_signal_connect(combo, "changed", G_CALLBACK(on_style_changed), a);
    gtk_grid_attach(GTK_GRID(grid), l_style, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), combo, 1, row++, 1, 1);

    GtkWidget *c_theme = indented(gtk_check_button_new_with_mnemonic("Usar cor de destaque do _tema"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c_theme), a->use_theme_color);
    g_signal_connect(c_theme, "toggled", G_CALLBACK(on_theme_color_toggled), a);
    gtk_grid_attach(GTK_GRID(grid), c_theme, 0, row++, 2, 1);

    GtkWidget *l_color = row_label("Cor de _destaque:");
    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, a->accent_hex))
        gdk_rgba_parse(&rgba, DEFAULT_ACCENT);
    a->color_button = gtk_color_button_new_with_rgba(&rgba);
    gtk_widget_set_sensitive(a->color_button, !a->use_theme_color);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l_color), a->color_button);
    g_signal_connect(a->color_button, "color-set", G_CALLBACK(on_color_set), a);
    gtk_grid_attach(GTK_GRID(grid), l_color, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), a->color_button, 1, row++, 1, 1);

    GtkWidget *l_radius = row_label("_Arredondamento:");
    GtkWidget *s_radius = gtk_spin_button_new_with_range(0, 20, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_radius), a->radius);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l_radius), s_radius);
    g_signal_connect(s_radius, "value-changed", G_CALLBACK(on_radius_changed), a);
    gtk_grid_attach(GTK_GRID(grid), l_radius, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_radius, 1, row++, 1, 1);

    GtkWidget *l_gap = row_label("E_spaçamento:");
    GtkWidget *s_gap = gtk_spin_button_new_with_range(0, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_gap), a->gap);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l_gap), s_gap);
    g_signal_connect(s_gap, "value-changed", G_CALLBACK(on_gap_changed), a);
    gtk_grid_attach(GTK_GRID(grid), l_gap, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_gap, 1, row++, 1, 1);

    GtkWidget *t_anim = section_title("Animação");
    gtk_widget_set_margin_top(t_anim, 8);
    gtk_grid_attach(GTK_GRID(grid), t_anim, 0, row++, 2, 1);

    GtkWidget *l_anim = row_label("_Duração (ms):");
    GtkWidget *s_anim = gtk_spin_button_new_with_range(0, MAX_ANIM_MS, 20);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s_anim), a->anim_ms);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l_anim), s_anim);
    g_signal_connect(s_anim, "value-changed", G_CALLBACK(on_anim_changed), a);
    gtk_grid_attach(GTK_GRID(grid), l_anim, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s_anim, 1, row++, 1, 1);

    g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response), a);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_dialog_destroyed), a);
    gtk_widget_show_all(dialog);
}

// --- 11. Limpeza de Memória ---
static void navispace_free(XfcePanelPlugin *plugin, NaviSpace *a) {
    if (a->dialog != NULL)
        gtk_widget_destroy(a->dialog);

    if (a->sync_id != 0)
        g_source_remove(a->sync_id);

    if (a->area != NULL) {
        g_signal_handlers_disconnect_by_data(a->area, a);
        if (a->tick_id != 0)
            gtk_widget_remove_tick_callback(a->area, a->tick_id);
    }

    if (a->screen != NULL) {
        g_signal_handlers_disconnect_by_data(a->screen, a);
        for (GList *l = wnck_screen_get_windows(a->screen); l != NULL; l = l->next)
            g_signal_handlers_disconnect_by_data(l->data, a);
        for (GList *l = wnck_screen_get_workspaces(a->screen); l != NULL; l = l->next)
            g_signal_handlers_disconnect_by_data(l->data, a);
    }

    if (a->items != NULL)
        g_ptr_array_free(a->items, TRUE);

    g_free(a->accent_hex);
    g_slice_free(NaviSpace, a);
}

// --- 12. Construtor ---
static void navispace_construct(XfcePanelPlugin *plugin) {
    NaviSpace *a = g_slice_new0(NaviSpace);
    a->plugin = plugin;
    a->hover_index = -1;
    a->items = g_ptr_array_new_with_free_func((GDestroyNotify)item_free);
    navispace_load(a);

    wnck_set_client_type(WNCK_CLIENT_TYPE_PAGER);
    a->screen = wnck_screen_get_default();

    if (a->screen == NULL) {
        GtkWidget *label = gtk_label_new("NaviSpace indisponível");
        gtk_widget_set_tooltip_text(label,
            "Requer uma sessão X11 com suporte a espaços de trabalho");
        gtk_container_add(GTK_CONTAINER(plugin), label);
        gtk_widget_show_all(GTK_WIDGET(plugin));
        g_signal_connect(plugin, "free-data", G_CALLBACK(navispace_free), a);
        return;
    }
    wnck_screen_force_update(a->screen);

    a->radius_a = a->radius;
    a->gap_a = a->gap;
    a->dots_t = (effective_style(a) == STYLE_DOTS) ? 1.0 : 0.0;

    a->area = gtk_drawing_area_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(a->area), "navispace");
    gtk_widget_add_events(a->area,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK |
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_widget_set_has_tooltip(a->area, TRUE);
    gtk_container_add(GTK_CONTAINER(plugin), a->area);

    navispace_update_colors(a, TRUE);

    g_signal_connect(a->area, "draw",                G_CALLBACK(on_draw),         a);
    g_signal_connect(a->area, "button-press-event",  G_CALLBACK(on_button_press), a);
    g_signal_connect(a->area, "motion-notify-event", G_CALLBACK(on_motion),       a);
    g_signal_connect(a->area, "leave-notify-event",  G_CALLBACK(on_leave),        a);
    g_signal_connect(a->area, "scroll-event",        G_CALLBACK(on_scroll),       a);
    g_signal_connect(a->area, "query-tooltip",       G_CALLBACK(on_query_tooltip), a);
    g_signal_connect(a->area, "style-updated",       G_CALLBACK(on_style_updated), a);
    g_signal_connect(a->area, "realize",             G_CALLBACK(on_realize),      a);
    g_signal_connect(a->area, "destroy",             G_CALLBACK(on_area_destroy), a);

    for (GList *l = wnck_screen_get_windows(a->screen); l != NULL; l = l->next)
        watch_window(a, WNCK_WINDOW(l->data));

    g_signal_connect(a->screen, "active-workspace-changed", G_CALLBACK(on_active_workspace_changed), a);
    g_signal_connect(a->screen, "workspace-created",   G_CALLBACK(on_workspace_count_changed), a);
    g_signal_connect(a->screen, "workspace-destroyed", G_CALLBACK(on_workspace_count_changed), a);
    g_signal_connect(a->screen, "window-opened", G_CALLBACK(on_window_opened), a);
    g_signal_connect(a->screen, "window-closed", G_CALLBACK(on_window_closed), a);

    navispace_sync(a);
    gtk_widget_show_all(GTK_WIDGET(plugin));

    xfce_panel_plugin_add_action_widget(plugin, a->area);
    xfce_panel_plugin_menu_show_configure(plugin);

    g_signal_connect(plugin, "configure-plugin",    G_CALLBACK(navispace_configure),    a);
    g_signal_connect(plugin, "save",                G_CALLBACK(navispace_save),         a);
    g_signal_connect(plugin, "free-data",           G_CALLBACK(navispace_free),         a);
    g_signal_connect(plugin, "size-changed",        G_CALLBACK(on_size_changed),        a);
    g_signal_connect(plugin, "orientation-changed", G_CALLBACK(on_orientation_changed), a);
}

XFCE_PANEL_PLUGIN_REGISTER(navispace_construct);
#define WNCK_I_KNOW_THIS_IS_UNSTABLE
#include <math.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>
#include <libwnck/libwnck.h>

#define MAX_WORKSPACES  32
#define DEFAULT_ACCENT  "#7aa2f7"
#define DEFAULT_ANIM_MS 240
#define MAX_ANIM_MS     800
#define EPSILON         0.002
#define PX_EPSILON      0.1
#define PADDING         4
#define BAR_THICKNESS   4
#define PULSE_PERIOD    1.4

/* Frequência natural (rad/s) = taxa / duração; o amortecimento crítico chega a ~98% na duração escolhida */
#define SETTLE_RATE     5.8
#define SPRING_GAIN     1.26
#define SPRING_ZETA     0.55
#define LEAD_GAIN       1.5
#define TRAIL_GAIN      0.55

enum { STYLE_NUMBERS, STYLE_NAMES, STYLE_ROMAN, STYLE_DOTS, STYLE_BARS, STYLE_COUNT };

enum { IND_FILL, IND_OUTLINE, IND_UNDERLINE, IND_COUNT };

enum { MARK_NONE, MARK_BAR, MARK_DOT, MARK_COUNT };

enum { MOTION_SLIDE, MOTION_STRETCH, MOTION_SPRING, MOTION_FADE, MOTION_COUNT };

static const GdkRGBA ALERT = { 0.898, 0.282, 0.302, 1.0 };
static const GdkRGBA WHITE = { 1.0, 1.0, 1.0, 1.0 };

/* Valor animado com velocidade */
typedef struct {
    gdouble x, v;
} Anim;

typedef struct {
    gint         index;
    gchar       *label;
    gchar       *name;
    gint         n_windows;
    PangoLayout *layout;
    gint         text_w, text_h;

    gboolean     exists, want_visible, is_active, occupied, urgent, hovered, sized;

    Anim         present, active_t, occupied_t, urgent_t, hover_t;
    Anim         nlen, ncross;

    gdouble      pos, len, cross;
} Item;

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget       *area;
    WnckScreen      *screen;
    GPtrArray       *items;

    guint            sync_id, tick_id;
    gint64           last_time;
    gdouble          scroll_acc;
    gboolean         ready, snap_next;
    gint             hover_index, count;

    Anim             hl_a, hl_b, hl_alpha;
    gdouble          hl_cross;
    gboolean         hl_ready;
    gint             hl_track_index;
    gdouble          hl_track_a, hl_track_b;

    Anim             compact_t, radius_a, gap_a;
    gdouble          total_main, pulse_t, pulse_f;
    GdkRGBA          fg, accent, accent_cur, accent_fg, accent_fg_cur;

    GtkWidget       *dialog, *count_spin, *color_button;

    gint             style, indicator, marker, motion, radius, gap, anim_ms;
    gboolean         hide_empty, wrap_scroll, use_theme_color, pulse;
    gchar           *accent_hex;
} NaviSpace;

/* dt do quadro e frequência natural (0 = saltar ao alvo) */
typedef struct {
    gdouble dt, omega;
} Step;

/* Tabela única: carrega, salva e monta o diálogo */
typedef struct {
    const gchar *key;
    gsize        offset;
    gint         def, min, max;
    gboolean     is_bool;
} Opt;

#define OPT_INT(k, f, d, lo, hi) { k, G_STRUCT_OFFSET(NaviSpace, f), d, lo, hi, FALSE }
#define OPT_BOOL(k, f, d)        { k, G_STRUCT_OFFSET(NaviSpace, f), d, 0, 1, TRUE }
#define OPT_FIELD(a, o)          ((gint *)G_STRUCT_MEMBER_P(a, (o)->offset))

static const Opt OPTS[] = {
    OPT_INT ("style",           style,     STYLE_NUMBERS,   0, STYLE_COUNT - 1),
    OPT_INT ("indicator",       indicator, IND_FILL,        0, IND_COUNT - 1),
    OPT_INT ("marker",          marker,    MARK_BAR,        0, MARK_COUNT - 1),
    OPT_INT ("motion",          motion,    MOTION_SLIDE,    0, MOTION_COUNT - 1),
    OPT_INT ("radius",          radius,    6,               0, 20),
    OPT_INT ("gap",             gap,       2,               0, 12),
    OPT_INT ("anim_ms",         anim_ms,   DEFAULT_ANIM_MS, 0, MAX_ANIM_MS),
    OPT_BOOL("hide_empty",      hide_empty,      FALSE),
    OPT_BOOL("wrap_scroll",     wrap_scroll,     TRUE),
    OPT_BOOL("use_theme_color", use_theme_color, FALSE),
    OPT_BOOL("pulse",           pulse,           FALSE),
};

static const Opt *opt_find(const gchar *key) {
    for (guint i = 0; i < G_N_ELEMENTS(OPTS); i++)
        if (g_strcmp0(OPTS[i].key, key) == 0)
            return &OPTS[i];
    g_assert_not_reached();
    return NULL;
}

static void navispace_load(NaviSpace *a) {
    gchar *file = xfce_panel_plugin_lookup_rc_file(a->plugin);
    XfceRc *rc = file != NULL ? xfce_rc_simple_open(file, TRUE) : NULL;

    for (guint i = 0; i < G_N_ELEMENTS(OPTS); i++) {
        const Opt *o = &OPTS[i];
        gint *f = OPT_FIELD(a, o);

        *f = o->def;
        if (rc != NULL)
            *f = o->is_bool ? xfce_rc_read_bool_entry(rc, o->key, o->def)
                            : xfce_rc_read_int_entry(rc, o->key, o->def);
        *f = CLAMP(*f, o->min, o->max);
    }
    a->accent_hex = g_strdup(rc != NULL ? xfce_rc_read_entry(rc, "accent", DEFAULT_ACCENT)
                                        : DEFAULT_ACCENT);
    if (rc != NULL)
        xfce_rc_close(rc);
    g_free(file);
}

static void navispace_save(XfcePanelPlugin *plugin, NaviSpace *a) {
    gchar *file = xfce_panel_plugin_save_location(plugin, TRUE);
    XfceRc *rc = file != NULL ? xfce_rc_simple_open(file, FALSE) : NULL;

    if (rc != NULL) {
        for (guint i = 0; i < G_N_ELEMENTS(OPTS); i++) {
            const Opt *o = &OPTS[i];

            if (o->is_bool)
                xfce_rc_write_bool_entry(rc, o->key, *OPT_FIELD(a, o));
            else
                xfce_rc_write_int_entry(rc, o->key, *OPT_FIELD(a, o));
        }
        xfce_rc_write_entry(rc, "accent", a->accent_hex);
        xfce_rc_close(rc);
    }
    g_free(file);
}

static gboolean is_vertical(NaviSpace *a) {
    return xfce_panel_plugin_get_orientation(a->plugin) == GTK_ORIENTATION_VERTICAL;
}

static gboolean is_compact(gint style) {
    return style == STYLE_DOTS || style == STYLE_BARS;
}

/* Nomes são largos demais para painéis verticais */
static gint effective_style(NaviSpace *a) {
    return (a->style == STYLE_NAMES && is_vertical(a)) ? STYLE_NUMBERS : a->style;
}

static gdouble thickness(NaviSpace *a) {
    return CLAMP(xfce_panel_plugin_get_size(a->plugin) - 10, 14, 40);
}

static gdouble dot_size(gdouble thick) {
    return CLAMP((gint)(thick / 2), 8, 12);
}

static GdkRGBA rgba_mix(GdkRGBA x, GdkRGBA y, gdouble t) {
    t = CLAMP(t, 0.0, 1.0);
    x.red   += (y.red   - x.red)   * t;
    x.green += (y.green - x.green) * t;
    x.blue  += (y.blue  - x.blue)  * t;
    x.alpha += (y.alpha - x.alpha) * t;
    return x;
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

static gboolean replace_str(gchar **dst, const gchar *src) {
    if (g_strcmp0(*dst, src) == 0)
        return FALSE;
    g_free(*dst);
    *dst = g_strdup(src);
    return TRUE;
}

static void to_roman(gint n, gchar out[16]) {
    static const struct { gint value; const gchar *glyph; } map[] = {
        { 10, "X" }, { 9, "IX" }, { 5, "V" }, { 4, "IV" }, { 1, "I" }
    };

    out[0] = '\0';
    for (guint i = 0; i < G_N_ELEMENTS(map); i++)
        for (; n >= map[i].value; n -= map[i].value)
            g_strlcat(out, map[i].glyph, 16);
}

static gdouble anim_seconds(NaviSpace *a) {
    gboolean enabled = TRUE;
    GtkSettings *settings = gtk_settings_get_default();

    if (settings != NULL)
        g_object_get(settings, "gtk-enable-animations", &enabled, NULL);
    return enabled ? a->anim_ms / 1000.0 : 0.0;
}

static gboolean anim_settle(Anim *s, gdouble target) {
    s->x = target;
    s->v = 0.0;
    return FALSE;
}

static gboolean anim_at_rest(const Anim *s, gdouble target, gdouble omega, gdouble eps) {
    return fabs(s->x - target) <= eps && fabs(s->v) <= eps * omega;
}

/* Oscilador amortecido de solução exata: estável para qualquer dt e com velocidade contínua entre alvos */
static gboolean anim_step(Anim *s, gdouble target, gdouble omega, gdouble zeta,
                          gdouble dt, gdouble eps) {
    gdouble d = s->x - target;

    if (omega <= 0.0 || anim_at_rest(s, target, omega, eps))
        return anim_settle(s, target);
    if (dt <= 0.0)
        return TRUE;

    gdouble e = exp(-MIN(zeta, 1.0) * omega * dt);

    if (zeta >= 1.0) {
        gdouble b = s->v + omega * d;

        s->x = target + (d + b * dt) * e;
        s->v = (s->v - omega * b * dt) * e;
    } else {
        gdouble wd = omega * sqrt(1.0 - zeta * zeta);
        gdouble b = (s->v + zeta * omega * d) / wd;
        gdouble c = cos(wd * dt), n = sin(wd * dt);

        s->x = target + e * (d * c + b * n);
        s->v = e * ((wd * b - zeta * omega * d) * c - (wd * d + zeta * omega * b) * n);
    }
    return anim_at_rest(s, target, omega, eps) ? anim_settle(s, target) : TRUE;
}

static gboolean glide(Anim *s, gdouble target, const Step *st, gdouble eps) {
    return anim_step(s, target, st->omega, 1.0, st->dt, eps);
}

static gboolean ease_channel(gdouble *v, gdouble target, const Step *st) {
    if (st->omega <= 0.0 || fabs(target - *v) <= EPSILON) {
        *v = target;
        return FALSE;
    }
    *v += (target - *v) * (1.0 - exp(-st->omega * 0.5 * st->dt));
    return TRUE;
}

static gboolean ease_rgba(GdkRGBA *v, const GdkRGBA *t, const Step *st) {
    gboolean m = FALSE;

    m |= ease_channel(&v->red,   t->red,   st);
    m |= ease_channel(&v->green, t->green, st);
    m |= ease_channel(&v->blue,  t->blue,  st);
    m |= ease_channel(&v->alpha, t->alpha, st);
    return m;
}

typedef struct {
    gboolean vertical;
    gdouble  avail_cross;
} Geo;

/* Itens ancorados no início: a largura animada nunca os desloca */
static Geo navispace_geo(NaviSpace *a) {
    Geo g;
    GtkAllocation al;

    gtk_widget_get_allocation(a->area, &al);
    g.vertical = is_vertical(a);
    g.avail_cross = g.vertical ? al.width : al.height;
    return g;
}

static void geo_rect(const Geo *g, gdouble pos, gdouble len, gdouble cross,
                     gdouble *x, gdouble *y, gdouble *w, gdouble *h) {
    gdouble m = PADDING + pos;
    gdouble c = floor((g->avail_cross - cross) / 2.0);

    if (g->vertical) { *x = c; *y = m; *w = cross; *h = len; }
    else             { *x = m; *y = c; *w = len;   *h = cross; }
}

static void navispace_update_colors(NaviSpace *a, gboolean snap) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(a->area);

    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &a->fg);
    a->fg.alpha = 1.0;

    if (a->use_theme_color) {
        if (!gtk_style_context_lookup_color(ctx, "theme_selected_bg_color", &a->accent))
            gdk_rgba_parse(&a->accent, DEFAULT_ACCENT);
        if (!gtk_style_context_lookup_color(ctx, "theme_selected_fg_color", &a->accent_fg))
            a->accent_fg = WHITE;
    } else {
        if (!gdk_rgba_parse(&a->accent, a->accent_hex))
            gdk_rgba_parse(&a->accent, DEFAULT_ACCENT);
        gdk_rgba_parse(&a->accent_fg, rgba_is_light(&a->accent) ? "#1a1b26" : "#ffffff");
    }
    a->accent.alpha = 1.0;
    a->accent_fg.alpha = 1.0;

    if (snap) {
        a->accent_cur = a->accent;
        a->accent_fg_cur = a->accent_fg;
    }
}

static void item_free(Item *it) {
    g_clear_object(&it->layout);
    g_free(it->label);
    g_free(it->name);
    g_slice_free(Item, it);
}

static void item_reset_layout(Item *it) {
    g_clear_object(&it->layout);
    it->text_w = it->text_h = 0;
}

static gint item_cmp(gconstpointer pa, gconstpointer pb) {
    return (*(Item *const *)pa)->index - (*(Item *const *)pb)->index;
}

static Item *item_find(NaviSpace *a, gint idx) {
    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        if (it->index == idx)
            return it;
    }
    return NULL;
}

/* Estilos compactos mantêm o último rótulo para o crossfade */
static void item_update_label(Item *it, gint style) {
    gchar buf[16];
    const gchar *text;

    switch (style) {
    case STYLE_NAMES:
        text = it->name;
        break;
    case STYLE_ROMAN:
        to_roman(it->index + 1, buf);
        text = buf;
        break;
    case STYLE_NUMBERS:
        g_snprintf(buf, sizeof buf, "%d", it->index + 1);
        text = buf;
        break;
    default:
        return;
    }
    if (replace_str(&it->label, text))
        item_reset_layout(it);
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

static void navispace_layout(NaviSpace *a) {
    gdouble pos = 0.0, last_gap = 0.0;

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        gdouble gap = a->gap_a.x * it->present.x;

        it->len   = it->nlen.x * it->present.x;
        it->cross = it->ncross.x;
        it->pos   = pos;
        pos += it->len + gap;
        if (it->present.x > 0.001)
            last_gap = gap;
    }
    a->total_main = MAX(pos - last_gap, 0.0);
}

/* Uma borda por extremo: permite esticar e usar mola */
static gboolean move_highlight(NaviSpace *a, gdouble ta, gdouble tb, const Step *st) {
    gboolean moving = FALSE;

    switch (a->motion) {
    case MOTION_SPRING:
        moving |= anim_step(&a->hl_a, ta, st->omega * SPRING_GAIN, SPRING_ZETA, st->dt, PX_EPSILON);
        moving |= anim_step(&a->hl_b, tb, st->omega * SPRING_GAIN, SPRING_ZETA, st->dt, PX_EPSILON);
        break;
    case MOTION_STRETCH: {

        gboolean forward = ta + tb > a->hl_a.x + a->hl_b.x;
        gdouble wa = st->omega * (forward ? TRAIL_GAIN : LEAD_GAIN);
        gdouble wb = st->omega * (forward ? LEAD_GAIN : TRAIL_GAIN);

        moving |= anim_step(&a->hl_a, ta, wa, 1.0, st->dt, PX_EPSILON);
        moving |= anim_step(&a->hl_b, tb, wb, 1.0, st->dt, PX_EPSILON);
        break;
    }
    case MOTION_FADE:
        anim_step(&a->hl_a, ta, 0.0, 1.0, st->dt, PX_EPSILON);
        anim_step(&a->hl_b, tb, 0.0, 1.0, st->dt, PX_EPSILON);
        break;
    default:
        moving |= glide(&a->hl_a, ta, st, PX_EPSILON);
        moving |= glide(&a->hl_b, tb, st, PX_EPSILON);
    }
    return moving;
}

static gboolean navispace_step(NaviSpace *a, gdouble dt, gdouble secs) {
    Step st = { dt, secs > 0.0 ? SETTLE_RATE / secs : 0.0 };
    gint style = effective_style(a);
    gboolean compact = is_compact(style);
    gdouble thick = thickness(a);
    gdouble dot = dot_size(thick);
    gboolean moving = FALSE, any_urgent = FALSE;
    Item *act = NULL;

    moving |= glide(&a->compact_t, compact ? 1.0 : 0.0, &st, EPSILON);
    moving |= glide(&a->radius_a, a->radius, &st, 0.02);
    moving |= glide(&a->gap_a, a->gap, &st, 0.02);
    moving |= ease_rgba(&a->accent_cur, &a->accent, &st);
    moving |= ease_rgba(&a->accent_fg_cur, &a->accent_fg, &st);

    for (guint i = 0; i < a->items->len; ) {
        Item *it = g_ptr_array_index(a->items, i);
        gdouble tlen, tcross;

        item_ensure_layout(a, it);

        if (style == STYLE_DOTS) {
            tlen = it->is_active ? dot * 2 + 4 : dot;
            tcross = dot;
        } else if (style == STYLE_BARS) {
            tlen = it->is_active ? dot * 3 : dot * 1.4;
            tcross = BAR_THICKNESS;
        } else {
            tlen = MAX(thick, it->text_w + 2 * (style == STYLE_NAMES ? 10 : 6));
            tcross = thick;
        }
        if (!it->sized) {
            it->nlen.x = tlen;
            it->ncross.x = tcross;
            it->sized = TRUE;
        }

        moving |= glide(&it->nlen,       tlen,   &st, PX_EPSILON);
        moving |= glide(&it->ncross,     tcross, &st, PX_EPSILON);
        moving |= glide(&it->present,    it->want_visible ? 1.0 : 0.0, &st, EPSILON);
        moving |= glide(&it->active_t,   it->is_active    ? 1.0 : 0.0, &st, EPSILON);
        moving |= glide(&it->occupied_t, it->occupied     ? 1.0 : 0.0, &st, EPSILON);
        moving |= glide(&it->urgent_t,   it->urgent       ? 1.0 : 0.0, &st, EPSILON);
        moving |= glide(&it->hover_t,    it->hovered      ? 1.0 : 0.0, &st, EPSILON);

        if (!it->exists && it->present.x <= 0.0) {
            g_ptr_array_remove_index(a->items, i);
            continue;
        }
        any_urgent |= it->urgent;
        if (it->exists && it->is_active)
            act = it;
        i++;
    }

    navispace_layout(a);

    if (act != NULL) {
        gdouble ta = act->pos, tb = act->pos + act->len;

        if (!a->hl_ready) {
            a->hl_a.x = ta;
            a->hl_b.x = tb;
            a->hl_ready = TRUE;
        } else if (act->index == a->hl_track_index) {
            a->hl_a.x += ta - a->hl_track_a;
            a->hl_b.x += tb - a->hl_track_b;
        }
        a->hl_track_index = act->index;
        a->hl_track_a = ta;
        a->hl_track_b = tb;
        a->hl_cross = act->cross;
        moving |= move_highlight(a, ta, tb, &st);
    }
    moving |= glide(&a->hl_alpha, (act != NULL && !compact) ? 1.0 : 0.0, &st, EPSILON);

    if (a->pulse && any_urgent && secs > 0.0) {
        a->pulse_t = fmod(a->pulse_t + dt, PULSE_PERIOD);
        a->pulse_f = 0.65 + 0.35 * (0.5 + 0.5 * cos(2.0 * G_PI * a->pulse_t / PULSE_PERIOD));
        moving = TRUE;
    } else {
        a->pulse_t = 0.0;
        a->pulse_f = 1.0;
    }
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

static void navispace_snap(NaviSpace *a) {
    navispace_step(a, 0.0, 0.0);
    navispace_apply_size(a);
    gtk_widget_queue_draw(a->area);
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
    NaviSpace *a = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    gdouble dt = a->last_time ? (now - a->last_time) / 1000000.0 : 1.0 / 60.0;

    a->last_time = now;
    gboolean moving = navispace_step(a, CLAMP(dt, 0.0, 0.1), anim_seconds(a));

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
    if (anim_seconds(a) <= 0.0 || !gtk_widget_get_mapped(a->area)) {
        navispace_snap(a);
        return;
    }
    if (a->tick_id == 0) {
        a->last_time = 0;
        a->tick_id = gtk_widget_add_tick_callback(a->area, on_tick, a, NULL);
    }
}

static gboolean window_counts(WnckWindow *w) {
    WnckWindowType t = wnck_window_get_window_type(w);

    return t != WNCK_WINDOW_DESKTOP && t != WNCK_WINDOW_DOCK && !wnck_window_is_skip_tasklist(w);
}

static void navispace_sync(NaviSpace *a) {
    gint style = effective_style(a);
    GList *workspaces = wnck_screen_get_workspaces(a->screen);
    gint count = (gint)g_list_length(workspaces);
    WnckWorkspace *active = wnck_screen_get_active_workspace(a->screen);

    for (guint i = 0; i < a->items->len; i++)
        ((Item *)g_ptr_array_index(a->items, i))->exists = FALSE;

    for (GList *l = workspaces; l != NULL; l = l->next) {
        WnckWorkspace *ws = l->data;
        gint idx = wnck_workspace_get_number(ws);
        Item *it = item_find(a, idx);

        if (it == NULL) {
            it = g_slice_new0(Item);
            it->index = idx;
            g_ptr_array_add(a->items, it);
        }
        it->exists = TRUE;
        it->is_active = (ws == active);
        it->n_windows = 0;
        it->urgent = FALSE;
        replace_str(&it->name, wnck_workspace_get_name(ws));
        item_update_label(it, style);
    }

    for (GList *l = wnck_screen_get_windows(a->screen); l != NULL; l = l->next) {
        WnckWindow *win = l->data;
        WnckWorkspace *ws = wnck_window_get_workspace(win);
        Item *it = (ws != NULL && window_counts(win)) ? item_find(a, wnck_workspace_get_number(ws)) : NULL;

        if (it == NULL || !it->exists)
            continue;
        it->n_windows++;
        it->urgent |= wnck_window_needs_attention(win);
    }

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);

        if (it->exists) {
            it->occupied = it->n_windows > 0;
            it->urgent &= !it->is_active;
            it->want_visible = !a->hide_empty || it->occupied || it->is_active;
            if (!it->sized)
                it->present.x = (a->ready || !it->want_visible) ? 0.0 : 1.0;
        } else {
            it->want_visible = it->is_active = it->occupied = it->urgent = FALSE;
        }
    }
    g_ptr_array_sort(a->items, item_cmp);

    if (count != a->count) {
        a->count = count;
        if (a->count_spin != NULL)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->count_spin), count);
    }

    if (!a->ready || a->snap_next) {
        a->ready = TRUE;
        a->snap_next = FALSE;
        navispace_snap(a);
    } else {
        navispace_animate(a);
    }
}

static gboolean sync_idle(gpointer data) {
    NaviSpace *a = data;

    a->sync_id = 0;
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

/* Usado com g_signal_connect_swapped: ignora os argumentos do sinal */
static void on_wnck_changed(NaviSpace *a) {
    navispace_queue_sync(a, FALSE);
}

static void watch_window(NaviSpace *a, WnckWindow *win) {
    g_signal_connect_swapped(win, "workspace-changed", G_CALLBACK(on_wnck_changed), a);
    g_signal_connect_swapped(win, "state-changed",     G_CALLBACK(on_wnck_changed), a);
}

static void watch_workspace(NaviSpace *a, WnckWorkspace *ws) {
    g_signal_connect_swapped(ws, "name-changed", G_CALLBACK(on_wnck_changed), a);
}

static void on_window_opened(WnckScreen *screen, WnckWindow *win, NaviSpace *a) {
    watch_window(a, win);
    navispace_queue_sync(a, FALSE);
}

static void on_workspace_created(WnckScreen *screen, WnckWorkspace *ws, NaviSpace *a) {
    watch_workspace(a, ws);
    navispace_queue_sync(a, FALSE);
}

static void draw_indicator(cairo_t *cr, NaviSpace *a, gdouble x, gdouble y,
                           gdouble w, gdouble h, gdouble alpha) {
    gdouble r = a->radius_a.x;

    set_source(cr, a->accent_cur, alpha);
    switch (a->indicator) {
    case IND_OUTLINE:
        rounded_rect(cr, x + 0.75, y + 0.75, w - 1.5, h - 1.5, r - 0.75);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        break;
    case IND_UNDERLINE: {
        gdouble inset = MIN(r, w / 4.0);

        rounded_rect(cr, x + inset, y + h - 3, w - 2 * inset, 3, 1.5);
        cairo_fill(cr);
        break;
    }
    default:
        rounded_rect(cr, x, y, w, h, r);
        cairo_fill(cr);
    }
}

static void draw_marker(cairo_t *cr, NaviSpace *a, gdouble x, gdouble y,
                        gdouble w, gdouble h, gdouble amount, gdouble alpha) {
    set_source(cr, rgba_with_alpha(a->accent_cur, 0.75), alpha);
    if (a->marker == MARK_BAR) {
        gdouble bw = w * 0.4 * amount;

        rounded_rect(cr, x + (w - bw) / 2.0, y + h - 4, bw, 2, 1);
    } else {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w / 2.0, y + h - 3.5, 1.75 * amount, 0, 2 * G_PI);
    }
    cairo_fill(cr);
}

static void draw_text_item(cairo_t *cr, NaviSpace *a, Item *it, gdouble x, gdouble y,
                           gdouble w, gdouble h, gdouble cov, gdouble vis) {
    if (it->hover_t.x > 0.01) {
        rounded_rect(cr, x, y, w, h, a->radius_a.x);
        set_source(cr, rgba_with_alpha(a->fg, 0.12), it->hover_t.x * (1.0 - cov) * vis);
        cairo_fill(cr);
    }
    if (it->urgent_t.x > 0.01) {
        rounded_rect(cr, x, y, w, h, a->radius_a.x);
        set_source(cr, ALERT, it->urgent_t.x * a->pulse_f * vis);
        cairo_fill(cr);
    }

    gdouble marker = it->occupied_t.x * (1.0 - cov) * (1.0 - it->urgent_t.x);
    if (a->marker != MARK_NONE && marker > 0.01)
        draw_marker(cr, a, x, y, w, h, marker, vis);

    if (it->layout == NULL)
        return;

    GdkRGBA lit = (a->indicator == IND_FILL) ? a->accent_fg_cur : a->accent_cur;
    GdkRGBA c = rgba_mix(rgba_with_alpha(a->fg, 0.45), rgba_with_alpha(a->fg, 0.95), it->occupied_t.x);

    c = rgba_mix(c, rgba_with_alpha(a->fg, 1.0), it->hover_t.x);
    c = rgba_mix(c, lit, cov);
    c = rgba_mix(c, WHITE, it->urgent_t.x);

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    set_source(cr, c, vis);
    cairo_move_to(cr, floor(x + (w - it->text_w) / 2.0), floor(y + (h - it->text_h) / 2.0));
    pango_cairo_show_layout(cr, it->layout);
    cairo_restore(cr);
}

static void draw_compact_item(cairo_t *cr, NaviSpace *a, Item *it, gdouble x, gdouble y,
                              gdouble w, gdouble h, gdouble vis) {
    GdkRGBA c = rgba_mix(rgba_with_alpha(a->fg, 0.30), rgba_with_alpha(a->fg, 0.65), it->occupied_t.x);

    c = rgba_mix(c, rgba_with_alpha(a->fg, 0.90), it->hover_t.x * (1.0 - it->active_t.x));
    c = rgba_mix(c, a->accent_cur, it->active_t.x);
    c = rgba_mix(c, ALERT, it->urgent_t.x);

    rounded_rect(cr, x, y, w, h, MIN(w, h) / 2.0);
    set_source(cr, c, vis * (1.0 - it->urgent_t.x * (1.0 - a->pulse_f)));
    cairo_fill(cr);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, NaviSpace *a) {
    Geo g = navispace_geo(a);
    gdouble text_vis = 1.0 - a->compact_t.x;
    gdouble hl_len = a->hl_b.x - a->hl_a.x;
    gboolean fade = a->motion == MOTION_FADE;
    gdouble x, y, w, h;

    if (!fade && a->hl_alpha.x > 0.005 && hl_len > 0.5) {
        geo_rect(&g, a->hl_a.x, hl_len, a->hl_cross, &x, &y, &w, &h);
        draw_indicator(cr, a, x, y, w, h, a->hl_alpha.x);
    }

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);
        gdouble cov = 0.0;

        if (it->present.x < 0.01 || it->len < 0.5)
            continue;

        geo_rect(&g, it->pos, it->len, it->cross, &x, &y, &w, &h);

        if (fade) {
            cov = it->active_t.x * a->hl_alpha.x;
            if (cov > 0.01)
                draw_indicator(cr, a, x, y, w, h, cov * it->present.x);
        } else {
            gdouble o0 = MAX(it->pos, a->hl_a.x);
            gdouble o1 = MIN(it->pos + it->len, a->hl_b.x);

            if (o1 > o0)
                cov = CLAMP((o1 - o0) / it->len, 0.0, 1.0) * a->hl_alpha.x;
        }

        if (text_vis > 0.01)
            draw_text_item(cr, a, it, x, y, w, h, cov, text_vis * it->present.x);
        if (a->compact_t.x > 0.01)
            draw_compact_item(cr, a, it, x, y, w, h, a->compact_t.x * it->present.x);
    }
    return FALSE;
}

static gint item_at(NaviSpace *a, gdouble ex, gdouble ey) {
    gdouble m = (is_vertical(a) ? ey : ex) - PADDING;
    gdouble half = a->gap_a.x / 2.0;

    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);

        if (it->exists && it->present.x >= 0.5 && m >= it->pos - half && m < it->pos + it->len + half)
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

static void activate_workspace(NaviSpace *a, gint idx, guint32 time) {
    WnckWorkspace *ws = wnck_screen_get_workspace(a->screen, idx);

    if (ws != NULL)
        wnck_workspace_activate(ws, time);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, NaviSpace *a) {
    gint idx = (ev->button == 1 && ev->type == GDK_BUTTON_PRESS) ? item_at(a, ev->x, ev->y) : -1;

    if (idx < 0)
        return FALSE;
    activate_workspace(a, idx, ev->time);
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
        if (fabs(a->scroll_acc) >= 1.0) {
            dir = a->scroll_acc < 0 ? -1 : 1;
            a->scroll_acc = 0.0;
        }
        break;
    default:
        return FALSE;
    }

    WnckWorkspace *active = wnck_screen_get_active_workspace(a->screen);
    if (dir == 0 || active == NULL)
        return TRUE;

    gint count = wnck_screen_get_workspace_count(a->screen);
    gint cur = wnck_workspace_get_number(active);
    gint next = cur + dir;

    if (next < 0 || next >= count)
        next = a->wrap_scroll ? (next + count) % count : cur;
    if (next != cur)
        activate_workspace(a, next, ev->time);
    return TRUE;
}

/* Refaz os layouts na hora para o texto não sumir por um quadro */
static void refresh_style(NaviSpace *a, gboolean snap) {
    for (guint i = 0; i < a->items->len; i++) {
        Item *it = g_ptr_array_index(a->items, i);

        item_reset_layout(it);
        item_ensure_layout(a, it);
    }
    navispace_update_colors(a, snap);
    navispace_queue_sync(a, snap);
}

static void on_style_updated(GtkWidget *w, NaviSpace *a) {
    refresh_style(a, FALSE);
}

static void on_realize(GtkWidget *w, NaviSpace *a) {
    refresh_style(a, TRUE);
}

static gboolean on_size_changed(XfcePanelPlugin *plugin, gint size, NaviSpace *a) {
    navispace_queue_sync(a, FALSE);
    return TRUE;
}

static void on_orientation_changed(XfcePanelPlugin *plugin, GtkOrientation o, NaviSpace *a) {
    navispace_queue_sync(a, TRUE);
}

static void on_area_destroy(GtkWidget *w, NaviSpace *a) {
    if (a->sync_id != 0) {
        g_source_remove(a->sync_id);
        a->sync_id = 0;
    }
    a->tick_id = 0;
    a->area = NULL;
}

typedef struct {
    NaviSpace *a;
    GtkWidget *grid;
    gint       row;
} Form;

static void apply_and_save(NaviSpace *a) {
    navispace_update_colors(a, FALSE);
    navispace_queue_sync(a, FALSE);
    navispace_save(a->plugin, a);
}

static void on_option_changed(GtkWidget *w, NaviSpace *a) {
    const Opt *o = g_object_get_data(G_OBJECT(w), "opt");
    gint *field = OPT_FIELD(a, o);

    if (GTK_IS_COMBO_BOX(w))
        *field = gtk_combo_box_get_active(GTK_COMBO_BOX(w));
    else if (GTK_IS_TOGGLE_BUTTON(w))
        *field = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    else
        *field = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));

    if (a->color_button != NULL)
        gtk_widget_set_sensitive(a->color_button, !a->use_theme_color);
    apply_and_save(a);
}

static GtkWidget *bind_option(Form *f, GtkWidget *w, const gchar *key, const gchar *signal) {
    g_object_set_data(G_OBJECT(w), "opt", (gpointer)opt_find(key));
    g_signal_connect(w, signal, G_CALLBACK(on_option_changed), f->a);
    return w;
}

static void form_title(Form *f, const gchar *text) {
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);

    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    if (f->row > 0)
        gtk_widget_set_margin_top(label, 8);
    gtk_grid_attach(GTK_GRID(f->grid), label, 0, f->row++, 2, 1);
    g_free(markup);
}

static void form_row(Form *f, const gchar *mnemonic, GtkWidget *w) {
    GtkWidget *label = gtk_label_new_with_mnemonic(mnemonic);

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 12);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), w);
    gtk_grid_attach(GTK_GRID(f->grid), label, 0, f->row, 1, 1);
    gtk_grid_attach(GTK_GRID(f->grid), w, 1, f->row++, 1, 1);
}

static void form_check(Form *f, const gchar *key, const gchar *mnemonic, const gchar *tip) {
    GtkWidget *w = gtk_check_button_new_with_mnemonic(mnemonic);

    gtk_widget_set_margin_start(w, 12);
    gtk_widget_set_tooltip_text(w, tip);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), *OPT_FIELD(f->a, opt_find(key)));
    gtk_grid_attach(GTK_GRID(f->grid), bind_option(f, w, key, "toggled"), 0, f->row++, 2, 1);
}

static void form_spin(Form *f, const gchar *key, const gchar *mnemonic, gdouble step) {
    const Opt *o = opt_find(key);
    GtkWidget *w = gtk_spin_button_new_with_range(o->min, o->max, step);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), *OPT_FIELD(f->a, o));
    form_row(f, mnemonic, bind_option(f, w, key, "value-changed"));
}

static void form_combo(Form *f, const gchar *key, const gchar *mnemonic, const gchar *const *items) {
    GtkWidget *w = gtk_combo_box_text_new();

    for (; *items != NULL; items++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), *items);
    gtk_combo_box_set_active(GTK_COMBO_BOX(w), *OPT_FIELD(f->a, opt_find(key)));
    form_row(f, mnemonic, bind_option(f, w, key, "changed"));
}

static void on_count_changed(GtkSpinButton *b, NaviSpace *a) {
    gint wanted = gtk_spin_button_get_value_as_int(b);

    if (wanted != wnck_screen_get_workspace_count(a->screen))
        wnck_screen_change_workspace_count(a->screen, wanted);
}

static void on_color_set(GtkColorButton *b, NaviSpace *a) {
    GdkRGBA rgba;

    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(b), &rgba);
    g_free(a->accent_hex);
    a->accent_hex = gdk_rgba_to_string(&rgba);
    apply_and_save(a);
}

static void on_dialog_destroyed(GtkWidget *dialog, NaviSpace *a) {
    a->dialog = a->count_spin = a->color_button = NULL;
    xfce_panel_plugin_unblock_menu(a->plugin);
}

static void navispace_configure(XfcePanelPlugin *plugin, NaviSpace *a) {
    static const gchar *const styles[] = {
        "Números (1 2 3)", "Nomes", "Algarismos romanos (I II III)",
        "Pontos (pílula)", "Traços", NULL };
    static const gchar *const indicators[] = { "Preenchido", "Contorno", "Sublinhado", NULL };
    static const gchar *const markers[]    = { "Nenhum", "Barra", "Ponto", NULL };
    static const gchar *const motions[]    = { "Deslizar", "Esticar", "Mola", "Desvanecer", NULL };

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
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 0);

    Form f = { a, grid, 0 };

    form_title(&f, "Espaços de trabalho");
    a->count_spin = gtk_spin_button_new_with_range(1, MAX_WORKSPACES, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->count_spin), wnck_screen_get_workspace_count(a->screen));
    g_signal_connect(a->count_spin, "value-changed", G_CALLBACK(on_count_changed), a);
    form_row(&f, "_Quantidade:", a->count_spin);
    form_check(&f, "hide_empty", "Ocultar espaços _vazios",
               "Mostra só o espaço ativo e os que têm janelas");
    form_check(&f, "wrap_scroll", "Roda do mouse dá a _volta (último ↔ primeiro)",
               "Ao passar do último espaço, volta ao primeiro");

    form_title(&f, "Aparência");
    form_combo(&f, "style", "_Estilo:", styles);
    form_combo(&f, "indicator", "Des_taque:", indicators);
    form_combo(&f, "marker", "_Marcador de janelas:", markers);
    form_check(&f, "use_theme_color", "Usar cor de destaque do _tema",
               "Usa a cor de seleção do tema GTK");

    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, a->accent_hex))
        gdk_rgba_parse(&rgba, DEFAULT_ACCENT);
    a->color_button = gtk_color_button_new_with_rgba(&rgba);
    gtk_widget_set_sensitive(a->color_button, !a->use_theme_color);
    g_signal_connect(a->color_button, "color-set", G_CALLBACK(on_color_set), a);
    form_row(&f, "Cor de _destaque:", a->color_button);

    form_spin(&f, "radius", "_Arredondamento:", 1);
    form_spin(&f, "gap", "E_spaçamento:", 1);

    form_title(&f, "Animação");
    form_combo(&f, "motion", "_Movimento:", motions);
    form_spin(&f, "anim_ms", "_Duração (ms):", 20);
    form_check(&f, "pulse", "_Pulsar quando uma janela pedir atenção",
               "Mantém o alerta piscando suavemente até ser atendido");

    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_dialog_destroyed), a);
    gtk_widget_show_all(dialog);
}

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

    g_ptr_array_free(a->items, TRUE);
    g_free(a->accent_hex);
    g_slice_free(NaviSpace, a);
}

static void navispace_construct(XfcePanelPlugin *plugin) {
    NaviSpace *a = g_slice_new0(NaviSpace);

    a->plugin = plugin;
    a->hover_index = -1;
    a->pulse_f = 1.0;
    a->items = g_ptr_array_new_with_free_func((GDestroyNotify)item_free);
    navispace_load(a);
    g_signal_connect(plugin, "free-data", G_CALLBACK(navispace_free), a);

    wnck_set_client_type(WNCK_CLIENT_TYPE_PAGER);
    a->screen = wnck_screen_get_default();

    if (a->screen == NULL) {
        GtkWidget *label = gtk_label_new("NaviSpace indisponível");

        gtk_widget_set_tooltip_text(label, "Requer uma sessão X11 com suporte a espaços de trabalho");
        gtk_container_add(GTK_CONTAINER(plugin), label);
        gtk_widget_show_all(GTK_WIDGET(plugin));
        return;
    }
    wnck_screen_force_update(a->screen);

    a->radius_a.x = a->radius;
    a->gap_a.x = a->gap;
    a->compact_t.x = is_compact(effective_style(a)) ? 1.0 : 0.0;

    a->area = gtk_drawing_area_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(a->area), "navispace");
    gtk_widget_add_events(a->area,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK |
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_widget_set_has_tooltip(a->area, TRUE);
    gtk_container_add(GTK_CONTAINER(plugin), a->area);

    navispace_update_colors(a, TRUE);

    g_signal_connect(a->area, "draw",                G_CALLBACK(on_draw),          a);
    g_signal_connect(a->area, "button-press-event",  G_CALLBACK(on_button_press),  a);
    g_signal_connect(a->area, "motion-notify-event", G_CALLBACK(on_motion),        a);
    g_signal_connect(a->area, "leave-notify-event",  G_CALLBACK(on_leave),         a);
    g_signal_connect(a->area, "scroll-event",        G_CALLBACK(on_scroll),        a);
    g_signal_connect(a->area, "query-tooltip",       G_CALLBACK(on_query_tooltip), a);
    g_signal_connect(a->area, "style-updated",       G_CALLBACK(on_style_updated), a);
    g_signal_connect(a->area, "realize",             G_CALLBACK(on_realize),       a);
    g_signal_connect(a->area, "destroy",             G_CALLBACK(on_area_destroy),  a);

    for (GList *l = wnck_screen_get_windows(a->screen); l != NULL; l = l->next)
        watch_window(a, l->data);
    for (GList *l = wnck_screen_get_workspaces(a->screen); l != NULL; l = l->next)
        watch_workspace(a, l->data);

    g_signal_connect(a->screen, "workspace-created", G_CALLBACK(on_workspace_created), a);
    g_signal_connect(a->screen, "window-opened",     G_CALLBACK(on_window_opened),     a);
    g_signal_connect_swapped(a->screen, "active-workspace-changed", G_CALLBACK(on_wnck_changed), a);
    g_signal_connect_swapped(a->screen, "workspace-destroyed",      G_CALLBACK(on_wnck_changed), a);
    g_signal_connect_swapped(a->screen, "window-closed",            G_CALLBACK(on_wnck_changed), a);

    navispace_sync(a);
    gtk_widget_show_all(GTK_WIDGET(plugin));

    xfce_panel_plugin_add_action_widget(plugin, a->area);
    xfce_panel_plugin_menu_show_configure(plugin);

    g_signal_connect(plugin, "configure-plugin",    G_CALLBACK(navispace_configure),    a);
    g_signal_connect(plugin, "save",                G_CALLBACK(navispace_save),         a);
    g_signal_connect(plugin, "size-changed",        G_CALLBACK(on_size_changed),        a);
    g_signal_connect(plugin, "orientation-changed", G_CALLBACK(on_orientation_changed), a);
}

XFCE_PANEL_PLUGIN_REGISTER(navispace_construct);
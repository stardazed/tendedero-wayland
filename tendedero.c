/*
 * tendedero - screenshots, hung out to dry. (Wayland / wlr-layer-shell port)
 *
 * A clothesline strung across the top of your screen. New screenshots swing in
 * on a clothes-peg with spring physics. Click = copy, drag = share,
 * hold = mark up, right-click = let it go.
 *
 * Build:  make            (needs libgtk-3-dev libgtk-layer-shell-dev)
 * Run:    tendedero &     then   tendedero --capture
 */
#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <gio/gio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define APP_ID      "io.stardazed.tendedero"
#define MAXW        170     /* max thumbnail size, logical px */
#define MAXH        130
#define BORDER      5.0
#define GAP         26.0
#define WIRE_Y      16.0    /* wire height inside the surface */
#define SAG         12.0
#define SWING_ROOM  70.0
#define WIN_H       ((int)(WIRE_Y + SAG + 6 + MAXH + 2 * BORDER + 18 + SWING_ROOM))
#define HOVER_SCALE 1.07
#define DRAG_THRESH 8.0
#define LONGPRESS_MS 550

/* ------------------------------------------------------------------ config */

typedef struct {
    int      offset;      /* px from the top of the screen to the wire (default 140) */
    gboolean reserve;     /* TRUE: reserve space (exclusive zone); FALSE: float on top */
    double   stiffness;   /* spring stiffness (rad/s)^2 */
    double   damping;     /* damping ratio, 0 = never stops, 1 = no wobble */
    double   kick;        /* pointer/entry swing strength multiplier */
    gboolean watch;       /* hang any new image in watch_dir */
    char    *watch_dir;
    char    *capture_cmd; /* optional custom capture command */
    char    *markup_cmd;  /* optional markup command, receives file as last arg */
} Config;

static Config cfg = { 140, FALSE, 160.0, 0.35, 1.0, TRUE, NULL, NULL, NULL };

static char *config_path(void) {
    return g_build_filename(g_get_user_config_dir(), "tendedero", "config.ini", NULL);
}

static void config_load(void) {
    GKeyFile *kf = g_key_file_new();
    char *p = config_path();
    if (g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        #define GI(k, f)  { int v = g_key_file_get_integer(kf, "tendedero", k, &e);  if (!e) cfg.f = v; g_clear_error(&e); }
        #define GD(k, f)  { double v = g_key_file_get_double(kf, "tendedero", k, &e); if (!e) cfg.f = v; g_clear_error(&e); }
        #define GB(k, f)  { gboolean v = g_key_file_get_boolean(kf, "tendedero", k, &e); if (!e) cfg.f = v; g_clear_error(&e); }
        #define GS(k, f)  { char *v = g_key_file_get_string(kf, "tendedero", k, NULL); if (v) { g_free(cfg.f); cfg.f = v; } }
        GI("offset", offset) GB("reserve", reserve)
        GD("stiffness", stiffness) GD("damping", damping) GD("kick", kick)
        GB("watch", watch) GS("watch_dir", watch_dir)
        GS("capture_cmd", capture_cmd) GS("markup_cmd", markup_cmd)
    }
    if (!cfg.watch_dir || !*cfg.watch_dir) {
        const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
        char *base = pics ? g_strdup(pics) : g_build_filename(g_get_home_dir(), "Pictures", NULL);
        g_free(cfg.watch_dir);
        cfg.watch_dir = g_build_filename(base, "Screenshots", NULL);
        g_free(base);
    }
    g_free(p);
    g_key_file_free(kf);
}

static void config_save(void) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "tendedero", "offset", cfg.offset);
    g_key_file_set_boolean(kf, "tendedero", "reserve", cfg.reserve);
    g_key_file_set_double(kf, "tendedero", "stiffness", cfg.stiffness);
    g_key_file_set_double(kf, "tendedero", "damping", cfg.damping);
    g_key_file_set_double(kf, "tendedero", "kick", cfg.kick);
    g_key_file_set_boolean(kf, "tendedero", "watch", cfg.watch);
    g_key_file_set_string(kf, "tendedero", "watch_dir", cfg.watch_dir ? cfg.watch_dir : "");
    g_key_file_set_string(kf, "tendedero", "capture_cmd", cfg.capture_cmd ? cfg.capture_cmd : "");
    g_key_file_set_string(kf, "tendedero", "markup_cmd", cfg.markup_cmd ? cfg.markup_cmd : "");
    char *p = config_path(), *d = g_path_get_dirname(p);
    g_mkdir_with_parents(d, 0755);
    g_key_file_save_to_file(kf, p, NULL);
    g_free(p); g_free(d);
    g_key_file_free(kf);
}

/* ------------------------------------------------------------------- cards */

typedef struct {
    char            *path;
    cairo_surface_t *thumb;          /* device-scale 2, logical size tw x th */
    int              tw, th;
    /* spring state */
    double x, vx, tx;                /* pin x, target x */
    double ang, w;                   /* swing angle (rad, +cw), angular velocity */
    double sc, vsc, sct;             /* scale */
    double ey, vey;                  /* entry offset below the wire */
    double alpha;
    /* let-go */
    gboolean falling;
    double   fy, fvy;
    /* ui state */
    gboolean hover, dragging;
    double   copied_t;
} Card;

static GList        *cards;          /* oldest first */
static GtkWidget    *win, *area, *settings_win;
static GHashTable   *seen;           /* paths already hung */
static GFileMonitor *monitor;
static guint         tick_id;
static gint64        last_us;
static double        wire_alpha;
static gboolean      layer_ok;
static int           surf_w = 1280;

/* pointer interaction */
static Card   *press_card;
static double  press_x, press_y;
static gboolean long_fired, drag_started;
static guint   long_id;
static double  last_mx, last_my;
static guint32 last_mt;

static double wire_y(double x) {
    double u = surf_w > 0 ? x / surf_w : 0.5;
    return WIRE_Y + SAG * 4.0 * u * (1.0 - u);
}
static double wire_slope(double x) {
    return surf_w > 0 ? SAG * 4.0 * (1.0 - 2.0 * x / surf_w) / surf_w : 0.0;
}
static double card_py(const Card *c) { return wire_y(c->x) + c->ey + (c->falling ? c->fy : 0); }
static double card_ang(const Card *c) { return c->ang + atan(wire_slope(c->x)); }
static double card_h(const Card *c)   { return c->th + 2 * BORDER; }

static void start_anim(void);

/* ---------------------------------------------------------------- physics */

static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static void layout_cards(void) {
    double total = 0; int n = 0;
    for (GList *l = cards; l; l = l->next) {
        Card *c = l->data; if (c->falling) continue;
        total += c->tw + 2 * BORDER; n++;
    }
    if (n > 1) total += GAP * (n - 1);
    /* too many to fit: let the oldest go */
    while (n > 1 && total > surf_w - 120) {
        for (GList *l = cards; l; l = l->next) {
            Card *c = l->data;
            if (!c->falling) {
                c->falling = TRUE; c->fvy = -260; c->w += 2.5; c->vx += 60;
                total -= c->tw + 2 * BORDER + GAP; n--; break;
            }
        }
    }
    double x = (surf_w - total) / 2.0;
    for (GList *l = cards; l; l = l->next) {
        Card *c = l->data; if (c->falling) continue;
        double cw = c->tw + 2 * BORDER;
        c->tx = x + cw / 2.0;
        x += cw + GAP;
    }
}

static void step_card(Card *c, double dt) {
    double k = cfg.stiffness, z = cfg.damping;
    double kp = k * 3.0, zp = fmin(1.1, 0.45 + 0.5 * z);
    double cp = 2 * zp * sqrt(kp), ca = 2 * z * sqrt(k);

    if (c->falling) {
        c->fvy += 2600.0 * dt;
        c->fy  += c->fvy * dt;
        c->x   += c->vx * dt;
        c->ang += c->w * dt;
        c->alpha -= dt * 1.5;
        return;
    }
    /* pin slides along the wire on a spring */
    double ax = kp * (c->tx - c->x) - cp * c->vx;
    c->vx += ax * dt;  c->x += c->vx * dt;

    /* the card is a damped spring-pendulum; pin acceleration shifts its equilibrium */
    double eq = clampd(-ax / 980.0 * 0.9, -0.6, 0.6);
    double aa = -k * (c->ang - eq) - ca * c->w;
    c->w += aa * dt;  c->ang += c->w * dt;

    /* hover / entry scale */
    double ks = kp, cs = 2 * 0.55 * sqrt(ks);
    c->vsc += (ks * (c->sct - c->sc) - cs * c->vsc) * dt;  c->sc += c->vsc * dt;

    /* drop onto the line */
    c->vey += (kp * (0 - c->ey) - cp * c->vey) * dt;  c->ey += c->vey * dt;

    c->alpha = fmin(1.0, c->alpha + dt * 6.0);
    if (c->copied_t > 0) c->copied_t -= dt;
}

static gboolean card_settled(const Card *c) {
    if (c->falling) return FALSE;
    return fabs(c->ang) < 0.0015 && fabs(c->w) < 0.02 &&
           fabs(c->tx - c->x) < 0.15 && fabs(c->vx) < 0.5 &&
           fabs(c->sct - c->sc) < 0.001 && fabs(c->vsc) < 0.01 &&
           fabs(c->ey) < 0.15 && fabs(c->vey) < 0.5 &&
           c->alpha >= 1.0 && c->copied_t <= 0;
}

static void card_free(Card *c) {
    if (c->thumb) cairo_surface_destroy(c->thumb);
    g_free(c->path); g_free(c);
}

/* -------------------------------------------------------------- hit-test */

static void to_local(const Card *c, double mx, double my, double *lx, double *ly) {
    double dx = mx - c->x, dy = my - card_py(c);
    double a = -card_ang(c), co = cos(a), si = sin(a);
    *lx = (dx * co - dy * si) / c->sc;
    *ly = (dx * si + dy * co) / c->sc;
}

static Card *card_at(double mx, double my) {
    for (GList *l = g_list_last(cards); l; l = l->prev) {
        Card *c = l->data;
        if (c->falling) continue;
        double lx, ly; to_local(c, mx, my, &lx, &ly);
        double hw = c->tw / 2.0 + BORDER;
        if (lx >= -hw && lx <= hw && ly >= -12 && ly <= 4 + card_h(c)) return c;
    }
    return NULL;
}

static void update_input_region(void) {
    if (!gtk_widget_get_realized(win)) return;
    cairo_region_t *reg = cairo_region_create();
    for (GList *l = cards; l; l = l->next) {
        Card *c = l->data; if (c->falling) continue;
        double hw = c->tw / 2.0 + BORDER + 6, top = -14, bot = 4 + card_h(c) + 10;
        double xs[4] = { -hw, hw, hw, -hw }, ys[4] = { top, top, bot, bot };
        double a = card_ang(c), co = cos(a), si = sin(a), py = card_py(c);
        double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
        for (int i = 0; i < 4; i++) {
            double px = c->x + (xs[i] * co - ys[i] * si) * c->sc;
            double qy = py   + (xs[i] * si + ys[i] * co) * c->sc;
            minx = fmin(minx, px); maxx = fmax(maxx, px);
            miny = fmin(miny, qy); maxy = fmax(maxy, qy);
        }
        cairo_rectangle_int_t r = { (int)floor(minx), (int)floor(miny),
                                    (int)ceil(maxx - minx) + 1, (int)ceil(maxy - miny) + 1 };
        cairo_region_union_rectangle(reg, &r);
    }
    gtk_widget_input_shape_combine_region(win, reg);
    cairo_region_destroy(reg);
}

/* -------------------------------------------------------------- windowing */

static void apply_layer_config(void) {
    if (layer_ok) {
        gtk_layer_set_layer(GTK_WINDOW(win),
            cfg.reserve ? GTK_LAYER_SHELL_LAYER_TOP : GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_margin(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, cfg.offset);
        /* reserve: windows are laid out below the line; overlay: floats over everything */
        gtk_layer_set_exclusive_zone(GTK_WINDOW(win), cfg.reserve ? WIN_H - 40 : 0);
    } else {
        gtk_window_move(GTK_WINDOW(win), 0, cfg.offset);
    }
}

static void show_window(void) {
    if (!gtk_widget_get_visible(win)) {
        apply_layer_config();
        gtk_widget_show_all(win);
    }
}

static void maybe_hide_window(void) {
    if (!cards && wire_alpha <= 0.01 && gtk_widget_get_visible(win)) {
        gtk_widget_hide(win);   /* gives the reserved space back */
    }
}

/* ------------------------------------------------------------------ tick */

static gboolean on_tick(GtkWidget *w, GdkFrameClock *fc, gpointer ud) {
    gint64 now = gdk_frame_clock_get_frame_time(fc);
    double dt = last_us ? (now - last_us) / 1e6 : 1.0 / 60.0;
    last_us = now;
    dt = clampd(dt, 0.001, 0.05);

    int n = (int)ceil(dt / (1.0 / 240.0));
    double h = dt / n;
    gboolean all_settled = TRUE;

    double wt = cards ? 1.0 : 0.0;
    for (int i = 0; i < n; i++) {
        for (GList *l = cards; l; l = l->next) step_card(l->data, h);
        wire_alpha += (wt - wire_alpha) * fmin(1.0, h * 10.0);
    }

    for (GList *l = cards; l;) {
        Card *c = l->data; GList *next = l->next;
        if (c->falling && (c->alpha <= 0 || c->fy > WIN_H + 200)) {
            cards = g_list_delete_link(cards, l);
            if (press_card == c) press_card = NULL;
            card_free(c);
        } else if (!card_settled(c)) all_settled = FALSE;
        l = next;
    }
    if (fabs(wire_alpha - wt) > 0.01) all_settled = FALSE;

    update_input_region();
    gtk_widget_queue_draw(area);

    if (all_settled) {
        wire_alpha = wt;
        for (GList *l = cards; l; l = l->next) {
            Card *c = l->data; c->ang = c->w = c->vsc = c->vey = c->vx = 0;
            c->x = c->tx; c->sc = c->sct; c->ey = 0;
        }
        gtk_widget_queue_draw(area);
        tick_id = 0; last_us = 0;
        maybe_hide_window();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void start_anim(void) {
    if (!tick_id && area) tick_id = gtk_widget_add_tick_callback(area, on_tick, NULL, NULL);
}

/* ---------------------------------------------------------------- drawing */

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r,     y + r,     r, G_PI, 1.5 * G_PI);
    cairo_close_path(cr);
}

static void draw_wire(cairo_t *cr) {
    if (wire_alpha < 0.01) return;
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int pass = 0; pass < 2; pass++) {
        double dy = pass == 0 ? 1.5 : 0;
        if (pass == 0) cairo_set_source_rgba(cr, 1, 1, 1, 0.25 * wire_alpha);
        else           cairo_set_source_rgba(cr, 0.16, 0.16, 0.19, 0.85 * wire_alpha);
        cairo_set_line_width(cr, pass == 0 ? 1.0 : 1.6);
        cairo_move_to(cr, -10, wire_y(-10) + dy);
        cairo_curve_to(cr, surf_w * 0.33, wire_y(surf_w * 0.33) + dy + SAG * 0.15,
                           surf_w * 0.66, wire_y(surf_w * 0.66) + dy + SAG * 0.15,
                           surf_w + 10, wire_y(surf_w + 10) + dy);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void draw_peg(cairo_t *cr) {
    /* wooden-white clothes-peg, centred on the pin point, clamping the card */
    cairo_pattern_t *g = cairo_pattern_create_linear(-5, 0, 5, 0);
    cairo_pattern_add_color_stop_rgb(g, 0.0, 0.93, 0.93, 0.94);
    cairo_pattern_add_color_stop_rgb(g, 0.5, 1.00, 1.00, 1.00);
    cairo_pattern_add_color_stop_rgb(g, 1.0, 0.78, 0.79, 0.82);
    rrect(cr, -5, -12, 10, 30, 3);
    cairo_set_source(cr, g); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.30); cairo_set_line_width(cr, 0.8); cairo_stroke(cr);
    cairo_pattern_destroy(g);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.28);                 /* jaw split */
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0, 2); cairo_line_to(cr, 0, 17); cairo_stroke(cr);
    cairo_set_source_rgba(cr, 0.35, 0.35, 0.40, 0.9);         /* spring */
    cairo_arc(cr, 0, 1, 1.8, 0, 2 * G_PI); cairo_fill(cr);
}

static void draw_card(cairo_t *cr, Card *c) {
    double py = card_py(c);
    double hw = c->tw / 2.0 + BORDER, ch = card_h(c);
    double alpha = clampd(c->alpha, 0, 1) * (c->dragging ? 0.30 : 1.0);

    cairo_save(cr);
    cairo_translate(cr, c->x, py);
    cairo_rotate(cr, card_ang(c));
    cairo_scale(cr, c->sc, c->sc);

    cairo_push_group(cr);

    /* soft shadow */
    for (int i = 0; i < 5; i++) {
        double g = i * 2.0;
        rrect(cr, -hw - g, 4 - g + 6, 2 * hw + 2 * g, ch + 2 * g, 12 + g);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.055);
        cairo_fill(cr);
    }
    /* white frame */
    rrect(cr, -hw, 4, 2 * hw, ch, 11);
    cairo_set_source_rgb(cr, 0.985, 0.985, 0.99); cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22); cairo_set_line_width(cr, 0.8); cairo_stroke(cr);
    /* screenshot */
    cairo_save(cr);
    rrect(cr, -c->tw / 2.0, 4 + BORDER, c->tw, c->th, 6);
    cairo_clip(cr);
    cairo_set_source_surface(cr, c->thumb, -c->tw / 2.0, 4 + BORDER);
    cairo_paint(cr);
    cairo_restore(cr);
    rrect(cr, -c->tw / 2.0, 4 + BORDER, c->tw, c->th, 6);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12); cairo_set_line_width(cr, 0.6); cairo_stroke(cr);

    draw_peg(cr);

    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);

    /* "Copied" badge, upright, under the card */
    if (c->copied_t > 0) {
        double a = clampd(c->copied_t / 0.35, 0, 1) * clampd((1.4 - c->copied_t) / 0.15 + 0.0001, 0, 1);
        a = clampd(a, 0, 1);
        const char *txt = "\xE2\x9C\x93 Copied";
        cairo_save(cr);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12);
        cairo_text_extents_t te; cairo_text_extents(cr, txt, &te);
        double bw = te.width + 22, bh = 24;
        double bx = c->x - bw / 2, by = py + 4 + (ch + 8) * c->sc + 4;
        rrect(cr, bx, by, bw, bh, bh / 2);
        cairo_set_source_rgba(cr, 0.12, 0.12, 0.13, 0.82 * a); cairo_fill(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, a);
        cairo_move_to(cr, bx + 11 - te.x_bearing, by + bh / 2 - te.y_bearing - te.height / 2);
        cairo_show_text(cr, txt);
        cairo_restore(cr);
    }
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    surf_w = gtk_widget_get_allocated_width(w);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    draw_wire(cr);
    for (GList *l = cards; l; l = l->next) draw_card(cr, l->data);
    return FALSE;
}

/* ------------------------------------------------------------- card store */

static void hang_file(const char *path) {
    if (!path || g_hash_table_contains(seen, path)) return;
    GError *e = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &e);
    if (!pb) { g_warning("tendedero: cannot load %s: %s", path, e ? e->message : "?"); g_clear_error(&e); return; }
    g_hash_table_add(seen, g_strdup(path));

    int ow = gdk_pixbuf_get_width(pb), oh = gdk_pixbuf_get_height(pb);
    double s = fmin((double)MAXW / ow, (double)MAXH / oh);
    int tw = MAX(24, (int)(ow * s)), th = MAX(24, (int)(oh * s));
    GdkPixbuf *sm = gdk_pixbuf_scale_simple(pb, tw * 2, th * 2, GDK_INTERP_HYPER);
    g_object_unref(pb);

    Card *c = g_new0(Card, 1);
    c->path = g_strdup(path);
    c->tw = tw; c->th = th;
    c->thumb = gdk_cairo_surface_create_from_pixbuf(sm, 2, NULL);
    g_object_unref(sm);

    c->sc = c->sct = 1.0;
    c->sc = 1.35;                         /* pops in big, springs down */
    c->ey = 80;  c->vey = -300;           /* rises up to the line */
    c->alpha = 0;
    c->w = (g_random_boolean() ? 1 : -1) * g_random_double_range(3.5, 5.5) * cfg.kick;
    cards = g_list_append(cards, c);
    layout_cards();
    c->x = c->tx;                         /* appears at its slot; others slide aside */

    show_window();
    start_anim();
}

static void card_copy(Card *c) {
    const char *wl = g_find_program_in_path("wl-copy");
    if (wl) {  /* most reliable on layer-shell surfaces */
        char *mime = g_content_type_guess(c->path, NULL, 0, NULL);
        char *cmd  = g_strdup_printf("%s -t '%s' < '%s'", wl, mime ? mime : "image/png", c->path);
        char *argv[] = { "sh", "-c", cmd, NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        g_free(cmd); g_free(mime);
    } else {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(c->path, NULL);
        if (pb) {
            gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), pb);
            g_object_unref(pb);
        }
    }
    c->copied_t = 1.4;
    c->w += (g_random_boolean() ? 1 : -1) * 1.5 * cfg.kick;
    start_anim();
}

static void card_markup(Card *c) {
    char **argv = NULL;
    if (cfg.markup_cmd && *cfg.markup_cmd) {
        char **parts = NULL; int pc = 0;
        if (g_shell_parse_argv(cfg.markup_cmd, &pc, &parts, NULL)) {
            argv = g_new0(char *, pc + 2);
            for (int i = 0; i < pc; i++) argv[i] = g_strdup(parts[i]);
            argv[pc] = g_strdup(c->path);
            g_strfreev(parts);
        }
    } else if (g_find_program_in_path("swappy")) {
        argv = g_new0(char *, 4); argv[0] = g_strdup("swappy"); argv[1] = g_strdup("-f"); argv[2] = g_strdup(c->path);
    } else if (g_find_program_in_path("satty")) {
        argv = g_new0(char *, 4); argv[0] = g_strdup("satty"); argv[1] = g_strdup("--filename"); argv[2] = g_strdup(c->path);
    } else {
        argv = g_new0(char *, 3); argv[0] = g_strdup("xdg-open"); argv[1] = g_strdup(c->path);
    }
    if (argv) {
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        g_strfreev(argv);
    }
    c->w += 3.0;
    start_anim();
}

static void card_let_go(Card *c) {
    if (c->falling) return;
    c->falling = TRUE;
    c->fy = 0; c->fvy = -320;
    c->w += (g_random_boolean() ? 1 : -1) * g_random_double_range(2.0, 4.0);
    c->vx += g_random_double_range(-90, 90);
    layout_cards();
    start_anim();
}

/* -------------------------------------------------------------- pointer io */

static gboolean on_long_press(gpointer ud) {
    long_id = 0;
    if (press_card && !drag_started) { long_fired = TRUE; card_markup(press_card); }
    return G_SOURCE_REMOVE;
}
static void cancel_long(void) { if (long_id) { g_source_remove(long_id); long_id = 0; } }

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    Card *c = card_at(ev->x, ev->y);
    if (!c) return FALSE;
    if (ev->button == 3) { card_let_go(c); return TRUE; }
    if (ev->button != 1) return FALSE;
    press_card = c; press_x = ev->x; press_y = ev->y;
    long_fired = drag_started = FALSE;
    cancel_long();
    long_id = g_timeout_add(LONGPRESS_MS, on_long_press, NULL);
    c->sct = 0.97;                         /* squish under the finger */
    start_anim();
    return TRUE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    if (ev->button != 1 || !press_card) return FALSE;
    cancel_long();
    Card *c = press_card; press_card = NULL;
    c->sct = c->hover ? HOVER_SCALE : 1.0;
    if (!long_fired && !drag_started) card_copy(c);
    start_anim();
    return TRUE;
}

static void on_drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *sd,
                             guint info, guint time, gpointer ud) {
    Card *c = ud;  /* validated through cards list in on_drag_begin */
    if (!g_list_find(cards, c)) return;
    GdkAtom t = gtk_selection_data_get_target(sd);
    if (t == gdk_atom_intern("image/png", FALSE)) {
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(c->path, NULL);
        if (pb) { gtk_selection_data_set_pixbuf(sd, pb); g_object_unref(pb); }
    } else {
        char *uri = g_filename_to_uri(c->path, NULL, NULL);
        gchar *uris[] = { uri, NULL };
        gtk_selection_data_set_uris(sd, uris);
        g_free(uri);
    }
}

static gulong drag_get_handler;

static void cancel_long(void);
static void on_drag_end(GtkWidget *w, GdkDragContext *ctx, gpointer ud) {
    press_card = NULL; cancel_long();   /* DnD swallows the button release */
    for (GList *l = cards; l; l = l->next) {
        Card *c = l->data; c->dragging = FALSE;
        c->sct = 1.0;
    }
    if (drag_get_handler) { g_signal_handler_disconnect(w, drag_get_handler); drag_get_handler = 0; }
    g_signal_handlers_disconnect_by_func(w, on_drag_end, NULL);
    start_anim();
}

static cairo_surface_t *make_drag_icon(Card *c) {
    /* own surface so the card's thumbnail never gets a device offset */
    cairo_surface_t *ic = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, c->tw, c->th);
    cairo_t *cr = cairo_create(ic);
    rrect(cr, 0, 0, c->tw, c->th, 6); cairo_clip(cr);
    cairo_set_source_surface(cr, c->thumb, 0, 0);
    cairo_paint_with_alpha(cr, 0.92);
    cairo_destroy(cr);
    cairo_surface_set_device_offset(ic, -c->tw / 2.0, -c->th / 2.0);
    return ic;
}

static void begin_drag(Card *c, GdkEventMotion *ev) {
    static const GtkTargetEntry targets[] = {
        { "text/uri-list", 0, 0 }, { "image/png", 0, 1 },
    };
    GtkTargetList *tl = gtk_target_list_new(targets, G_N_ELEMENTS(targets));
    drag_get_handler = g_signal_connect(area, "drag-data-get", G_CALLBACK(on_drag_data_get), c);
    g_signal_connect(area, "drag-end", G_CALLBACK(on_drag_end), NULL);
    GdkDragContext *ctx = gtk_drag_begin_with_coordinates(area, tl, GDK_ACTION_COPY, 1,
                                                          (GdkEvent *)ev, (int)ev->x, (int)ev->y);
    gtk_target_list_unref(tl);
    if (ctx) {
        cairo_surface_t *ic = make_drag_icon(c);
        gtk_drag_set_icon_surface(ctx, ic);
        cairo_surface_destroy(ic);
        c->dragging = TRUE;
    } else {                       /* drag refused: undo wiring, treat as a plain press */
        g_signal_handler_disconnect(area, drag_get_handler); drag_get_handler = 0;
        g_signal_handlers_disconnect_by_func(area, on_drag_end, NULL);
    }
    drag_started = TRUE;
    cancel_long();
    start_anim();
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer ud) {
    /* pointer velocity -> gives hovered cards a nudge, like brushing past them */
    double dt = last_mt ? (ev->time - last_mt) / 1000.0 : 0;
    double vx = (dt > 0.001 && dt < 0.2) ? (ev->x - last_mx) / dt : 0;
    last_mx = ev->x; last_my = ev->y; last_mt = ev->time;

    if (press_card && (ev->state & GDK_BUTTON1_MASK) && !drag_started) {
        if (hypot(ev->x - press_x, ev->y - press_y) > DRAG_THRESH) begin_drag(press_card, ev);
        return TRUE;
    }
    Card *h = card_at(ev->x, ev->y);
    for (GList *l = cards; l; l = l->next) {
        Card *c = l->data;
        gboolean hv = (c == h);
        if (hv != c->hover) { c->hover = hv; if (!press_card) c->sct = hv ? HOVER_SCALE : 1.0; start_anim(); }
    }
    if (h && !h->falling && fabs(vx) > 40) {
        h->w = clampd(h->w - vx * 0.0025 * cfg.kick, -8, 8);
        start_anim();
    }
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer ud) {
    last_mt = 0;
    for (GList *l = cards; l; l = l->next) {
        Card *c = l->data;
        if (c->hover && !press_card) { c->hover = FALSE; c->sct = 1.0; }
    }
    start_anim();
    return FALSE;
}

/* ------------------------------------------------------------ watch / capture */

static GHashTable *pending;   /* path -> timeout id */

static gboolean pending_fire(gpointer path) {
    g_hash_table_remove(pending, path);
    char *low = g_ascii_strdown(path, -1);
    gboolean img = g_str_has_suffix(low, ".png") || g_str_has_suffix(low, ".jpg") ||
                   g_str_has_suffix(low, ".jpeg") || g_str_has_suffix(low, ".webp");
    g_free(low);
    if (img) hang_file(path);
    return G_SOURCE_REMOVE;
}

static void on_dir_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer ud) {
    if (ev != G_FILE_MONITOR_EVENT_CREATED && ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
        ev != G_FILE_MONITOR_EVENT_MOVED_IN) return;
    char *path = g_file_get_path(f);
    if (!path) return;
    char *base = g_path_get_basename(path);
    if (base[0] == '.') { g_free(base); g_free(path); return; }
    g_free(base);
    gpointer old = g_hash_table_lookup(pending, path);
    if (old) g_source_remove(GPOINTER_TO_UINT(old));
    /* debounce: let the writer finish */
    guint id = g_timeout_add(350, pending_fire, g_strdup(path));
    g_hash_table_insert(pending, path, GUINT_TO_POINTER(id));
}

static void restart_watch(void) {
    if (monitor) { g_file_monitor_cancel(monitor); g_clear_object(&monitor); }
    if (!cfg.watch || !cfg.watch_dir) return;
    GFile *d = g_file_new_for_path(cfg.watch_dir);
    GError *e = NULL;
    monitor = g_file_monitor_directory(d, G_FILE_MONITOR_WATCH_MOVES, NULL, &e);
    if (monitor) g_signal_connect(monitor, "changed", G_CALLBACK(on_dir_changed), NULL);
    else { g_warning("tendedero: cannot watch %s: %s", cfg.watch_dir, e ? e->message : "?"); g_clear_error(&e); }
    g_object_unref(d);
}

/* xdg-desktop-portal Screenshot (GNOME, KDE, ...) */
typedef struct { GDBusConnection *c; guint sub; } PortalReq;

static void on_portal_response(GDBusConnection *c, const gchar *sender, const gchar *path,
                               const gchar *iface, const gchar *sig, GVariant *params, gpointer ud) {
    PortalReq *r = ud;
    guint32 code; GVariant *res;
    g_variant_get(params, "(u@a{sv})", &code, &res);
    if (code == 0) {
        const char *uri = NULL;
        if (g_variant_lookup(res, "uri", "&s", &uri) && uri) {
            char *p = g_filename_from_uri(uri, NULL, NULL);
            if (p) { hang_file(p); g_free(p); }
        }
    }
    g_variant_unref(res);
    g_dbus_connection_signal_unsubscribe(r->c, r->sub);
    g_object_unref(r->c); g_free(r);
}

static void do_capture(void) {
    if (cfg.capture_cmd && *cfg.capture_cmd) {
        g_spawn_command_line_async(cfg.capture_cmd, NULL);
        return;
    }
    GError *e = NULL;
    GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
    if (!c) { g_warning("tendedero: no session bus: %s", e->message); g_clear_error(&e); return; }
    char *sender = g_strdup(g_dbus_connection_get_unique_name(c) + 1);
    for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
    char *token = g_strdup_printf("tendedero%u", g_random_int());
    char *rpath = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

    PortalReq *r = g_new0(PortalReq, 1);
    r->c = g_object_ref(c);
    r->sub = g_dbus_connection_signal_subscribe(c, "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request", "Response", rpath, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_portal_response, r, NULL);

    GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&b, "{sv}", "interactive", g_variant_new_boolean(TRUE));
    g_dbus_connection_call(c, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Screenshot", "Screenshot",
        g_variant_new("(sa{sv})", "", &b), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_free(sender); g_free(token); g_free(rpath); g_object_unref(c);
}

/* ----------------------------------------------------------- sample image */

static void hang_sample(void) {
    static int n;
    int W = 640, H = 420 - (n % 3) * 60;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.985); cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.12);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 22); cairo_move_to(cr, 28, 46); cairo_show_text(cr, "Monthly recurring revenue");
    cairo_set_font_size(cr, 54); cairo_move_to(cr, 28, 112); cairo_show_text(cr, "$48.2K");
    cairo_move_to(cr, 28, H - 30);
    cairo_set_source_rgb(cr, 0.15, 0.45, 0.95); cairo_set_line_width(cr, 4);
    double phase = n * 0.9;
    for (int i = 0; i <= 24; i++) {
        double x = 28 + i * (W - 56) / 24.0;
        double y = H - 40 - (i * (H - 190) / 24.0) - 14 * sin(i * 0.7 + phase);
        if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    char *path = g_strdup_printf("%s/tendedero-sample-%d-%u.png", g_get_tmp_dir(), n++, g_random_int() % 10000);
    cairo_surface_write_to_png(s, path);
    cairo_destroy(cr); cairo_surface_destroy(s);
    hang_file(path); g_free(path);
}

/* --------------------------------------------------------------- settings */

static void on_offset(GtkSpinButton *sp, gpointer ud) {
    cfg.offset = gtk_spin_button_get_value_as_int(sp);
    apply_layer_config(); config_save();
}
static gboolean on_reserve(GtkSwitch *sw, gboolean state, gpointer ud) {
    cfg.reserve = state;
    gtk_switch_set_state(sw, state);
    apply_layer_config(); config_save();
    return TRUE;
}
static void on_stiff(GtkRange *r, gpointer ud) { cfg.stiffness = gtk_range_get_value(r); config_save(); }
static void on_damp (GtkRange *r, gpointer ud) { cfg.damping   = gtk_range_get_value(r); config_save(); }
static void on_kick (GtkRange *r, gpointer ud) { cfg.kick      = gtk_range_get_value(r); config_save(); }
static gboolean on_watch(GtkSwitch *sw, gboolean state, gpointer ud) {
    cfg.watch = state; gtk_switch_set_state(sw, state); restart_watch(); config_save(); return TRUE;
}
static void on_watchdir(GtkEntry *e, gpointer ud) {
    g_free(cfg.watch_dir); cfg.watch_dir = g_strdup(gtk_entry_get_text(e));
    restart_watch(); config_save();
}
static gboolean on_watchdir_focus(GtkWidget *e, GdkEventFocus *f, gpointer ud) { on_watchdir(GTK_ENTRY(e), NULL); return FALSE; }
static void on_test(GtkButton *b, gpointer ud)    { hang_sample(); }
static void on_capture_btn(GtkButton *b, gpointer ud) { do_capture(); }
static void on_quit_btn(GtkButton *b, gpointer ud) { g_application_quit(g_application_get_default()); }

static GtkWidget *scale_row(GtkWidget *grid, int row, const char *label, double lo, double hi,
                            double step, double val, GCallback cb, const char *tip) {
    GtkWidget *l = gtk_label_new(label); gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_range_set_value(GTK_RANGE(s), val);
    gtk_widget_set_hexpand(s, TRUE); gtk_widget_set_size_request(s, 240, -1);
    gtk_widget_set_tooltip_text(s, tip);
    g_signal_connect(s, "value-changed", cb, NULL);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), s, 1, row, 1, 1);
    return s;
}

static void show_settings(void) {
    if (settings_win) { gtk_window_present(GTK_WINDOW(settings_win)); return; }
    settings_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(settings_win), "Tendedero");
    gtk_window_set_default_size(GTK_WINDOW(settings_win), 460, -1);
    g_signal_connect(settings_win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10); gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 18);
    gtk_container_add(GTK_CONTAINER(settings_win), grid);
    int r = 0;

    GtkWidget *h = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(h), "<b>Position</b>"); gtk_widget_set_halign(h, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), h, 0, r++, 2, 1);

    GtkWidget *l = gtk_label_new("Space from top (px)"); gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *sp = gtk_spin_button_new_with_range(0, 2000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp), cfg.offset);
    g_signal_connect(sp, "value-changed", G_CALLBACK(on_offset), NULL);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), sp, 1, r++, 1, 1);

    l = gtk_label_new("Reserve space (windows tile below)"); gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *sw = gtk_switch_new(); gtk_widget_set_halign(sw, GTK_ALIGN_START);
    gtk_switch_set_active(GTK_SWITCH(sw), cfg.reserve); gtk_switch_set_state(GTK_SWITCH(sw), cfg.reserve);
    gtk_widget_set_tooltip_text(sw, "On: the clothesline claims screen space (exclusive zone), windows never go under it.\n"
                                    "Off: it floats above everything, like an always-on-top overlay.");
    g_signal_connect(sw, "state-set", G_CALLBACK(on_reserve), NULL);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), sw, 1, r++, 1, 1);

    h = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h), "<b>Spring physics</b>");
    gtk_widget_set_halign(h, GTK_ALIGN_START); gtk_widget_set_margin_top(h, 8);
    gtk_grid_attach(GTK_GRID(grid), h, 0, r++, 2, 1);
    scale_row(grid, r++, "Stiffness", 30, 600, 5, cfg.stiffness, G_CALLBACK(on_stiff), "Higher = quicker, tighter swing");
    scale_row(grid, r++, "Damping",   0.05, 1.2, 0.01, cfg.damping, G_CALLBACK(on_damp), "Low = swings for ages, 1 = dead stop");
    scale_row(grid, r++, "Swing kick", 0, 3, 0.05, cfg.kick, G_CALLBACK(on_kick), "How hard new cards and the mouse push them");

    h = gtk_label_new(NULL); gtk_label_set_markup(GTK_LABEL(h), "<b>Capture</b>");
    gtk_widget_set_halign(h, GTK_ALIGN_START); gtk_widget_set_margin_top(h, 8);
    gtk_grid_attach(GTK_GRID(grid), h, 0, r++, 2, 1);

    l = gtk_label_new("Hang new files in folder"); gtk_widget_set_halign(l, GTK_ALIGN_START);
    sw = gtk_switch_new(); gtk_widget_set_halign(sw, GTK_ALIGN_START);
    gtk_switch_set_active(GTK_SWITCH(sw), cfg.watch); gtk_switch_set_state(GTK_SWITCH(sw), cfg.watch);
    g_signal_connect(sw, "state-set", G_CALLBACK(on_watch), NULL);
    gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1); gtk_grid_attach(GTK_GRID(grid), sw, 1, r++, 1, 1);

    GtkWidget *en = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(en), cfg.watch_dir);
    g_signal_connect(en, "activate", G_CALLBACK(on_watchdir), NULL);
    g_signal_connect(en, "focus-out-event", G_CALLBACK(on_watchdir_focus), NULL);
    gtk_grid_attach(GTK_GRID(grid), en, 0, r++, 2, 1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_set_margin_top(box, 10);
    GtkWidget *b1 = gtk_button_new_with_label("Capture…"), *b2 = gtk_button_new_with_label("Hang a test card"),
              *b3 = gtk_button_new_with_label("Quit");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_capture_btn), NULL);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_test), NULL);
    g_signal_connect(b3, "clicked", G_CALLBACK(on_quit_btn), NULL);
    gtk_box_pack_start(GTK_BOX(box), b1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), b2, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), b3, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), box, 0, r++, 2, 1);

    gtk_widget_show_all(settings_win);
}

/* -------------------------------------------------------------- application */

static void build_window(void) {
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Tendedero");
    gtk_widget_set_app_paintable(win, TRUE);
    GdkScreen *scr = gtk_widget_get_screen(win);
    GdkVisual *vis = gdk_screen_get_rgba_visual(scr);
    if (vis) gtk_widget_set_visual(win, vis);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 1, WIN_H);

    layer_ok = gtk_layer_is_supported();
    if (layer_ok) {
        gtk_layer_init_for_window(GTK_WINDOW(win));
        gtk_layer_set_namespace(GTK_WINDOW(win), "tendedero");
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP,   TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT,  TRUE);
        gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    } else {
        g_printerr("tendedero: compositor lacks wlr-layer-shell (GNOME/Mutter?). "
                   "Falling back to a plain window: no reserved space, no guaranteed on-top.\n");
        GdkRectangle g; GdkDisplay *d = gdk_display_get_default();
        GdkMonitor *m = gdk_display_get_primary_monitor(d); if (!m) m = gdk_display_get_monitor(d, 0);
        gdk_monitor_get_geometry(m, &g);
        gtk_window_set_default_size(GTK_WINDOW(win), g.width, WIN_H);
        gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
        gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
        gtk_window_stick(GTK_WINDOW(win));
        surf_w = g.width;
    }

    area = gtk_drawing_area_new();
    gtk_widget_add_events(area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                                GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(area, "button-press-event", G_CALLBACK(on_button_press), NULL);
    g_signal_connect(area, "button-release-event", G_CALLBACK(on_button_release), NULL);
    g_signal_connect(area, "motion-notify-event", G_CALLBACK(on_motion), NULL);
    g_signal_connect(area, "leave-notify-event", G_CALLBACK(on_leave), NULL);
    gtk_container_add(GTK_CONTAINER(win), area);
    apply_layer_config();
    /* realised lazily on first card; nothing is mapped (or reserved) until then */
}

static void on_startup(GApplication *app, gpointer ud) {
    g_application_hold(app);
    config_load();
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    build_window();
    restart_watch();
}

static int on_cmdline(GApplication *app, GApplicationCommandLine *cl, gpointer ud) {
    gint argc; gchar **argv = g_application_command_line_get_arguments(cl, &argc);
    gboolean capture = FALSE, settings = FALSE, quit = FALSE, sample = FALSE;
    gchar **adds = NULL, **rest = NULL;
    GOptionEntry ents[] = {
        { "capture",  'c', 0, G_OPTION_ARG_NONE, &capture,  "Take a screenshot (portal, or capture_cmd) and hang it", NULL },
        { "add",      'a', 0, G_OPTION_ARG_FILENAME_ARRAY, &adds, "Hang an existing image file", "FILE" },
        { "settings", 's', 0, G_OPTION_ARG_NONE, &settings, "Open settings", NULL },
        { "sample",     0, 0, G_OPTION_ARG_NONE, &sample,   "Hang a generated test card", NULL },
        { "quit",     'q', 0, G_OPTION_ARG_NONE, &quit,     "Stop the running instance", NULL },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &rest, NULL, "[FILE...]" },
        { NULL }
    };
    GOptionContext *ctx = g_option_context_new("- screenshots, hung out to dry");
    g_option_context_add_main_entries(ctx, ents, NULL);
    GError *e = NULL;
    if (!g_option_context_parse_strv(ctx, &argv, &e)) {
        g_application_command_line_printerr(cl, "%s\n", e->message); g_clear_error(&e);
        g_option_context_free(ctx); return 1;
    }
    for (char **p = adds; p && *p; p++) { GFile *f = g_application_command_line_create_file_for_arg(cl, *p); char *s = g_file_get_path(f); hang_file(s); g_free(s); g_object_unref(f); }
    for (char **p = rest; p && *p; p++) { GFile *f = g_application_command_line_create_file_for_arg(cl, *p); char *s = g_file_get_path(f); hang_file(s); g_free(s); g_object_unref(f); }
    if (sample)   hang_sample();
    if (capture)  do_capture();
    if (settings) show_settings();
    if (quit)     g_application_quit(app);
    g_option_context_free(ctx);
    return 0;
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_cmdline), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}

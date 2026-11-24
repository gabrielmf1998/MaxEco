/* MaxEco tray icon.
 *
 * A StatusNotifierItem (the tray protocol of KDE Plasma, and of GNOME with
 * the AppIndicator extension) whose menu picks one of three levels, like a
 * slider: Normal, Eco and MaxEco. The mouse wheel over the icon moves
 * between them too. Eco starts maxeco-level@eco.service, MaxEco
 * maxeco-level@max.service (both pull in maxeco.service, which saves the
 * settings first) and Normal stops maxeco.service, which puts everything
 * back. Screens are only touched when "60 Hz Screens in MaxEco" is checked:
 * on KDE Plasma every screen faster than 60 Hz then goes to 60 Hz while
 * MaxEco is on (through kscreen-doctor).
 *
 * The program sleeps in ppoll() and only wakes up when the tray host talks to
 * it, so it costs no CPU while idle.
 */
#include "rt.h"
#include "dbus.h"

#define MAXECO_VERSION "1.2.0"
#define SNI_PATH "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"
#define STATE_FILE_DEFAULT "/run/maxeco/state"
#define LEVEL_FILE_DEFAULT "/run/maxeco/level"
#define WATCHER "org.kde.StatusNotifierWatcher"
#define KSCREEN_DEFAULT "/usr/bin/kscreen-doctor"

enum { ID_ROOT, ID_NORMAL, ID_ECO, ID_MAX, ID_SEP1, ID_HZ, ID_AUTO, ID_SEP2, ID_QUIT, ID_LAST = ID_QUIT };
enum { LV_NORMAL, LV_ECO, LV_MAX };
static const char *const level_label[] = {"Normal", "Eco", "MaxEco"};
static const char *const level_word[] = {"normal", "eco", "max"};

static struct db_conn sess;
static unsigned char rep_buf[8192], sig_buf[1024];
static char sni_name[64];
static int level, want, busy, want_quit, autostart_on, hz60, scroll_acc;
static int child_pid, child_target;
static unsigned menu_rev = 1;
static const char *STATE_FILE = STATE_FILE_DEFAULT, *LEVEL_FILE = LEVEL_FILE_DEFAULT;
static const char *systemctl_path = "/usr/bin/systemctl";
static const char *kscreen_path = KSCREEN_DEFAULT;
static int debug;
static char p_state[512], p_auto[512], p_cfg_maxeco[512], p_cfg_auto[512], self_exe[512];
static char p_display[512], p_auto_display[512], p_hz[512];

/* -------------------------------------------------------- small helpers */
static void path_join(char *out, rt_size cap, const char *a, const char *b)
{
    struct rt_sb s;
    sb_init(&s, out, cap);
    sb_add(&s, a), sb_add(&s, b);
    if (!s.ok) out[0] = 0;
}

/* mkdir -p, for ~/.config/maxeco and ~/.config/autostart */
static void mkdir_p(const char *path)
{
    char b[512];
    rt_size n = rt_strlen(path);
    if (!n || n >= sizeof b) return;
    memcpy(b, path, n + 1);
    for (rt_size i = 1; i <= n; i++)
        if (b[i] == '/' || b[i] == 0) {
            char c = b[i];
            b[i] = 0;
            rt_mkdir(b, 0755);
            b[i] = c;
        }
}

static int read_small(const char *path, char *buf, rt_size cap)
{
    int fd = rt_open(path, RT_O_RDONLY | RT_O_CLOEXEC, 0);
    if (fd < 0) return -1;
    rt_ssize n = rt_read(fd, buf, cap - 1);
    rt_close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int write_file(const char *path, const char *text)
{
    char tmp[600];
    path_join(tmp, sizeof tmp, path, ".tmp");
    if (!*path || !*tmp) return -1;
    int fd = rt_open(tmp, RT_O_WRONLY | RT_O_CREAT | RT_O_TRUNC | RT_O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int r = rt_write_all(fd, text, rt_strlen(text));
    rt_close(fd);
    if (r < 0 || rt_rename(tmp, path) < 0) {
        rt_unlink(tmp);
        return -1;
    }
    return 0;
}

/* the level in use: from the root helper's files */
static int actual_level(void)
{
    char b[8];
    if (!rt_exists(STATE_FILE)) return LV_NORMAL;
    if (read_small(LEVEL_FILE, b, sizeof b) >= 3 && b[0] == 'e' && b[1] == 'c' && b[2] == 'o') return LV_ECO;
    return LV_MAX; /* saved settings without a level: MaxEco 1.1 */
}

/* the level chosen by the user, applied again at the next login */
static int saved_level(void)
{
    char b[8];
    int n = read_small(p_state, b, sizeof b);
    if (n >= 3 && b[0] == 'e' && b[1] == 'c' && b[2] == 'o') return LV_ECO;
    if (n >= 2 && ((b[0] == 'm' && b[1] == 'a') || (b[0] == 'o' && b[1] == 'n'))) return LV_MAX; /* "on" = 1.1 */
    return LV_NORMAL;
}

static void save_level(int l)
{
    char text[16];
    struct rt_sb s;
    if (!*p_state) return;
    sb_init(&s, text, sizeof text);
    sb_add(&s, level_word[l]), sb_add(&s, "\n");
    mkdir_p(p_cfg_maxeco);
    write_file(p_state, text);
}

static void save_hz(int on)
{
    if (!*p_hz) return;
    if (on) {
        mkdir_p(p_cfg_maxeco);
        write_file(p_hz, "on\n");
    } else {
        rt_unlink(p_hz);
    }
}

static int autostart_enabled(void)
{
    char b[2048];
    if (read_small(p_auto, b, sizeof b) < 0) return 0;
    for (char *p = b; *p; p++)
        if ((p == b || p[-1] == '\n') && rt_startswith(p, "Hidden=true")) return 0;
    return 1;
}

static void display_login_entry(void);

static void set_autostart(int on)
{
    if (!*p_auto) return;
    if (on) {
        char text[1024];
        struct rt_sb s;
        int quote = 0;
        for (const char *p = self_exe; *p; p++)
            if (*p == ' ') quote = 1;
        sb_init(&s, text, sizeof text);
        sb_add(&s, "[Desktop Entry]\nType=Application\nName=MaxEco\n"
                   "Comment=Power level switch in the system tray\nExec=");
        sb_add(&s, quote ? "\"" : ""), sb_add(&s, self_exe), sb_add(&s, quote ? "\"" : "");
        sb_add(&s, "\nIcon=maxeco-on\nTerminal=false\nX-GNOME-Autostart-enabled=true\n");
        mkdir_p(p_cfg_auto);
        if (s.ok) write_file(p_auto, text);
    } else {
        rt_unlink(p_auto);
    }
    autostart_on = autostart_enabled();
    display_login_entry();
}

/* ------------------------------------------------------------- screens */
/* Runs argv with its output in buf (when buf is set) and waits for it, at
 * most timeout_ms. Returns the exit code, or -1. */
static int run_wait(char *const *argv, char *buf, rt_size cap, int timeout_ms)
{
    int p[2];
    if (rt_pipe(p) < 0) return -1;
    int pid = rt_fork();
    if (pid < 0) {
        rt_close(p[0]), rt_close(p[1]);
        return -1;
    }
    if (pid == 0) {
        unsigned long none = 0;
        rt_sigmask(RT_SIG_SETMASK, &none);
        rt_dup2(p[1], 1);
        int nul = rt_open("/dev/null", RT_O_RDONLY, 0);
        if (nul >= 0) rt_dup2(nul, 0);
        rt_execve(argv[0], argv, rt_envp);
        rt_exit(127);
    }
    rt_close(p[1]);
    rt_size len = 0;
    for (int left = timeout_ms;;) {
        struct rt_pollfd f = {p[0], RT_POLLIN, 0};
        struct rt_timespec slice = {0, 100000000L};
        int n = rt_poll(&f, 1, &slice);
        if (n == -RT_EINTR) continue;
        if (n < 0) break;
        if (n == 0) {
            if ((left -= 100) > 0) continue;
            rt_kill(pid, RT_SIGKILL);
            break;
        }
        char junk[256];
        int keep = buf && len + 1 < cap;
        rt_ssize r = rt_read(p[0], keep ? buf + len : junk, keep ? cap - 1 - len : sizeof junk);
        if (r == -RT_EINTR) continue;
        if (r <= 0) break;
        if (keep) len += (rt_size)r;
    }
    if (buf) buf[len] = 0;
    rt_close(p[0]);
    int st = 0;
    while (rt_wait(pid, &st, 0) == -RT_EINTR) {}
    return (st & 0x7f) == 0 ? (st >> 8) & 0xff : -1;
}

/* What "kscreen-doctor -o" prints, one block per screen:
 *   Output: 2 DP-4 <uuid>
 *       enabled
 *       Modes:  26:1920x1080@240.00*!  29:1920x1080@60.00 ...
 * with colour codes around some words; "*" marks the current mode. */
#define MAX_OUT 8
#define MAX_MODES 160
struct mode { int id, w, h, r; }; /* r = refresh rate x 100 */
struct output { char name[40]; int enabled, nm, cur; struct mode m[MAX_MODES]; };
struct screens { int n; struct output o[MAX_OUT]; char text[65536]; };

static void strip_colours(char *s)
{
    char *d = s;
    for (char *p = s; *p;) {
        if (*p != 033) {
            *d++ = *p++;
            continue;
        }
        if (*++p == '[')
            for (p++; *p && !(*p >= '@' && *p <= '~'); p++) {}
        if (*p) p++;
    }
    *d = 0;
}

static int read_int(const char **p)
{
    int v = 0, any = 0;
    while (**p >= '0' && **p <= '9' && v < 100000000) v = v * 10 + (*(*p)++ - '0'), any = 1;
    return any ? v : -1;
}

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static void parse_modes(struct output *o, const char *p)
{
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return;
        int id = read_int(&p), w = -1, h = -1, ri = -1, rf = 0, cur = 0;
        if (id >= 0 && *p == ':') {
            p++, w = read_int(&p);
            if (*p == 'x') p++, h = read_int(&p);
            if (*p == '@') p++, ri = read_int(&p);
            if (*p == '.') {
                p++;
                for (int k = 0; k < 2; k++, rf *= 10)
                    if (*p >= '0' && *p <= '9') rf += *p++ - '0';
                rf /= 10;
            }
        }
        for (; *p && *p != ' ' && *p != '\t'; p++)
            if (*p == '*') cur = 1;
        if (w > 0 && h > 0 && ri >= 0 && o->nm < MAX_MODES) {
            o->m[o->nm] = (struct mode){id, w, h, ri * 100 + rf};
            if (cur) o->cur = o->nm;
            o->nm++;
        }
    }
}

static void parse_screens(struct screens *s)
{
    struct output *o = 0;
    strip_colours(s->text);
    s->n = 0;
    for (char *line = s->text; *line;) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char keep = *end;
        *end = 0;
        const char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (rt_startswith(t, "Output:")) {
            o = s->n < MAX_OUT ? &s->o[s->n++] : 0;
            if (o) {
                memset(o, 0, sizeof *o);
                o->cur = -1;
                t += 7;
                while (*t == ' ') t++;
                while (*t >= '0' && *t <= '9') t++; /* the output number */
                while (*t == ' ') t++;
                rt_size n = 0;
                while (name_char(t[n]) && n + 1 < sizeof o->name) o->name[n] = t[n], n++;
                if (!n || (t[n] && t[n] != ' ')) s->n--, o = 0; /* not a name we can pass on */
                else if (o)
                    for (t += n; *t; t++)
                        if (rt_startswith(t, " enabled")) o->enabled = 1;
            }
        } else if (o && rt_streq(t, "enabled")) {
            o->enabled = 1;
        } else if (o && rt_startswith(t, "Modes:")) {
            parse_modes(o, t + 6);
        }
        *end = keep;
        line = keep ? end + 1 : end;
    }
}

static struct screens *screens_read(void)
{
    if (!rt_exists(kscreen_path)) return 0;
    struct screens *s = rt_map(sizeof *s);
    char *argv[] = {(char *)kscreen_path, "-o", 0};
    if (!s) return 0;
    if (run_wait(argv, s->text, sizeof s->text, 5000) != 0) {
        rt_unmap(s, sizeof *s);
        return 0;
    }
    parse_screens(s);
    return s;
}

/* Runs kscreen-doctor output.NAME.mode.ID ... for the n requests. */
static int screens_apply(char args[][80], int n)
{
    char *argv[MAX_OUT + 2];
    argv[0] = (char *)kscreen_path;
    for (int i = 0; i < n; i++) argv[i + 1] = args[i];
    argv[n + 1] = 0;
    return n ? run_wait(argv, 0, 0, 10000) : 0;
}

static void mode_arg(char *out, const struct output *o, const struct mode *m)
{
    struct rt_sb b;
    sb_init(&b, out, 80);
    sb_add(&b, "output."), sb_add(&b, o->name), sb_add(&b, ".mode."), sb_num(&b, (unsigned long)m->id);
}

static int dist(int a, int b) { return a > b ? a - b : b - a; }

/* The mode with the same size as `size` whose rate is closest to `rate`;
 * with below = 1 only rates under the current one count. */
static int closest_mode(const struct output *o, const struct mode *size, int rate, int below)
{
    int best = -1;
    for (int i = 0; i < o->nm; i++) {
        const struct mode *m = &o->m[i];
        if (m->w != size->w || m->h != size->h) continue;
        if (below && (m->r >= size->r || m->r < 4800)) continue;
        if (best < 0 || dist(m->r, rate) < dist(o->m[best].r, rate)) best = i;
    }
    return best;
}

/* While a screen is changed and the tray does not start with the session, a
 * small autostart entry puts the screens back at the next login, in case the
 * PC was turned off in MaxEco. */
static void display_login_entry(void)
{
    if (!*p_auto_display) return;
    if (autostart_on || !rt_exists(p_display)) {
        rt_unlink(p_auto_display);
        return;
    }
    char text[1024];
    struct rt_sb s;
    int quote = 0;
    for (const char *p = self_exe; *p; p++)
        if (*p == ' ') quote = 1;
    sb_init(&s, text, sizeof text);
    sb_add(&s, "[Desktop Entry]\nType=Application\nName=MaxEco screen restore\n"
               "Comment=Puts the screens back at their refresh rate after MaxEco\nExec=");
    sb_add(&s, quote ? "\"" : ""), sb_add(&s, self_exe), sb_add(&s, quote ? "\"" : "");
    sb_add(&s, " --restore-display\nNoDisplay=true\nTerminal=false\nX-GNOME-Autostart-enabled=true\n");
    mkdir_p(p_cfg_auto);
    if (s.ok) write_file(p_auto_display, text);
}

/* MaxEco with 60 Hz screens: screens above 60 Hz go to 60 Hz (or the closest rate below).
 * The previous modes are kept in ~/.config/maxeco/display; a screen that is
 * already listed there keeps its first saved mode. */
static void display_eco(void)
{
    if (!*p_display) return;
    struct screens *s = screens_read();
    if (!s) return;
    char saved[1024], args[MAX_OUT][80];
    struct rt_sb add;
    sb_init(&add, saved, sizeof saved);
    int n = read_small(p_display, saved, sizeof saved), na = 0;
    if (n > 0) add.len = (rt_size)n;
    for (int i = 0; i < s->n; i++) {
        struct output *o = &s->o[i];
        if (!o->enabled || o->cur < 0) continue;
        const struct mode *c = &o->m[o->cur];
        int to = c->r > 6100 ? closest_mode(o, c, 6000, 1) : -1;
        if (to < 0) continue;
        mode_arg(args[na++], o, &o->m[to]);
        char head[48];
        struct rt_sb h;
        sb_init(&h, head, sizeof head);
        sb_add(&h, o->name), sb_add(&h, " ");
        int listed = 0;
        for (const char *p = saved; n > 0 && *p;) {
            if (rt_startswith(p, head)) listed = 1;
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
        if (listed) continue;
        sb_add(&add, head), sb_num(&add, (unsigned long)c->w), sb_add(&add, " ");
        sb_num(&add, (unsigned long)c->h), sb_add(&add, " "), sb_num(&add, (unsigned long)c->r);
        sb_add(&add, "\n");
    }
    if (na && add.ok && add.len) {
        mkdir_p(p_cfg_maxeco);
        if (write_file(p_display, saved) == 0) {
            if (screens_apply(args, na) != 0) rt_log("kscreen-doctor could not change the refresh rate", 0);
            display_login_entry();
        }
    }
    rt_unmap(s, sizeof *s);
}

/* Every saved screen goes back to its mode. Returns 0 when there is nothing
 * left to restore (and then runs nothing, so the screens do not blink). */
static int display_restore(void)
{
    char saved[1024], args[MAX_OUT][80];
    if (!*p_display || read_small(p_display, saved, sizeof saved) < 0) return 0;
    struct screens *s = screens_read();
    if (!s) return -1; /* try again next time */
    int na = 0;
    for (const char *p = saved; *p && na < MAX_OUT;) {
        char name[40];
        rt_size k = 0;
        while (name_char(*p) && k + 1 < sizeof name) name[k++] = *p++;
        name[k] = 0;
        struct mode was = {0, 0, 0, 0};
        if (*p == ' ') p++, was.w = read_int(&p);
        if (*p == ' ') p++, was.h = read_int(&p);
        if (*p == ' ') p++, was.r = read_int(&p);
        while (*p && *p != '\n') p++;
        if (*p) p++;
        for (int i = 0; k && was.w > 0 && was.h > 0 && was.r > 0 && i < s->n; i++) {
            struct output *o = &s->o[i];
            if (!rt_streq(o->name, name) || !o->enabled) continue;
            int to = closest_mode(o, &was, was.r, 0);
            if (to >= 0 && to != o->cur) mode_arg(args[na++], o, &o->m[to]);
        }
    }
    rt_unmap(s, sizeof *s);
    if (screens_apply(args, na) != 0) {
        rt_log("kscreen-doctor could not restore the refresh rate", 0);
        return -1;
    }
    rt_unlink(p_display);
    display_login_entry();
    return 0;
}

/* ------------------------------------------------------------- signals */
/* screens follow the level: 60 Hz only in MaxEco with the option checked */
static void screens_for_level(void)
{
    if (level == LV_MAX && hz60) display_eco();
    else display_restore();
}

/* the icon and the menu show the level asked for, so the wheel feels instant */
static const char *icon_name(void)
{
    return want == LV_MAX ? "maxeco-on" : want == LV_ECO ? "maxeco-eco" : "maxeco-off";
}

static const char *tooltip_text(void)
{
    if (busy || want != level)
        return want == LV_MAX ? "Switching to MaxEco" : want == LV_ECO ? "Switching to Eco" : "Switching to Normal";
    if (level == LV_ECO) return "Eco. CPU and GPU use less power and games still run, quiet fans.\nThe mouse wheel here changes the level.";
    if (level == LV_MAX)
        return hz60 ? "MaxEco. CPU and GPU at their lowest power, quiet fans, screens at 60 Hz.\nThe mouse wheel here changes the level."
                    : "MaxEco. CPU and GPU at their lowest power, quiet fans.\nThe mouse wheel here changes the level.";
    return "Normal. Everything runs as usual.\nThe mouse wheel here changes the level.";
}

static void emit(const char *path, const char *iface, const char *member, const char *sig,
                 const struct db_w *body)
{
    db_send(&sess, DB_SIGNAL, 0, 0, path, iface, member, 0, 0, sig, body);
}

static void refresh_ui(void)
{
    struct db_w w;
    dw_init(&w, sig_buf, sizeof sig_buf);
    dw_u32(&w, ++menu_rev);
    dw_u32(&w, 0);
    emit(MENU_PATH, "com.canonical.dbusmenu", "LayoutUpdated", "ui", &w);
    emit(SNI_PATH, "org.kde.StatusNotifierItem", "NewIcon", "", 0);
    emit(SNI_PATH, "org.kde.StatusNotifierItem", "NewToolTip", "", 0);
}

static void notify(const char *summary, const char *text)
{
    struct db_w w;
    dw_init(&w, sig_buf, sizeof sig_buf);
    dw_str(&w, "MaxEco");
    dw_u32(&w, 0);
    dw_str(&w, "maxeco-off");
    dw_str(&w, summary);
    dw_str(&w, text);
    unsigned a = dw_abegin(&w, 4);
    dw_aend(&w, a, 4);
    a = dw_abegin(&w, 8);
    dw_aend(&w, a, 8);
    dw_u32(&w, (unsigned)-1);
    db_call(&sess, DB_NO_REPLY, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "Notify", "susssasa{sv}i", &w);
}

static void register_sni(void)
{
    struct db_w w;
    dw_init(&w, sig_buf, sizeof sig_buf);
    dw_str(&w, sni_name);
    db_call(&sess, 0, WATCHER, "/StatusNotifierWatcher", WATCHER, "RegisterStatusNotifierItem",
            "s", &w);
}

/* ----------------------------------------------------------- switching */
static int spawn_systemctl(const char *verb, const char *unit)
{
    char *argv[] = {"systemctl", (char *)verb, (char *)unit, 0};
    int pid = rt_fork();
    if (pid == 0) {
        unsigned long none = 0;
        rt_sigmask(RT_SIG_SETMASK, &none);
        rt_execve(systemctl_path, argv, rt_envp);
        if (systemctl_path[0] == '/' && rt_streq(systemctl_path, "/usr/bin/systemctl"))
            rt_execve("/bin/systemctl", argv, rt_envp);
        rt_exit(127);
    }
    return pid;
}

static void quit_now(void)
{
    save_level(LV_NORMAL);
    rt_exit(0);
}

/* Normal stops maxeco.service (everything goes back), the others start
 * their level unit, which first starts maxeco.service (saves the settings) */
static void start_level(int target)
{
    static const char *const unit[] = {"maxeco.service", "maxeco-level@eco.service", "maxeco-level@max.service"};
    if (busy) return;
    int pid = spawn_systemctl(target == LV_NORMAL ? "stop" : "start", unit[target]);
    if (pid < 0) {
        notify("MaxEco", "Could not run systemctl.");
        want = level;
        refresh_ui();
        return;
    }
    child_pid = pid, child_target = target, busy = 1;
    refresh_ui();
}

/* the user picked a level; while a switch runs, only the last pick counts */
static void go(int target)
{
    if (want_quit) return;
    want = target;
    if (!busy && want != level) start_level(want);
    else refresh_ui();
}

static void child_done(int ok)
{
    int target = child_target;
    child_pid = 0, busy = 0;
    level = actual_level();
    int reached = ok && level == target;
    if (!reached) {
        if (target == LV_NORMAL)
            notify("MaxEco could not go back to Normal",
                   "systemctl stop maxeco.service failed. Details in journalctl -u maxeco");
        else
            notify(target == LV_ECO ? "MaxEco could not switch to Eco" : "MaxEco could not switch to MaxEco",
                   "systemctl start failed. Details in journalctl -u maxeco -u maxeco-level@*");
        want = level; /* do not keep trying */
    }
    if (want_quit) {
        if (target != LV_NORMAL && level != LV_NORMAL) {
            want = LV_NORMAL;
            start_level(LV_NORMAL);
            return;
        }
        display_restore();
        quit_now();
    }
    if (reached) save_level(level);
    if (want != level) {
        start_level(want); /* the user moved on while this one ran */
        return;
    }
    screens_for_level();
    refresh_ui();
}

static void reap_children(void)
{
    int st, pid;
    while ((pid = rt_wait(-1, &st, RT_WNOHANG)) > 0)
        if (pid == child_pid) child_done((st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0);
}

/* one wheel notch (120) is one step: down goes toward MaxEco, up toward Normal */
static void on_scroll(int delta)
{
    if (!delta || want_quit) return;
    if ((delta > 0) != (scroll_acc > 0)) scroll_acc = 0;
    scroll_acc += delta;
    int t = want;
    while (scroll_acc >= 120) t--, scroll_acc -= 120;
    while (scroll_acc <= -120) t++, scroll_acc += 120;
    if (t < LV_NORMAL) t = LV_NORMAL;
    if (t > LV_MAX) t = LV_MAX;
    if (t != want) go(t);
}

static void on_click(int id)
{
    switch (id) {
    case ID_NORMAL: go(LV_NORMAL); break;
    case ID_ECO: go(LV_ECO); break;
    case ID_MAX: go(LV_MAX); break;
    case ID_HZ:
        hz60 = !hz60;
        save_hz(hz60);
        if (!busy) screens_for_level();
        refresh_ui();
        break;
    case ID_AUTO:
        set_autostart(!autostart_on);
        refresh_ui();
        break;
    case ID_QUIT:
        want_quit = 1;
        save_level(LV_NORMAL);
        if (busy) break;
        if (level != LV_NORMAL || rt_exists(STATE_FILE)) {
            want = LV_NORMAL;
            start_level(LV_NORMAL);
        } else {
            display_restore(), quit_now();
        }
        break;
    }
}

/* Picks up changes made outside the tray, e.g. systemctl stop by hand. */
static int sync_state(void)
{
    if (busy) return 0;
    int now = actual_level(), changed = autostart_enabled() != autostart_on;
    autostart_on = autostart_enabled();
    if (now != level || want != level) {
        level = want = now;
        changed = 1;
    }
    if (changed) refresh_ui();
    return changed;
}

/* --------------------------------------------------------------- menu */
static void dict_s(struct db_w *w, const char *k, const char *v) { dw_pad(w, 8), dw_str(w, k), dw_var_s(w, v); }
static void dict_i(struct db_w *w, const char *k, int v) { dw_pad(w, 8), dw_str(w, k), dw_var_i(w, v); }
static void dict_b(struct db_w *w, const char *k, int v) { dw_pad(w, 8), dw_str(w, k), dw_var_b(w, v); }

/* Writes one property value as a variant; key 0 writes them all as a{sv}. */
static int item_props(struct db_w *w, int id, const char *key)
{
    struct { const char *k; char t; const char *s; int i; } p[6];
    int n = 0;
    switch (id) {
    case ID_ROOT:
        p[n++] = (typeof(p[0])){"children-display", 's', "submenu", 0};
        break;
    case ID_NORMAL: case ID_ECO: case ID_MAX:
        p[n++] = (typeof(p[0])){"label", 's', level_label[id - ID_NORMAL], 0};
        p[n++] = (typeof(p[0])){"toggle-type", 's', "radio", 0};
        p[n++] = (typeof(p[0])){"toggle-state", 'i', 0, want == id - ID_NORMAL};
        p[n++] = (typeof(p[0])){"enabled", 'b', 0, !want_quit};
        break;
    case ID_HZ:
        p[n++] = (typeof(p[0])){"label", 's', "60 Hz Screens in MaxEco", 0};
        p[n++] = (typeof(p[0])){"toggle-type", 's', "checkmark", 0};
        p[n++] = (typeof(p[0])){"toggle-state", 'i', 0, hz60};
        break;
    case ID_SEP1: case ID_SEP2:
        p[n++] = (typeof(p[0])){"type", 's', "separator", 0};
        break;
    case ID_AUTO:
        p[n++] = (typeof(p[0])){"label", 's', "Start With System", 0};
        p[n++] = (typeof(p[0])){"toggle-type", 's', "checkmark", 0};
        p[n++] = (typeof(p[0])){"toggle-state", 'i', 0, autostart_on};
        break;
    case ID_QUIT:
        p[n++] = (typeof(p[0])){"label", 's', "Quit", 0};
        p[n++] = (typeof(p[0])){"enabled", 'b', 0, !(want_quit && busy)};
        break;
    default:
        return 0;
    }
    unsigned a = 0;
    if (!key) a = dw_abegin(w, 8);
    for (int i = 0; i < n; i++) {
        if (key && !rt_streq(key, p[i].k)) continue;
        if (!key) dw_pad(w, 8), dw_str(w, p[i].k);
        if (p[i].t == 's') dw_var_s(w, p[i].s);
        else if (p[i].t == 'i') dw_var_i(w, p[i].i);
        else dw_var_b(w, p[i].i);
        if (key) return 1;
    }
    if (key) return 0;
    dw_aend(w, a, 8);
    return 1;
}

static void put_item(struct db_w *w, int id, int children)
{
    dw_pad(w, 8);
    dw_u32(w, (unsigned)id);
    item_props(w, id, 0);
    unsigned a = dw_abegin(w, 1);
    if (id == ID_ROOT && children)
        for (int i = 1; i <= ID_LAST; i++) {
            dw_sig(w, "(ia{sv}av)");
            put_item(w, i, 0);
        }
    dw_aend(w, a, 1);
}

/* ------------------------------------------------------- introspection */
#define STD_IFACES                                                                           \
    "<interface name=\"org.freedesktop.DBus.Properties\">"                                   \
    "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>" \
    "<arg type=\"v\" direction=\"out\"/></method>"                                           \
    "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/>"                             \
    "<arg type=\"a{sv}\" direction=\"out\"/></method>"                                       \
    "<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>" \
    "<arg type=\"v\" direction=\"in\"/></method>"                                            \
    "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>" \
    "</interface>"                                                                           \
    "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"   \
    "<arg type=\"s\" direction=\"out\"/></method></interface>"                               \
    "<interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/>"                  \
    "<method name=\"GetMachineId\"><arg type=\"s\" direction=\"out\"/></method></interface>"
#define XML_HEAD                                                                             \
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "      \
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>"
#define P(n, t) "<property name=\"" n "\" type=\"" t "\" access=\"read\"/>"
#define XY "<arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/>"

static const char xml_root[] = XML_HEAD STD_IFACES
    "<node name=\"StatusNotifierItem\"/><node name=\"MenuBar\"/></node>";
static const char xml_sni[] = XML_HEAD STD_IFACES
    "<interface name=\"org.kde.StatusNotifierItem\">"
    P("Category", "s") P("Id", "s") P("Title", "s") P("Status", "s") P("WindowId", "i")
    P("IconThemePath", "s") P("Menu", "o") P("ItemIsMenu", "b") P("IconName", "s")
    P("IconPixmap", "a(iiay)") P("OverlayIconName", "s") P("OverlayIconPixmap", "a(iiay)")
    P("AttentionIconName", "s") P("AttentionIconPixmap", "a(iiay)") P("AttentionMovieName", "s")
    P("ToolTip", "(sa(iiay)ss)")
    "<method name=\"ContextMenu\">" XY "</method><method name=\"Activate\">" XY "</method>"
    "<method name=\"SecondaryActivate\">" XY "</method>"
    "<method name=\"Scroll\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>"
    "<method name=\"ProvideXdgActivationToken\"><arg type=\"s\" direction=\"in\"/></method>"
    "<signal name=\"NewTitle\"/><signal name=\"NewIcon\"/><signal name=\"NewAttentionIcon\"/>"
    "<signal name=\"NewOverlayIcon\"/><signal name=\"NewToolTip\"/>"
    "<signal name=\"NewStatus\"><arg type=\"s\"/></signal></interface></node>";
static const char xml_menu[] = XML_HEAD STD_IFACES
    "<interface name=\"com.canonical.dbusmenu\">"
    P("Version", "u") P("TextDirection", "s") P("Status", "s") P("IconThemePath", "as")
    "<method name=\"GetLayout\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/>"
    "<arg type=\"as\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/>"
    "<arg type=\"(ia{sv}av)\" direction=\"out\"/></method>"
    "<method name=\"GetGroupProperties\"><arg type=\"ai\" direction=\"in\"/>"
    "<arg type=\"as\" direction=\"in\"/><arg type=\"a(ia{sv})\" direction=\"out\"/></method>"
    "<method name=\"GetProperty\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"v\" direction=\"out\"/></method>"
    "<method name=\"Event\"><arg type=\"i\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"v\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>"
    "<method name=\"EventGroup\"><arg type=\"a(isvu)\" direction=\"in\"/>"
    "<arg type=\"ai\" direction=\"out\"/></method>"
    "<method name=\"AboutToShow\"><arg type=\"i\" direction=\"in\"/>"
    "<arg type=\"b\" direction=\"out\"/></method>"
    "<method name=\"AboutToShowGroup\"><arg type=\"ai\" direction=\"in\"/>"
    "<arg type=\"ai\" direction=\"out\"/><arg type=\"ai\" direction=\"out\"/></method>"
    "<signal name=\"ItemsPropertiesUpdated\"><arg type=\"a(ia{sv})\"/><arg type=\"a(ia{sv}as)\"/></signal>"
    "<signal name=\"LayoutUpdated\"><arg type=\"u\"/><arg type=\"i\"/></signal>"
    "<signal name=\"ItemActivationRequested\"><arg type=\"i\"/><arg type=\"u\"/></signal>"
    "</interface></node>";

/* ---------------------------------------------------------- properties */
static const char *const sni_props[] = {
    "Category", "Id", "Title", "Status", "WindowId", "IconThemePath", "Menu", "ItemIsMenu",
    "IconName", "IconPixmap", "OverlayIconName", "OverlayIconPixmap", "AttentionIconName",
    "AttentionIconPixmap", "AttentionMovieName", "ToolTip", 0};
static const char *const menu_props[] = {"Version", "TextDirection", "Status", "IconThemePath", 0};

static int sni_prop(struct db_w *w, const char *n)
{
    if (rt_streq(n, "Category")) dw_var_s(w, "Hardware");
    else if (rt_streq(n, "Id")) dw_var_s(w, "maxeco");
    else if (rt_streq(n, "Title")) dw_var_s(w, "MaxEco");
    else if (rt_streq(n, "Status")) dw_var_s(w, "Active");
    else if (rt_streq(n, "WindowId")) dw_var_i(w, 0);
    else if (rt_streq(n, "Menu")) dw_var_o(w, MENU_PATH);
    else if (rt_streq(n, "ItemIsMenu")) dw_var_b(w, 1);
    else if (rt_streq(n, "IconName")) dw_var_s(w, icon_name());
    else if (rt_streq(n, "IconThemePath") || rt_streq(n, "OverlayIconName") ||
             rt_streq(n, "AttentionIconName") || rt_streq(n, "AttentionMovieName"))
        dw_var_s(w, "");
    else if (rt_streq(n, "IconPixmap") || rt_streq(n, "OverlayIconPixmap") ||
             rt_streq(n, "AttentionIconPixmap"))
        dw_sig(w, "a(iiay)"), dw_empty_pixmaps(w);
    else if (rt_streq(n, "ToolTip")) {
        dw_sig(w, "(sa(iiay)ss)");
        dw_pad(w, 8);
        dw_str(w, icon_name());
        dw_empty_pixmaps(w);
        dw_str(w, "MaxEco");
        dw_str(w, tooltip_text());
    } else
        return 0;
    return 1;
}

static int menu_prop(struct db_w *w, const char *n)
{
    if (rt_streq(n, "Version")) dw_sig(w, "u"), dw_u32(w, 3);
    else if (rt_streq(n, "TextDirection")) dw_var_s(w, "ltr");
    else if (rt_streq(n, "Status")) dw_var_s(w, "normal");
    else if (rt_streq(n, "IconThemePath")) {
        dw_sig(w, "as");
        unsigned a = dw_abegin(w, 4);
        dw_aend(w, a, 4);
    } else
        return 0;
    return 1;
}

/* ---------------------------------------------------------- dispatcher */
static void reply(struct db_msg *m, const char *sig, const struct db_w *w)
{
    if (m->flags & DB_NO_REPLY) return;
    db_send(&sess, DB_RETURN, 0, m->sender, 0, 0, 0, m->serial, 0, sig, w);
}

static void reply_error(struct db_msg *m, const char *name, const char *text)
{
    struct db_w w;
    if (m->flags & DB_NO_REPLY) return;
    dw_init(&w, rep_buf, sizeof rep_buf);
    dw_str(&w, text);
    db_send(&sess, DB_ERROR, 0, m->sender, 0, 0, 0, m->serial, name, "s", &w);
}

static void handle_properties(struct db_msg *m, int sni, struct db_r *r, struct db_w *w)
{
    const char *want_iface = sni ? "org.kde.StatusNotifierItem" : "com.canonical.dbusmenu";
    int (*one)(struct db_w *, const char *) = sni ? sni_prop : menu_prop;
    const char *const *all = sni ? sni_props : menu_props;
    if (rt_streq(m->member, "Get") && rt_streq(m->sig, "ss")) {
        const char *iface = dr_str(r), *name = dr_str(r);
        if (*iface && !rt_streq(iface, want_iface)) {
            reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "Unknown interface");
        } else if (one(w, name)) {
            reply(m, "v", w);
        } else {
            reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", "Unknown property");
        }
    } else if (rt_streq(m->member, "GetAll") && rt_streq(m->sig, "s")) {
        const char *iface = dr_str(r);
        unsigned a = dw_abegin(w, 8);
        if (!*iface || rt_streq(iface, want_iface))
            for (int i = 0; all[i]; i++) {
                dw_pad(w, 8);
                dw_str(w, all[i]);
                one(w, all[i]);
            }
        dw_aend(w, a, 8);
        reply(m, "a{sv}", w);
    } else if (rt_streq(m->member, "Set")) {
        reply_error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", "Properties are read only");
    } else {
        reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
    }
}

static void handle_menu(struct db_msg *m, struct db_r *r, struct db_w *w)
{
    const char *mb = m->member, *sig = m->sig;
    if (rt_streq(mb, "GetLayout") && rt_streq(sig, "iias")) {
        int parent = (int)dr_u32(r), depth = (int)dr_u32(r);
        if (parent < 0 || parent > ID_LAST) {
            reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "Unknown menu item");
            return;
        }
        dw_u32(w, menu_rev);
        put_item(w, parent, depth != 0);
        reply(m, "u(ia{sv}av)", w);
    } else if (rt_streq(mb, "GetGroupProperties") && rt_streq(sig, "aias")) {
        unsigned n = dr_u32(r), end = r->pos + n;
        unsigned a = dw_abegin(w, 8);
        if (!r->err && n <= r->len - r->pos) {
            if (!n)
                for (int i = 0; i <= ID_LAST; i++) dw_pad(w, 8), dw_u32(w, (unsigned)i), item_props(w, i, 0);
            while (!r->err && r->pos < end) {
                int id = (int)dr_u32(r);
                if (!r->err && id >= 0 && id <= ID_LAST) dw_pad(w, 8), dw_u32(w, (unsigned)id), item_props(w, id, 0);
            }
        }
        dw_aend(w, a, 8);
        reply(m, "a(ia{sv})", w);
    } else if (rt_streq(mb, "GetProperty") && rt_streq(sig, "is")) {
        int id = (int)dr_u32(r);
        const char *name = dr_str(r);
        if (!r->err && item_props(w, id, name)) reply(m, "v", w);
        else reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "Unknown property");
    } else if (rt_streq(mb, "Event") && rt_streq(sig, "isvu")) {
        int id = (int)dr_u32(r);
        int clicked = rt_streq(dr_str(r), "clicked") && !r->err;
        reply(m, "", 0);
        if (clicked) on_click(id);
    } else if (rt_streq(mb, "EventGroup") && rt_streq(sig, "a(isvu)")) {
        int clicks[8], nc = 0;
        unsigned n = dr_u32(r);
        if (!r->err && !dr_pad(r, 8) && n <= r->len - r->pos) {
            unsigned end = r->pos + n;
            while (!r->err && r->pos < end) {
                dr_pad(r, 8);
                int id = (int)dr_u32(r);
                int clicked = rt_streq(dr_str(r), "clicked");
                dr_skip(r, "v", 0);
                dr_u32(r);
                if (!r->err && clicked && nc < 8) clicks[nc++] = id;
            }
        }
        unsigned a = dw_abegin(w, 4);
        dw_aend(w, a, 4);
        reply(m, "ai", w);
        for (int i = 0; i < nc; i++) on_click(clicks[i]);
    } else if (rt_streq(mb, "AboutToShow") && rt_streq(sig, "i")) {
        dw_u32(w, (unsigned)sync_state());
        reply(m, "b", w);
    } else if (rt_streq(mb, "AboutToShowGroup") && rt_streq(sig, "ai")) {
        sync_state();
        unsigned a = dw_abegin(w, 4);
        dw_aend(w, a, 4);
        a = dw_abegin(w, 4);
        dw_aend(w, a, 4);
        reply(m, "aiai", w);
    } else {
        reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
    }
}

static void handle_call(struct db_msg *m)
{
    struct db_w w;
    struct db_r r = {m->body, m->body_len, 0, 0};
    const char *ifc = m->iface ? m->iface : "", *mb = m->member;
    int sni = rt_streq(m->path, SNI_PATH), menu = rt_streq(m->path, MENU_PATH);
    dw_init(&w, rep_buf, sizeof rep_buf);
    if (debug) {
        const char *parts[] = {"maxeco: call ", ifc, " ", mb, " from ", m->sender ? m->sender : "?", "\n"};
        for (int i = 0; i < 7; i++) rt_write_all(2, parts[i], rt_strlen(parts[i]));
    }

    if (rt_streq(ifc, "org.freedesktop.DBus.Peer") || (!*ifc && (rt_streq(mb, "Ping") || rt_streq(mb, "GetMachineId")))) {
        if (rt_streq(mb, "Ping")) {
            reply(m, "", 0);
        } else if (rt_streq(mb, "GetMachineId")) {
            char id[64];
            int n = read_small("/etc/machine-id", id, sizeof id);
            while (n > 0 && (id[n - 1] == '\n' || id[n - 1] == ' ')) id[--n] = 0;
            dw_str(&w, n > 0 ? id : "");
            reply(m, "s", &w);
        } else {
            reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
        }
    } else if (rt_streq(mb, "Introspect") && (!*ifc || rt_streq(ifc, "org.freedesktop.DBus.Introspectable"))) {
        dw_str(&w, sni ? xml_sni : menu ? xml_menu : rt_streq(m->path, "/") ? xml_root
                                                    : XML_HEAD "</node>");
        reply(m, "s", &w);
    } else if (!sni && !menu) {
        reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", "No such object");
    } else if (rt_streq(ifc, "org.freedesktop.DBus.Properties") ||
               (!*ifc && (rt_streq(mb, "Get") || rt_streq(mb, "GetAll") || rt_streq(mb, "Set")))) {
        handle_properties(m, sni, &r, &w);
    } else if (sni && (!*ifc || rt_streq(ifc, "org.kde.StatusNotifierItem"))) {
        if (rt_streq(mb, "Scroll") && rt_streq(m->sig, "is")) {
            int delta = (int)dr_u32(&r);
            const char *dir = dr_str(&r);
            reply(m, "", 0);
            if (!r.err && (dir[0] == 'v' || dir[0] == 'V')) on_scroll(delta);
        } else if (rt_streq(mb, "Activate") || rt_streq(mb, "SecondaryActivate") || rt_streq(mb, "ContextMenu") ||
                   rt_streq(mb, "Scroll") || rt_streq(mb, "ProvideXdgActivationToken"))
            reply(m, "", 0);
        else
            reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
    } else if (menu && (!*ifc || rt_streq(ifc, "com.canonical.dbusmenu"))) {
        handle_menu(m, &r, &w);
    } else {
        reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "Unknown interface");
    }
}

static void on_session_msg(struct db_conn *c, struct db_msg *m)
{
    (void)c;
    if (m->type == DB_CALL) {
        handle_call(m);
    } else if (m->type == DB_SIGNAL && rt_streq(m->member, "NameOwnerChanged") &&
               rt_streq(m->sender, "org.freedesktop.DBus") && rt_streq(m->sig, "sss")) {
        struct db_r r = {m->body, m->body_len, 0, 0};
        const char *name = dr_str(&r);
        dr_str(&r);
        const char *owner = dr_str(&r);
        if (!r.err && rt_streq(name, WATCHER) && *owner) register_sni();
    }
}

/* ---------------------------------------------------------------- main */
static void init_paths(void)
{
    const char *cfg = rt_getenv("XDG_CONFIG_HOME"), *home = rt_getenv("HOME");
    char base[400];
    if (cfg && *cfg == '/') path_join(base, sizeof base, cfg, "");
    else if (home && *home == '/') path_join(base, sizeof base, home, "/.config");
    else base[0] = 0;
    if (*base) {
        path_join(p_cfg_maxeco, sizeof p_cfg_maxeco, base, "/maxeco");
        path_join(p_state, sizeof p_state, base, "/maxeco/state");
        path_join(p_cfg_auto, sizeof p_cfg_auto, base, "/autostart");
        path_join(p_auto, sizeof p_auto, base, "/autostart/maxeco.desktop");
        path_join(p_display, sizeof p_display, base, "/maxeco/display");
        path_join(p_hz, sizeof p_hz, base, "/maxeco/screens-60hz");
        path_join(p_auto_display, sizeof p_auto_display, base, "/autostart/maxeco-restore-display.desktop");
    }
    rt_ssize n = rt_readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
    self_exe[n > 0 ? n : 0] = 0;
    if (n <= 0) path_join(self_exe, sizeof self_exe, "maxeco", "");
}

static int single_instance(void)
{
    char path[400];
    const char *rd = rt_getenv("XDG_RUNTIME_DIR");
    if (rd && *rd == '/') {
        path_join(path, sizeof path, rd, "/maxeco.lock");
    } else {
        struct rt_sb s;
        sb_init(&s, path, sizeof path);
        sb_add(&s, "/tmp/maxeco-"), sb_num(&s, rt_getuid()), sb_add(&s, ".lock");
    }
    int fd = rt_open(path, RT_O_WRONLY | RT_O_CREAT | RT_O_CLOEXEC, 0600);
    return fd >= 0 && rt_flock(fd, RT_LOCK_EX | RT_LOCK_NB) == 0;
}

int main(int argc, char **argv)
{
    init_paths();
    /* test hooks: tests/t_tray.py points these at fakes */
    if (rt_getenv("MAXECO_STATE")) STATE_FILE = rt_getenv("MAXECO_STATE");
    if (rt_getenv("MAXECO_LEVEL")) LEVEL_FILE = rt_getenv("MAXECO_LEVEL");
    if (rt_getenv("MAXECO_SYSTEMCTL")) systemctl_path = rt_getenv("MAXECO_SYSTEMCTL");
    if (rt_getenv("MAXECO_KSCREEN")) kscreen_path = rt_getenv("MAXECO_KSCREEN");
    debug = rt_getenv("MAXECO_DEBUG") != 0;
    if (argc > 1) {
        const char *a = argv[1];
        if (rt_streq(a, "--version")) {
            rt_write_all(1, "maxeco " MAXECO_VERSION "\n", sizeof("maxeco " MAXECO_VERSION "\n") - 1);
            return 0;
        }
        if (rt_streq(a, "--restore-display")) {
            /* login after the PC was turned off in MaxEco with 60 Hz screens;
             * when the tray starts with the session it does this itself */
            autostart_on = autostart_enabled();
            if (autostart_on || rt_exists(STATE_FILE)) return 0;
            return display_restore() == 0 ? 0 : 1;
        }
        static const char use[] = "usage: maxeco [--version | --restore-display]\n"
                                  "Tray switch for the power level. The menu has Normal, Eco, MaxEco,\n"
                                  "60 Hz Screens in MaxEco, Start With System and Quit.\n";
        rt_write_all(rt_streq(a, "--help") ? 1 : 2, use, sizeof use - 1);
        return rt_streq(a, "--help") ? 0 : 2;
    }
    if (!single_instance()) {
        rt_log("already running", 0);
        return 0;
    }
    unsigned long mask = 1UL << (RT_SIGHUP - 1) | 1UL << (RT_SIGINT - 1) | 1UL << (RT_SIGTERM - 1) |
                         1UL << (RT_SIGCHLD - 1) | 1UL << (RT_SIGPIPE - 1);
    rt_sigmask(RT_SIG_BLOCK, &mask);
    int sfd = rt_signalfd(&mask);
    if (sfd < 0) {
        rt_log("signalfd failed", 0);
        return 1;
    }

    const char *addr = rt_getenv("DBUS_SESSION_BUS_ADDRESS");
    char fallback[420];
    if (!addr) {
        const char *rd = rt_getenv("XDG_RUNTIME_DIR");
        struct rt_sb a;
        sb_init(&a, fallback, sizeof fallback);
        sb_add(&a, "unix:path="), sb_add(&a, rd ? rd : ""), sb_add(&a, "/bus");
        addr = fallback;
    }
    if (db_open(&sess, addr) < 0) {
        rt_log("cannot connect to the session bus", 0);
        return 1;
    }

    struct rt_sb s;
    sb_init(&s, sni_name, sizeof sni_name);
    sb_add(&s, "org.kde.StatusNotifierItem-"), sb_num(&s, (unsigned long)rt_getpid()), sb_add(&s, "-1");
    struct db_w w;
    dw_init(&w, sig_buf, sizeof sig_buf);
    dw_str(&w, sni_name);
    dw_u32(&w, 4); /* DBUS_NAME_FLAG_DO_NOT_QUEUE */
    db_call(&sess, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "RequestName", "su", &w);
    dw_init(&w, sig_buf, sizeof sig_buf);
    dw_str(&w, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
               "member='NameOwnerChanged',path='/org/freedesktop/DBus',arg0='" WATCHER "'");
    db_call(&sess, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "AddMatch", "s", &w);
    register_sni();

    autostart_on = autostart_enabled();
    hz60 = *p_hz && rt_exists(p_hz);
    level = want = actual_level();
    int saved = saved_level();
    if (level != LV_NORMAL) screens_for_level(); /* new login while a level stayed on */
    else if (saved != LV_NORMAL) go(saved);      /* the level chosen before the reboot */
    else display_restore();                      /* the PC was turned off in MaxEco at 60 Hz */

    for (;;) {
        struct rt_pollfd f[2] = {{sess.fd, RT_POLLIN, 0}, {sfd, RT_POLLIN, 0}};
        int n = rt_poll(f, 2, 0);
        if (n == -RT_EINTR) continue;
        if (n < 0) return 1;
        if (f[1].revents) {
            struct rt_sigfd_info si;
            while (rt_read(sfd, &si, sizeof si) == (rt_ssize)sizeof si) {
                if (si.signo == RT_SIGCHLD) reap_children();
                else if (si.signo != RT_SIGPIPE)
                    return 0; /* session ends: keep the saved level */
            }
        }
        if (f[0].revents && db_read(&sess, on_session_msg) < 0) return 0;
    }
}

/* maxeco-helper: applies and undoes the MaxEco levels. Runs as root from
 * maxeco.service, maxeco-level@.service and maxeco-resume.service.
 *
 *   maxeco-helper save         save the current settings once (Normal)
 *   maxeco-helper level eco    apply Eco (saves first when nothing is saved)
 *   maxeco-helper level max    apply MaxEco
 *   maxeco-helper off          put back exactly what was saved
 *   maxeco-helper resume       apply the current level again after a suspend
 *   maxeco-helper status       print the level and the saved settings
 *   maxeco-helper on           same as "level max" (MaxEco 1.1 and older)
 *
 *                  Eco                            MaxEco
 *   CPU            powersave, EPP balance_power,  powersave, EPP power,
 *                  max frequency 60 %             max frequency = lowest
 *   AMD GPU        power cap 60 % of default;     performance level low
 *                  "low" on cards with no screen
 *   NVIDIA GPU     power limit 60 % of default,   power limit at its minimum,
 *                  clocks up to 60 % (graphics)   clocks locked at their
 *                  and 75 % (memory)              lowest values
 *   board fans     the motherboard fans (nct67xx) follow a quiet curve kept in
 *                  the chip, in both levels
 *   NVIDIA fans    left as they were in both levels. A fixed low speed is not
 *                  safe (on an RTX 3060 Ti 30 %, the lowest NVML reports, does
 *                  not spin the fans at all) and the card's automatic curve can
 *                  be louder than what the user had.
 *
 * Eco never raises a limit: when a setting already was below the Eco value,
 * it stays where it was. Everything that is changed is first written to
 * /run/maxeco/state, so "off" restores the previous values and a reboot
 * always starts clean. The current level is in /run/maxeco/level.
 */
/* _DEFAULT_SOURCE and not _GNU_SOURCE: with _GNU_SOURCE, glibc 2.38 headers
 * turn strtol and sscanf into the C23 versions, and the binary would then
 * refuse to start on a distro older than glibc 2.38. */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* a GNU extension, declared here because _GNU_SOURCE is off (see above) */
int versionsort(const struct dirent **, const struct dirent **);

#define MAX_ITEMS 1024
#define MAX_GPUS 8

static const char *sysroot = "";        /* test hook, ignored for root */
static const char *rundir = "/run/maxeco";
static int no_nvml;
static int failures;

enum { L_ECO, L_MAX, NLEVELS };
static const char *const level_names[NLEVELS] = {"eco", "max"};
/* Eco limits, in percent of what the hardware can do */
#define ECO_CPU_PCT 60      /* of the highest CPU frequency */
#define ECO_GPU_PCT 60      /* of the highest NVIDIA graphics clock */
#define ECO_MEM_PCT 75      /* of the highest NVIDIA memory clock */
#define ECO_POWER_PCT 60    /* of the default power limit (NVIDIA, AMD) */

struct item { char path[256]; char value[128]; };
static struct item items[MAX_ITEMS];
static int nitems;
static unsigned nv_saved_mw[MAX_GPUS];
static int nv_saved[MAX_GPUS];

/* ------------------------------------------------------------ sysfs io */
static int read_value(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
    return 0;
}

static int write_value(const char *path, const char *value)
{
    char buf[160];
    int n = snprintf(buf, sizeof buf, "%s\n", value);
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t w = write(fd, buf, (size_t)n);
    int e = errno;
    close(fd);
    errno = e;
    return w == n ? 0 : -1;
}

static int has_word(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; (p = strstr(p, word)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ' || p[n] == '\n')) return 1;
    return 0;
}

/* path = sysroot + rel (sysroot is empty outside of tests) */
static int sys_path(char *out, size_t cap, const char *rel)
{
    int n = snprintf(out, cap, "%s%s", sysroot, rel);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

/* path = a/b/c; returns -1 when it does not fit */
static int join3(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    int n = snprintf(out, cap, "%s/%s/%s", a, b, c);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

/* ---------------------------------------------------- what a level means */
/* One setting: the value for each level ("" = the saved original). Fan
 * channels are written as a group (grp > 0); "steady" marks the entries that
 * stay in place once the curve runs, used to skip a group already set. */
struct change { char path[256]; char val[NLEVELS][128]; int grp, steady; };
static struct change plan[MAX_ITEMS];
static int nplan;

static int plan_add(const char *path, const char *eco, const char *max)
{
    char cur[128];
    if (nplan >= MAX_ITEMS || strlen(path) >= sizeof plan[0].path || strlen(eco) >= sizeof plan[0].val[0] ||
        strlen(max) >= sizeof plan[0].val[0] || access(path, W_OK) != 0 || read_value(path, cur, sizeof cur) < 0)
        return -1;
    struct change *c = &plan[nplan++];
    snprintf(c->path, sizeof c->path, "%s", path);
    snprintf(c->val[L_ECO], sizeof c->val[0], "%s", eco);
    snprintf(c->val[L_MAX], sizeof c->val[0], "%s", max);
    c->grp = c->steady = 0;
    return 0;
}

/* the value before MaxEco changed anything: the saved one once saved,
 * otherwise the current one */
static const char *saved_value(const char *path)
{
    for (int i = 0; i < nitems; i++)
        if (strcmp(items[i].path, path) == 0) return items[i].value;
    return NULL;
}

static int base_value(const char *path, char *out, size_t cap)
{
    const char *v = saved_value(path);
    if (v) {
        snprintf(out, cap, "%s", v);
        return 0;
    }
    return read_value(path, out, cap);
}

static long read_long(const char *path)
{
    char v[64], *end;
    if (read_value(path, v, sizeof v) < 0) return -1;
    long n = strtol(v, &end, 10);
    return end == v || n < 0 ? -1 : n;
}

static int policy_filter(const struct dirent *d)
{
    return strncmp(d->d_name, "policy", 6) == 0;
}

static int card_filter(const struct dirent *d)
{
    const char *p = d->d_name;
    if (strncmp(p, "card", 4) != 0 || !p[4]) return 0;
    for (p += 4; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static void plan_fans(void);

/* true when one of the card's connectors has a screen plugged in */
static int card_has_screen(const char *drm, const char *card)
{
    char path[400], val[32];
    size_t n = strlen(card);
    DIR *d = opendir(drm);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d)))
        if (strncmp(e->d_name, card, n) == 0 && e->d_name[n] == '-' &&
            join3(path, sizeof path, drm, e->d_name, "status") == 0 && read_value(path, val, sizeof val) == 0 &&
            strcmp(val, "connected") == 0)
            found = 1;
    closedir(d);
    return found;
}

/* the hwmon directory of an AMD card that has a power cap, or "" */
static void amd_cap_dir(const char *dev, char *out, size_t cap)
{
    char base[400], path[400];
    struct dirent *e;
    DIR *d;
    out[0] = 0;
    if (snprintf(base, sizeof base, "%s/hwmon", dev) >= (int)sizeof base || !(d = opendir(base))) return;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5) != 0 || join3(path, sizeof path, base, e->d_name, "power1_cap") != 0 ||
            access(path, W_OK) != 0)
            continue;
        if (snprintf(out, cap, "%s/%s", base, e->d_name) >= (int)cap) out[0] = 0;
        break;
    }
    closedir(d);
}

static void plan_amd(const char *drm, const char *card)
{
    char dev[400], path[460], hw[460], val[64];
    if (snprintf(dev, sizeof dev, "%s/%s/device", drm, card) >= (int)sizeof dev ||
        snprintf(path, sizeof path, "%s/vendor", dev) >= (int)sizeof path ||
        read_value(path, val, sizeof val) < 0 || strcmp(val, "0x1002") != 0)
        return;
    amd_cap_dir(dev, hw, sizeof hw);
    /* Eco keeps the card free to clock up, but a card with no screen and no
     * power cap (an unused integrated GPU) can stay at its lowest level */
    int idle = !hw[0] && !card_has_screen(drm, card);
    if (snprintf(path, sizeof path, "%s/power_dpm_force_performance_level", dev) < (int)sizeof path)
        plan_add(path, idle ? "low" : "", "low");
    if (!hw[0]) return;
    /* power cap in microwatts: 60 % of the default, never above the current */
    long def, lo, hi, cur, want;
    if (snprintf(path, sizeof path, "%s/power1_cap_default", hw) >= (int)sizeof path || (def = read_long(path)) <= 0)
        return;
    lo = snprintf(path, sizeof path, "%s/power1_cap_min", hw) < (int)sizeof path ? read_long(path) : -1;
    hi = snprintf(path, sizeof path, "%s/power1_cap_max", hw) < (int)sizeof path ? read_long(path) : -1;
    if (snprintf(path, sizeof path, "%s/power1_cap", hw) >= (int)sizeof path || base_value(path, val, sizeof val) < 0)
        return;
    cur = strtol(val, NULL, 10);
    want = def / 100 * ECO_POWER_PCT;
    if (lo > 0 && want < lo) want = lo;
    if (hi > 0 && want > hi) want = hi;
    if (cur > 0 && want > cur) want = cur;
    snprintf(val, sizeof val, "%ld", want);
    plan_add(path, val, val);
}

/* Builds the list in the order it must be applied (and restored):
 * governor before EPP, then the frequency limit. */
static void build_plan(void)
{
    char base[256], path[300], val[256];
    struct dirent **pol = NULL;
    nplan = 0;
    if (sys_path(base, sizeof base, "/sys/devices/system/cpu/cpufreq") < 0) return;
    int np = scandir(base, &pol, policy_filter, versionsort);
    if (np < 0) np = 0;

    for (int i = 0; i < np; i++) {
        if (join3(path, sizeof path, base, pol[i]->d_name, "scaling_available_governors") == 0 &&
            read_value(path, val, sizeof val) == 0 && has_word(val, "powersave") &&
            join3(path, sizeof path, base, pol[i]->d_name, "scaling_governor") == 0)
            plan_add(path, "powersave", "powersave");
    }
    for (int i = 0; i < np; i++) {
        /* with the performance governor the kernel lists only "performance"
         * as available, so the others can not be checked before the switch */
        if (join3(path, sizeof path, base, pol[i]->d_name, "energy_performance_preference") == 0)
            plan_add(path, "balance_power", "power");
    }
    /* Turbo/boost is left alone: the frequency limit below already keeps the
     * CPU under its turbo clocks, and power-profiles-daemon writes the
     * per-core boost files itself; a global boost=0 makes those writes fail. */
    for (int i = 0; i < np; i++) {
        long lo = -1, hi = -1, was = -1;
        char eco[32], max[32];
        if (join3(path, sizeof path, base, pol[i]->d_name, "cpuinfo_min_freq") == 0) lo = read_long(path);
        if (join3(path, sizeof path, base, pol[i]->d_name, "cpuinfo_max_freq") == 0) hi = read_long(path);
        if (lo <= 0 || join3(path, sizeof path, base, pol[i]->d_name, "scaling_max_freq") != 0) continue;
        if (base_value(path, val, sizeof val) == 0) was = strtol(val, NULL, 10);
        long e = hi > 0 ? hi * ECO_CPU_PCT / 100 : -1;
        if (e >= 0 && e < lo) e = lo;
        if (e >= 0 && was > 0 && was < e) e = was;
        snprintf(eco, sizeof eco, "%ld", e);
        snprintf(max, sizeof max, "%ld", lo);
        plan_add(path, e >= 0 ? eco : "", max);
    }
    for (int i = 0; i < np; i++) free(pol[i]);
    free(pol);

    struct dirent **cards = NULL;
    if (sys_path(base, sizeof base, "/sys/class/drm") < 0) return;
    int nc = scandir(base, &cards, card_filter, versionsort);
    for (int i = 0; i < nc; i++) {
        plan_amd(base, cards[i]->d_name);
        free(cards[i]);
    }
    free(cards);
    plan_fans();
}

/* ------------------------------------------------------ motherboard fans */
/* Nuvoton nct67xx chips (most ASUS, MSI, ASRock and many Gigabyte boards).
 * Every fan that is spinning gets a quiet curve written into the chip itself
 * (Smart Fan IV mode) that follows the processor temperature. The chip runs
 * the curve with no program running, so a fan is never left slow while
 * something heats up: from 70 C it speeds up, from 80 C it is at 100 %. */
static const struct { int temp_c, pct; } eco_curve[5] = {{25, 35}, {55, 35}, {70, 60}, {80, 100}, {90, 100}};
#define MAX_FANS 16
static char fan_pwm[MAX_FANS][256];
static int fan_skipped[MAX_FANS];
static int nfans;

static int hwmon_filter(const struct dirent *d) { return strncmp(d->d_name, "hwmon", 5) == 0; }

/* the chip's temperature input that reads the processor, 0 if none */
static int cpu_temp_input(const char *hw)
{
    char path[300], val[64];
    for (int t = 1; t <= 32; t++) {
        if (snprintf(path, sizeof path, "%s/temp%d_label", hw, t) >= (int)sizeof path) return 0;
        /* AMD shows the processor as SMBUSMASTER 0 (TSI), Intel as PECI Agent 0 */
        if (read_value(path, val, sizeof val) == 0 &&
            (strcmp(val, "SMBUSMASTER 0") == 0 || strcmp(val, "PECI Agent 0") == 0))
            return t;
    }
    return 0;
}

static void plan_fans(void)
{
    char base[256], hw[300], path[320], val[64];
    struct dirent **list = NULL;
    nfans = 0;
    if (sys_path(base, sizeof base, "/sys/class/hwmon") < 0) return;
    int n = scandir(base, &list, hwmon_filter, versionsort);
    for (int i = 0; i < n; i++) {
        int bad = snprintf(hw, sizeof hw, "%s/%s", base, list[i]->d_name) >= (int)sizeof hw;
        free(list[i]);
        bad |= snprintf(path, sizeof path, "%s/name", hw) >= (int)sizeof path;
        if (bad || read_value(path, val, sizeof val) < 0 || strncmp(val, "nct67", 5) != 0) continue;
        int src = cpu_temp_input(hw);
        if (!src) {
            printf("fans on %s: no processor temperature input, left alone\n", val);
            continue;
        }
        for (int c = 1; c <= 7 && nfans < MAX_FANS; c++) {
            /* every file of the channel must be there, or the channel is skipped */
            static const char *const parts[] = {"_enable", "", "_temp_sel", "_auto_point1_temp", "_auto_point1_pwm",
                                                "_auto_point2_temp", "_auto_point2_pwm", "_auto_point3_temp",
                                                "_auto_point3_pwm", "_auto_point4_temp", "_auto_point4_pwm",
                                                "_auto_point5_temp", "_auto_point5_pwm"};
            char pwm[280], files[13][300], eco[13][16];
            int ok = 1;
            if (snprintf(pwm, sizeof pwm, "%s/pwm%d", hw, c) >= (int)sizeof pwm ||
                snprintf(path, sizeof path, "%s/fan%d_input", hw, c) >= (int)sizeof path ||
                read_value(path, val, sizeof val) < 0 || atoi(val) <= 0)
                continue; /* no fan spinning here */
            for (int k = 0; k < 13; k++) {
                char cur[128];
                if (snprintf(files[k], sizeof files[k], "%s%s", pwm, parts[k]) >= (int)sizeof files[k] ||
                    access(files[k], W_OK) != 0 || read_value(files[k], cur, sizeof cur) < 0)
                    ok = 0;
            }
            if (!ok || strlen(pwm) >= sizeof fan_pwm[0]) continue;
            snprintf(eco[0], sizeof eco[0], "1");   /* manual first, so the fan is never without a command */
            snprintf(eco[1], sizeof eco[1], "255"); /* at 100 % while the curve is written */
            snprintf(eco[2], sizeof eco[2], "%d", src);
            for (int k = 0; k < 5; k++) {
                snprintf(eco[3 + 2 * k], sizeof eco[0], "%d", eco_curve[k].temp_c * 1000);
                snprintf(eco[4 + 2 * k], sizeof eco[0], "%d", (eco_curve[k].pct * 255 + 50) / 100);
            }
            /* the same quiet curve in both levels */
            int first = nplan;
            for (int k = 0; k < 13; k++) plan_add(files[k], eco[k], eco[k]);
            plan_add(files[0], "5", "5"); /* 5 = Smart Fan IV, the curve runs inside the chip */
            if (nplan != first + 14) { /* a file went away: do not write half a channel */
                nplan = first;
                continue;
            }
            for (int k = first; k < nplan; k++) {
                plan[k].grp = nfans + 1;
                plan[k].steady = k - first >= 2; /* not the manual 100 % used while writing */
            }
            fan_skipped[nfans] = 0;
            snprintf(fan_pwm[nfans++], sizeof fan_pwm[0], "%s", pwm);
        }
    }
    free(list);
}

/* A chip that did not take the curve gets its fan back at 100 %. */
static void check_fans(void)
{
    for (int i = 0; i < nfans; i++) {
        char path[300], val[16];
        const char *name = strrchr(fan_pwm[i], '/') + 1;
        if (fan_skipped[i]) continue;
        if (snprintf(path, sizeof path, "%s_enable", fan_pwm[i]) >= (int)sizeof path) continue;
        if (read_value(path, val, sizeof val) == 0 && strcmp(val, "5") == 0) {
            printf("fan %s: quiet curve on the processor temperature\n", name);
            continue;
        }
        write_value(path, "1");
        write_value(fan_pwm[i], "255");
        printf("fan %s: the chip did not take the curve, left at 100%%\n", name);
        failures++;
    }
}

/* -------------------------------------------------------------- NVIDIA */
typedef int nvret;
typedef void *nvdev;
static struct {
    void *lib;
    nvret (*init)(void);
    nvret (*shutdown)(void);
    const char *(*error)(nvret);
    nvret (*count)(unsigned *);
    nvret (*handle)(unsigned, nvdev *);
    nvret (*pl_get)(nvdev, unsigned *);
    nvret (*pl_range)(nvdev, unsigned *, unsigned *);
    nvret (*pl_set)(nvdev, unsigned);
    nvret (*pl_default)(nvdev, unsigned *); /* optional */
    nvret (*mem_clocks)(nvdev, unsigned *, unsigned *);
    nvret (*gr_clocks)(nvdev, unsigned, unsigned *, unsigned *);
    nvret (*lock_gr)(nvdev, unsigned, unsigned);
    nvret (*reset_gr)(nvdev);
    nvret (*lock_mem)(nvdev, unsigned, unsigned);
    nvret (*reset_mem)(nvdev);
    /* fans: optional, older drivers do not have them */
    nvret (*fan_count)(nvdev, unsigned *);
    nvret (*fan_policy)(nvdev, unsigned, unsigned *);
    nvret (*fan_target)(nvdev, unsigned, unsigned *);
    nvret (*fan_auto)(nvdev, unsigned);
    nvret (*fan_set)(nvdev, unsigned, unsigned);
} nv;
#define NV_FAN_MANUAL 1 /* NVML_FAN_POLICY_MANUAL */
#define MAX_NVFANS 32
static struct { unsigned gpu, fan, policy, speed; } nvfan[MAX_NVFANS];
static int nnvfans;

static int nv_open(void)
{
    if (no_nvml) return 0;
    nv.lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!nv.lib) return 0;
#define SYM(field, name) if (!(*(void **)&nv.field = dlsym(nv.lib, name))) goto fail
    SYM(init, "nvmlInit_v2");
    SYM(shutdown, "nvmlShutdown");
    SYM(error, "nvmlErrorString");
    SYM(count, "nvmlDeviceGetCount_v2");
    SYM(handle, "nvmlDeviceGetHandleByIndex_v2");
    SYM(pl_get, "nvmlDeviceGetPowerManagementLimit");
    SYM(pl_range, "nvmlDeviceGetPowerManagementLimitConstraints");
    SYM(pl_set, "nvmlDeviceSetPowerManagementLimit");
    SYM(mem_clocks, "nvmlDeviceGetSupportedMemoryClocks");
    SYM(gr_clocks, "nvmlDeviceGetSupportedGraphicsClocks");
    SYM(lock_gr, "nvmlDeviceSetGpuLockedClocks");
    SYM(reset_gr, "nvmlDeviceResetGpuLockedClocks");
    SYM(lock_mem, "nvmlDeviceSetMemoryLockedClocks");
    SYM(reset_mem, "nvmlDeviceResetMemoryLockedClocks");
#undef SYM
    *(void **)&nv.pl_default = dlsym(nv.lib, "nvmlDeviceGetPowerManagementDefaultLimit");
    *(void **)&nv.fan_count = dlsym(nv.lib, "nvmlDeviceGetNumFans");
    *(void **)&nv.fan_policy = dlsym(nv.lib, "nvmlDeviceGetFanControlPolicy_v2");
    *(void **)&nv.fan_target = dlsym(nv.lib, "nvmlDeviceGetTargetFanSpeed");
    *(void **)&nv.fan_auto = dlsym(nv.lib, "nvmlDeviceSetDefaultFanSpeed_v2");
    *(void **)&nv.fan_set = dlsym(nv.lib, "nvmlDeviceSetFanSpeed_v2");
    if (!nv.fan_count || !nv.fan_policy || !nv.fan_target || !nv.fan_auto || !nv.fan_set) nv.fan_count = NULL;
    if (nv.init() != 0) goto fail;
    return 1;
fail:
    dlclose(nv.lib);
    nv.lib = NULL;
    return 0;
}

static void nv_close(void)
{
    if (!nv.lib) return;
    nv.shutdown();
    dlclose(nv.lib);
    nv.lib = NULL;
}

static unsigned nv_count(void)
{
    unsigned n = 0;
    if (!nv.lib || nv.count(&n) != 0) return 0;
    return n > MAX_GPUS ? MAX_GPUS : n;
}

static void nv_check(const char *what, unsigned i, nvret r)
{
    if (r == 0) return;
    printf("nvidia gpu%u: %s failed: %s\n", i, what, nv.error(r));
    failures++;
}

static unsigned min_of(const unsigned *v, unsigned n)
{
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++)
        if (v[i] && (!m || v[i] < m)) m = v[i];
    return m;
}

/* the highest value that is at most pct % of the highest one (at least the lowest) */
static unsigned pct_of(const unsigned *v, unsigned n, unsigned pct)
{
    unsigned hi = 0, best = min_of(v, n);
    for (unsigned i = 0; i < n; i++)
        if (v[i] > hi) hi = v[i];
    for (unsigned i = 0; i < n; i++)
        if (v[i] > best && (unsigned long)v[i] * 100 <= (unsigned long)hi * pct) best = v[i];
    return best;
}

static void nv_save(void)
{
    for (unsigned i = 0; i < nv_count(); i++) {
        nvdev d;
        unsigned nf = 0;
        if (nv.handle(i, &d) != 0) continue;
        if (nv.pl_get(d, &nv_saved_mw[i]) == 0) nv_saved[i] = 1;
        if (!nv.fan_count || nv.fan_count(d, &nf) != 0) continue;
        for (unsigned f = 0; f < nf && nnvfans < MAX_NVFANS; f++) {
            unsigned pol = 0, speed = 0;
            if (nv.fan_policy(d, f, &pol) != 0) continue;
            if (pol == NV_FAN_MANUAL && nv.fan_target(d, f, &speed) != 0) continue;
            nvfan[nnvfans].gpu = i, nvfan[nnvfans].fan = f;
            nvfan[nnvfans].policy = pol, nvfan[nnvfans].speed = speed;
            nnvfans++;
        }
    }
}

/* the GPU's fans back to what was saved: the same manual speed, or automatic */
static void nv_fans_back(unsigned i, nvdev d, int quiet)
{
    for (int k = 0; nv.fan_count && k < nnvfans; k++) {
        if (nvfan[k].gpu != i) continue;
        if (nvfan[k].policy == NV_FAN_MANUAL) {
            nv_check("fan speed", i, nv.fan_set(d, nvfan[k].fan, nvfan[k].speed));
            if (!quiet) printf("nvidia gpu%u: fan%u back to %u%%\n", i, nvfan[k].fan, nvfan[k].speed);
        } else {
            nv_check("fan automatic", i, nv.fan_auto(d, nvfan[k].fan));
        }
    }
}

static void nv_level(int level)
{
    for (unsigned i = 0; i < nv_count(); i++) {
        nvdev d;
        unsigned lo = 0, hi = 0, def = 0, mem[64], gr[512], nm = 64, ng = 512;
        if (nv.handle(i, &d) != 0) continue;
        if (nv.pl_range(d, &lo, &hi) == 0 && lo) {
            unsigned want = lo;
            if (level == L_ECO) {
                if (nv.pl_default && nv.pl_default(d, &def) == 0 && def) want = def / 100 * ECO_POWER_PCT;
                if (want < lo) want = lo;
                if (hi && want > hi) want = hi;
                if (nv_saved[i] && nv_saved_mw[i] >= lo && nv_saved_mw[i] < want) want = nv_saved_mw[i];
            }
            nv_check("power limit", i, nv.pl_set(d, want));
            printf("nvidia gpu%u: power limit %u W\n", i, want / 1000);
        }
        if (nv.mem_clocks(d, &nm, mem) == 0 && nm) {
            unsigned mlo = min_of(mem, nm), mhi = level == L_ECO ? pct_of(mem, nm, ECO_MEM_PCT) : mlo;
            nv_check("memory clock lock", i, nv.lock_mem(d, mlo, mhi));
            if (nv.gr_clocks(d, mhi, &ng, gr) == 0 && ng) {
                unsigned glo = min_of(gr, ng), ghi = level == L_ECO ? pct_of(gr, ng, ECO_GPU_PCT) : glo;
                nv_check("graphics clock lock", i, nv.lock_gr(d, glo, ghi));
                if (level == L_ECO)
                    printf("nvidia gpu%u: graphics clock %u to %u MHz, memory %u to %u MHz\n", i, glo, ghi, mlo, mhi);
                else
                    printf("nvidia gpu%u: clocks locked at %u MHz graphics, %u MHz memory\n", i, glo, mlo);
            }
        }
        /* the fans stay as the user had them (put back in case something
         * changed them, e.g. an older MaxEco that used the automatic curve);
         * both levels draw less than Normal, so they are enough */
        unsigned nf = 0;
        if (nv.fan_count && nv.fan_count(d, &nf) == 0 && nf) {
            nv_fans_back(i, d, 1);
            printf("nvidia gpu%u: fans as they were\n", i);
        }
    }
}

static void nv_restore(void)
{
    for (unsigned i = 0; i < nv_count(); i++) {
        nvdev d;
        if (nv.handle(i, &d) != 0) continue;
        nv_check("graphics clock reset", i, nv.reset_gr(d));
        nv_check("memory clock reset", i, nv.reset_mem(d));
        if (nv_saved[i]) {
            nv_check("power limit", i, nv.pl_set(d, nv_saved_mw[i]));
            printf("nvidia gpu%u: clocks unlocked, power limit back to %u W\n", i, nv_saved_mw[i] / 1000);
        }
        nv_fans_back(i, d, 0);
    }
}

/* --------------------------------------------------------------- state */
static char state_path[300], tmp_path[310], level_path[300];

static int state_exists(void) { return access(state_path, F_OK) == 0; }

static int save_state(void)
{
    mkdir(rundir, 0755);
    FILE *f = fopen(tmp_path, "we");
    if (!f) {
        printf("cannot write %s: %s\n", tmp_path, strerror(errno));
        return -1;
    }
    fchmod(fileno(f), 0644);
    fprintf(f, "# maxeco saved settings, restored by: maxeco-helper off\n");
    /* every setting any level changes; a path listed twice (fan modes) is
     * restored twice, in this order */
    for (int i = 0; i < nplan; i++) {
        char cur[128];
        if (read_value(plan[i].path, cur, sizeof cur) == 0) fprintf(f, "file\t%s\t%s\n", plan[i].path, cur);
    }
    for (unsigned i = 0; i < MAX_GPUS; i++)
        if (nv_saved[i]) fprintf(f, "nvidia\t%u\t%u\n", i, nv_saved_mw[i]);
    for (int i = 0; i < nnvfans; i++)
        fprintf(f, "nvfan\t%u\t%u %u %u\n", nvfan[i].gpu, nvfan[i].fan, nvfan[i].policy, nvfan[i].speed);
    if (fclose(f) != 0 || rename(tmp_path, state_path) != 0) {
        printf("cannot write %s: %s\n", state_path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int safe_path(const char *p)
{
    char want[300];
    snprintf(want, sizeof want, "%s/sys/", sysroot);
    return strncmp(p, want, strlen(want)) == 0 && !strstr(p, "/../") && !strchr(p, '\t');
}

static int load_state(void)
{
    FILE *f = fopen(state_path, "re");
    char line[512];
    if (!f) return -1;
    nitems = nnvfans = 0;
    memset(nv_saved, 0, sizeof nv_saved);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (!a || !b) continue;
        *a++ = 0, *b++ = 0;
        if (strcmp(line, "file") == 0 && nitems < MAX_ITEMS && safe_path(a)) {
            snprintf(items[nitems].path, sizeof items[nitems].path, "%s", a);
            snprintf(items[nitems].value, sizeof items[nitems].value, "%s", b);
            nitems++;
        } else if (strcmp(line, "nvidia") == 0) {
            unsigned i = (unsigned)strtoul(a, NULL, 10);
            if (i < MAX_GPUS) nv_saved[i] = 1, nv_saved_mw[i] = (unsigned)strtoul(b, NULL, 10);
        } else if (strcmp(line, "nvfan") == 0 && nnvfans < MAX_NVFANS) {
            unsigned g = (unsigned)strtoul(a, NULL, 10), fan, pol, speed;
            if (g < MAX_GPUS && sscanf(b, "%u %u %u", &fan, &pol, &speed) == 3 && speed <= 100) {
                nvfan[nnvfans].gpu = g, nvfan[nnvfans].fan = fan;
                nvfan[nnvfans].policy = pol, nvfan[nnvfans].speed = speed;
                nnvfans++;
            }
        }
    }
    fclose(f);
    return 0;
}

/* --------------------------------------------------------------- level */
static int read_level(void)
{
    char v[16];
    if (read_value(level_path, v, sizeof v) == 0 && strcmp(v, "eco") == 0) return L_ECO;
    return L_MAX; /* MaxEco 1.1 saved no level and had only this one */
}

static void write_level(int level)
{
    char tmp[320];
    snprintf(tmp, sizeof tmp, "%s.tmp", level_path);
    FILE *f = fopen(tmp, "we");
    if (!f) return;
    fchmod(fileno(f), 0644);
    fprintf(f, "%s\n", level_names[level]);
    if (fclose(f) != 0 || rename(tmp, level_path) != 0) unlink(tmp);
}

/* ------------------------------------------------------------ commands */
/* fan channel already running the curve of this level: nothing to write */
static int group_in_place(int from, int to, int level)
{
    for (int i = from; i < to; i++) {
        char cur[128];
        if (plan[i].steady && (read_value(plan[i].path, cur, sizeof cur) < 0 || strcmp(cur, plan[i].val[level]) != 0))
            return 0;
    }
    return 1;
}

static void apply_plan(int level)
{
    int ok = 0, same = 0, bad = 0;
    for (int i = 0; i < nplan;) {
        int end = i + 1;
        if (plan[i].grp) {
            int unsaved = 0;
            while (end < nplan && plan[end].grp == plan[i].grp) end++;
            for (int k = i; k < end; k++) unsaved |= !saved_value(plan[k].path);
            if (unsaved) { /* not in the saved settings (a fan that started later): "off" could not undo it */
                fan_skipped[plan[i].grp - 1] = 1;
                i = end;
                continue;
            }
            if (group_in_place(i, end, level)) {
                same += end - i;
                i = end;
                continue;
            }
        }
        for (; i < end; i++) {
            const char *orig = saved_value(plan[i].path), *want = plan[i].val[level][0] ? plan[i].val[level] : orig;
            char cur[128];
            if (!orig) continue; /* only what "off" can put back */
            if (!plan[i].grp && read_value(plan[i].path, cur, sizeof cur) == 0 && strcmp(cur, want) == 0) {
                same++;
            } else if (write_value(plan[i].path, want) == 0) {
                ok++;
            } else {
                bad++;
                failures++;
                printf("cannot set %s to %s: %s\n", plan[i].path, want, strerror(errno));
            }
        }
    }
    printf("%s settings applied: %d, already set: %d, failed: %d\n", level == L_ECO ? "Eco" : "MaxEco", ok, same,
           bad);
}

static int cmd_save(void)
{
    if (state_exists()) {
        printf("previous settings already saved in %s\n", state_path);
        return 0;
    }
    nitems = 0;
    build_plan();
    int have_nv = nv_open();
    if (have_nv) nv_save();
    int r = save_state();
    nv_close();
    if (r < 0) return 1;
    printf("previous settings saved in %s\n", state_path);
    return 0;
}

static int cmd_level(int level)
{
    if (!state_exists() && cmd_save() != 0) return 1;
    if (load_state() < 0) {
        printf("cannot read %s\n", state_path);
        return 1;
    }
    build_plan(); /* with the saved values as the originals */
    apply_plan(level);
    check_fans();
    if (nv_open()) {
        nv_level(level);
        nv_close();
    }
    write_level(level);
    printf("level: %s\n", level == L_ECO ? "Eco" : "MaxEco");
    return 0;
}

static int cmd_off(void)
{
    if (load_state() < 0) {
        unlink(level_path);
        printf("MaxEco is not on, nothing to restore\n");
        return 0;
    }
    if (nv_open()) {
        nv_restore();
        nv_close();
    }
    int ok = 0;
    for (int i = 0; i < nitems; i++) {
        if (write_value(items[i].path, items[i].value) == 0) {
            ok++;
        } else {
            failures++;
            printf("cannot restore %s to %s: %s\n", items[i].path, items[i].value, strerror(errno));
        }
    }
    unlink(level_path);
    unlink(state_path);
    printf("settings restored: %d of %d\n", ok, nitems);
    return 0;
}

static int cmd_status(void)
{
    FILE *f = fopen(state_path, "re");
    if (!f) {
        printf("level: Normal\n");
        return 0;
    }
    printf("level: %s\nsaved settings:\n", read_level() == L_ECO ? "Eco" : "MaxEco");
    char line[512];
    while (fgets(line, sizeof line, f))
        if (line[0] != '#') fputs(line, stdout);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (getuid() != 0) { /* test hooks for unprivileged runs against a fake tree */
        const char *v;
        if ((v = getenv("MAXECO_SYSROOT"))) sysroot = v;
        if ((v = getenv("MAXECO_RUNDIR"))) rundir = v;
        no_nvml = getenv("MAXECO_NO_NVML") != NULL;
    }
    snprintf(state_path, sizeof state_path, "%s/state", rundir);
    snprintf(tmp_path, sizeof tmp_path, "%s/state.tmp", rundir);
    snprintf(level_path, sizeof level_path, "%s/level", rundir);
    const char *cmd = argc > 1 ? argv[1] : "", *arg = argc > 2 ? argv[2] : "";
    if (strcmp(cmd, "status") != 0) { /* one change at a time */
        char lock[310];
        snprintf(lock, sizeof lock, "%s/lock", rundir);
        mkdir(rundir, 0755);
        int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) flock(fd, LOCK_EX);
    }
    if (strcmp(cmd, "save") == 0) return cmd_save();
    if (strcmp(cmd, "level") == 0 && argc == 3)
        for (int l = 0; l < NLEVELS; l++)
            if (strcmp(arg, level_names[l]) == 0) return cmd_level(l);
    if (strcmp(cmd, "on") == 0) return cmd_level(L_MAX);
    if (strcmp(cmd, "resume") == 0) return state_exists() ? cmd_level(read_level()) : 0;
    if (strcmp(cmd, "off") == 0) return cmd_off();
    if (strcmp(cmd, "status") == 0) return cmd_status();
    fprintf(stderr, "usage: maxeco-helper save | level eco|max | off | resume | status\n");
    return 2;
}

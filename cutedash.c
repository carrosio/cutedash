#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <ctype.h>
#include <locale.h>
#include <ncurses.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <getopt.h>
#include <signal.h>

#define MAX_CORES 128
#define MAX_PROCS 512
#define MAX_IFACES 16
#define MAX_DOCKER 32
#define HISTORY_LEN 120
#define REFRESH_MS 1000
#define BAR_FULL "\u2501"
#define BAR_DIM  "\u2500"

static const char *SPARK[] = {
    "\u2581", "\u2582", "\u2583", "\u2584",
    "\u2585", "\u2586", "\u2587", "\u2588"
};

enum {
    CLR_GREEN = 1, CLR_YELLOW, CLR_RED, CLR_CYAN,
    CLR_MAGENTA, CLR_BLUE, CLR_DIM, CLR_HEADER,
    CLR_WHITE, CLR_ALERT
};

enum { THEME_DEFAULT = 0, THEME_NEON, THEME_LIGHT, THEME_COUNT };
enum { SORT_CPU = 0, SORT_MEM, SORT_PID };

static int g_theme = THEME_DEFAULT;
static int g_sort = SORT_CPU;
static int g_once = 0;
static int g_alert_cpu = 90;
static int g_alert_temp = 85;
static int g_alert_flash = 0;
static volatile int g_resize = 0;

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total, busy;
} cpu_stat_t;

typedef struct {
    int pid;
    char name[64];
    double cpu_pct;
    double mem_pct;
    unsigned long long prev_total, prev_utime, prev_stime;
} proc_info_t;

typedef struct {
    char name[32];
    unsigned long long rx, tx;
    double rx_speed, tx_speed;
    unsigned long long prev_rx, prev_tx;
} iface_t;

typedef struct {
    char name[64];
    char id[16];
    char status[16];
    double cpu_pct;
    double mem_mb;
} docker_info_t;

typedef struct {
    char label[32];
    int rpm;
} fan_info_t;

typedef struct {
    int present;
    int charging;
    int capacity;
    char status[16];
} battery_t;

typedef struct {
    unsigned long long prev_read, prev_write;
    double read_speed, write_speed;
    double read_hist[HISTORY_LEN], write_hist[HISTORY_LEN];
    int hist_len, hist_pos;
} disk_io_t;

static cpu_stat_t prev_cpu[MAX_CORES + 1];
static int num_cores = 0;
static double cpu_history[HISTORY_LEN];
static int cpu_hist_len = 0, cpu_hist_pos = 0;

static iface_t ifaces[MAX_IFACES];
static int num_ifaces = 0;
static double net_rx_hist[HISTORY_LEN], net_tx_hist[HISTORY_LEN];
static int net_hist_len = 0, net_hist_pos = 0;

static disk_io_t disk_io = {0};

static proc_info_t prev_procs[MAX_PROCS];
static int prev_nprocs = 0;

static void handle_resize(int sig) { (void)sig; g_resize = 1; }

static int color_for_pct(double pct) {
    if (pct < 50.0) return CLR_GREEN;
    if (pct < 80.0) return CLR_YELLOW;
    return CLR_RED;
}

static void draw_bar(WINDOW *w, int y, int x, int width, double pct, int color) {
    int filled = (int)(pct / 100.0 * width);
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;
    int empty = width - filled;
    wmove(w, y, x);
    wattron(w, COLOR_PAIR(color) | A_BOLD);
    for (int i = 0; i < filled; i++) wprintw(w, BAR_FULL);
    wattroff(w, A_BOLD);
    wattron(w, COLOR_PAIR(CLR_DIM));
    for (int i = 0; i < empty; i++) wprintw(w, BAR_DIM);
    wattroff(w, COLOR_PAIR(CLR_DIM) | COLOR_PAIR(color));
}

static void draw_sparkline(WINDOW *w, int y, int x, double *data, int len, int pos, int total, int width) {
    wmove(w, y, x);
    if (len < width) {
        wattron(w, COLOR_PAIR(CLR_DIM));
        for (int i = 0; i < width - len; i++) wprintw(w, BAR_DIM);
        wattroff(w, COLOR_PAIR(CLR_DIM));
    }
    double mn = 1e18, mx = -1e18;
    int count = (len < width) ? len : width;
    for (int i = 0; i < count; i++) {
        int idx = (pos - count + i + total) % total;
        if (data[idx] < mn) mn = data[idx];
        if (data[idx] > mx) mx = data[idx];
    }
    double rng = (mx - mn > 0.001) ? mx - mn : 1.0;
    for (int i = 0; i < count; i++) {
        int idx = (pos - count + i + total) % total;
        int si = (int)((data[idx] - mn) / rng * 7);
        if (si < 0) si = 0; if (si > 7) si = 7;
        double pv = (mx <= 100.0) ? data[idx] : (data[idx] - mn) / rng * 100.0;
        wattron(w, COLOR_PAIR(color_for_pct(pv)));
        wprintw(w, "%s", SPARK[si]);
        wattroff(w, COLOR_PAIR(color_for_pct(pv)));
    }
}

static void fmt_bytes(char *buf, size_t sz, double b) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    snprintf(buf, sz, "%.1f %s", b, u[i]);
}

static void fmt_speed(char *buf, size_t sz, double b) {
    const char *u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int i = 0;
    while (b >= 1024.0 && i < 3) { b /= 1024.0; i++; }
    snprintf(buf, sz, "%.1f %s", b, u[i]);
}

static void draw_box(WINDOW *w, int y, int x, int h, int width, int color, const char *title) {
    if (h < 2 || width < 2) return;
    wattron(w, COLOR_PAIR(color));
    mvwhline(w, y, x, ACS_HLINE, width);
    mvwhline(w, y + h - 1, x, ACS_HLINE, width);
    mvwvline(w, y, x, ACS_VLINE, h);
    mvwvline(w, y, x + width - 1, ACS_VLINE, h);
    mvwaddch(w, y, x, ACS_ULCORNER);
    mvwaddch(w, y, x + width - 1, ACS_URCORNER);
    mvwaddch(w, y + h - 1, x, ACS_LLCORNER);
    mvwaddch(w, y + h - 1, x + width - 1, ACS_LRCORNER);
    if (title) {
        wattron(w, A_BOLD);
        mvwprintw(w, y, x + 2, " %s ", title);
        wattroff(w, A_BOLD);
    }
    wattroff(w, COLOR_PAIR(color));
}

/* ── Data readers ────────────────────────────────────────── */

static void read_cpu_stats(cpu_stat_t *stats, int *count) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[512];
    *count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        cpu_stat_t *s = &stats[*count];
        if (line[3] == ' ')
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &s->user, &s->nice, &s->system, &s->idle,
                   &s->iowait, &s->irq, &s->softirq, &s->steal);
        else
            sscanf(line + 3, "%*d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &s->user, &s->nice, &s->system, &s->idle,
                   &s->iowait, &s->irq, &s->softirq, &s->steal);
        s->total = s->user + s->nice + s->system + s->idle + s->iowait + s->irq + s->softirq + s->steal;
        s->busy = s->total - s->idle - s->iowait;
        (*count)++;
        if (*count > MAX_CORES) break;
    }
    fclose(f);
}

static double calc_cpu_pct(cpu_stat_t *cur, cpu_stat_t *prev) {
    unsigned long long dt = cur->total - prev->total;
    unsigned long long db = cur->busy - prev->busy;
    if (dt == 0) return 0.0;
    return (double)db / dt * 100.0;
}

static void read_mem(unsigned long *total, unsigned long *avail, unsigned long *used,
                     unsigned long *buffers, unsigned long *cached,
                     unsigned long *sw_total, unsigned long *sw_free) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    *total = *avail = *buffers = *cached = *sw_total = *sw_free = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line + 9, "%lu", total);
        else if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line + 13, "%lu", avail);
        else if (strncmp(line, "Buffers:", 8) == 0) sscanf(line + 8, "%lu", buffers);
        else if (strncmp(line, "Cached:", 7) == 0) sscanf(line + 7, "%lu", cached);
        else if (strncmp(line, "SwapTotal:", 10) == 0) sscanf(line + 10, "%lu", sw_total);
        else if (strncmp(line, "SwapFree:", 9) == 0) sscanf(line + 9, "%lu", sw_free);
    }
    fclose(f);
    *used = *total - *avail;
}

static int read_temps(char labels[][32], double *temps, double *highs, double *crits, int max) {
    int count = 0;
    char path[512], buf[128];
    DIR *hwmon = opendir("/sys/class/hwmon");
    if (!hwmon) return 0;
    struct dirent *hd;
    while ((hd = readdir(hwmon)) && count < max) {
        if (hd->d_name[0] == '.') continue;
        char base[512];
        snprintf(base, sizeof(base), "/sys/class/hwmon/%.200s", hd->d_name);
        for (int i = 1; i < 20 && count < max; i++) {
            snprintf(path, sizeof(path), "%.400s/temp%d_input", base, i);
            FILE *f = fopen(path, "r");
            if (!f) break;
            if (fgets(buf, sizeof(buf), f)) temps[count] = atof(buf) / 1000.0;
            fclose(f);

            snprintf(path, sizeof(path), "%.400s/temp%d_label", base, i);
            f = fopen(path, "r");
            if (f) {
                if (fgets(buf, sizeof(buf), f)) { buf[strcspn(buf, "\n")] = 0; snprintf(labels[count], 32, "%.31s", buf); }
                fclose(f);
            } else {
                snprintf(path, sizeof(path), "%.400s/name", base);
                f = fopen(path, "r");
                if (f) {
                    if (fgets(buf, sizeof(buf), f)) { buf[strcspn(buf, "\n")] = 0; snprintf(labels[count], 32, "%.24s #%d", buf, i); }
                    fclose(f);
                } else snprintf(labels[count], 32, "sensor%d", count);
            }
            highs[count] = crits[count] = 0;
            snprintf(path, sizeof(path), "%.400s/temp%d_max", base, i);
            f = fopen(path, "r");
            if (f) { if (fgets(buf, sizeof(buf), f)) highs[count] = atof(buf) / 1000.0; fclose(f); }
            snprintf(path, sizeof(path), "%.400s/temp%d_crit", base, i);
            f = fopen(path, "r");
            if (f) { if (fgets(buf, sizeof(buf), f)) crits[count] = atof(buf) / 1000.0; fclose(f); }
            count++;
        }
    }
    closedir(hwmon);
    return count;
}

static int read_fans(fan_info_t *fans, int max) {
    int count = 0;
    char path[512], buf[128];
    DIR *hwmon = opendir("/sys/class/hwmon");
    if (!hwmon) return 0;
    struct dirent *hd;
    while ((hd = readdir(hwmon)) && count < max) {
        if (hd->d_name[0] == '.') continue;
        char base[512];
        snprintf(base, sizeof(base), "/sys/class/hwmon/%.200s", hd->d_name);
        for (int i = 1; i < 10 && count < max; i++) {
            snprintf(path, sizeof(path), "%.400s/fan%d_input", base, i);
            FILE *f = fopen(path, "r");
            if (!f) break;
            if (fgets(buf, sizeof(buf), f)) fans[count].rpm = atoi(buf);
            fclose(f);

            snprintf(path, sizeof(path), "%.400s/fan%d_label", base, i);
            f = fopen(path, "r");
            if (f) {
                if (fgets(buf, sizeof(buf), f)) { buf[strcspn(buf, "\n")] = 0; snprintf(fans[count].label, 32, "%.31s", buf); }
                fclose(f);
            } else {
                snprintf(fans[count].label, 32, "Fan %d", i);
            }
            count++;
        }
    }
    closedir(hwmon);
    return count;
}

static int read_ifaces(iface_t *ifs, int max) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return 0;
    char line[512];
    int count = 0;
    (void)fgets(line, sizeof(line), f);
    (void)fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f) && count < max) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char iface[64] = {0};
        int len = (int)(colon - line);
        if (len > 63) len = 63;
        strncpy(iface, line, len);
        iface[len] = 0;
        char *p = iface;
        while (*p == ' ') p++;
        if (strcmp(p, "lo") == 0) continue;

        snprintf(ifs[count].name, 32, "%s", p);
        unsigned long long r, t;
        sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t);
        ifs[count].rx = r;
        ifs[count].tx = t;

        for (int i = 0; i < num_ifaces; i++) {
            if (strcmp(ifaces[i].name, ifs[count].name) == 0) {
                ifs[count].rx_speed = (double)(r - ifaces[i].rx) / (REFRESH_MS / 1000.0);
                ifs[count].tx_speed = (double)(t - ifaces[i].tx) / (REFRESH_MS / 1000.0);
                break;
            }
        }
        count++;
    }
    fclose(f);
    return count;
}

static void read_disk_io(disk_io_t *dio) {
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return;
    char line[512];
    unsigned long long total_read = 0, total_write = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned int major, minor;
        char devname[64];
        unsigned long long rd_sectors, wr_sectors;
        int n = sscanf(line, "%u %u %63s %*u %*u %llu %*u %*u %*u %llu",
                       &major, &minor, devname, &rd_sectors, &wr_sectors);
        if (n < 5) continue;
        if (minor != 0) continue;
        if (strncmp(devname, "loop", 4) == 0) continue;
        if (strncmp(devname, "ram", 3) == 0) continue;
        total_read += rd_sectors * 512;
        total_write += wr_sectors * 512;
    }
    fclose(f);

    if (dio->prev_read > 0) {
        dio->read_speed = (double)(total_read - dio->prev_read) / (REFRESH_MS / 1000.0);
        dio->write_speed = (double)(total_write - dio->prev_write) / (REFRESH_MS / 1000.0);
    }
    dio->prev_read = total_read;
    dio->prev_write = total_write;

    dio->read_hist[dio->hist_pos] = dio->read_speed;
    dio->write_hist[dio->hist_pos] = dio->write_speed;
    dio->hist_pos = (dio->hist_pos + 1) % HISTORY_LEN;
    if (dio->hist_len < HISTORY_LEN) dio->hist_len++;
}

static int read_procs_with_cpu(proc_info_t *procs, int max, unsigned long mem_total_kb,
                               proc_info_t *prev, int prev_count) {
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return 0;
    int count = 0;
    struct dirent *de;
    long clk = sysconf(_SC_CLK_TCK);
    long page_size = sysconf(_SC_PAGESIZE);

    while ((de = readdir(proc_dir)) && count < max) {
        if (!isdigit(de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        char path[256], line[1024];

        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(line, sizeof(line), f)) { fclose(f); continue; }
        fclose(f);

        char *name_s = strchr(line, '(');
        char *name_e = strrchr(line, ')');
        if (!name_s || !name_e) continue;

        procs[count].pid = pid;
        int nlen = (int)(name_e - name_s - 1);
        if (nlen > 63) nlen = 63;
        memcpy(procs[count].name, name_s + 1, nlen);
        procs[count].name[nlen] = 0;

        unsigned long utime = 0, stime = 0;
        char *p = name_e + 2;
        int field = 0;
        while (*p && field < 12) {
            while (*p == ' ') p++;
            if (field == 11) { utime = strtoul(p, &p, 10); }
            else if (field == 12) { stime = strtoul(p, &p, 10); }
            else { while (*p && *p != ' ') p++; }
            field++;
        }
        if (*p) stime = strtoul(p, NULL, 10);

        unsigned long long proc_total_time = utime + stime;
        FILE *uf = fopen("/proc/uptime", "r");
        double uptime_sec = 0;
        if (uf) { (void)fscanf(uf, "%lf", &uptime_sec); fclose(uf); }
        unsigned long long sys_total = (unsigned long long)(uptime_sec * clk);

        procs[count].cpu_pct = 0;
        for (int i = 0; i < prev_count; i++) {
            if (prev[i].pid == pid) {
                unsigned long long dt = sys_total - prev[i].prev_total;
                unsigned long long dp = proc_total_time - (prev[i].prev_utime + prev[i].prev_stime);
                if (dt > 0)
                    procs[count].cpu_pct = (double)dp / dt * 100.0 * num_cores;
                break;
            }
        }
        procs[count].prev_total = sys_total;
        procs[count].prev_utime = utime;
        procs[count].prev_stime = stime;

        unsigned long rss = 0;
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        f = fopen(path, "r");
        if (f) { unsigned long sz; (void)fscanf(f, "%lu %lu", &sz, &rss); fclose(f); }
        double mem_kb = (double)rss * page_size / 1024.0;
        procs[count].mem_pct = (mem_total_kb > 0) ? mem_kb / mem_total_kb * 100.0 : 0;
        count++;
    }
    closedir(proc_dir);
    return count;
}

static int proc_cmp_cpu(const void *a, const void *b) {
    double da = ((const proc_info_t *)a)->cpu_pct, db = ((const proc_info_t *)b)->cpu_pct;
    return (db > da) - (db < da);
}
static int proc_cmp_mem(const void *a, const void *b) {
    double da = ((const proc_info_t *)a)->mem_pct, db = ((const proc_info_t *)b)->mem_pct;
    return (db > da) - (db < da);
}
static int proc_cmp_pid(const void *a, const void *b) {
    return ((const proc_info_t *)b)->pid - ((const proc_info_t *)a)->pid;
}

static battery_t read_battery(void) {
    battery_t bat = {0};
    FILE *f;
    char buf[128];

    f = fopen("/sys/class/power_supply/BAT0/present", "r");
    if (!f) f = fopen("/sys/class/power_supply/BAT1/present", "r");
    if (!f) return bat;
    if (fgets(buf, sizeof(buf), f)) bat.present = atoi(buf);
    fclose(f);
    if (!bat.present) return bat;

    const char *bases[] = {"/sys/class/power_supply/BAT0", "/sys/class/power_supply/BAT1"};
    const char *base = NULL;
    for (int i = 0; i < 2; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/capacity", bases[i]);
        f = fopen(p, "r");
        if (f) { base = bases[i]; fclose(f); break; }
    }
    if (!base) return bat;

    char p[256];
    snprintf(p, sizeof(p), "%s/capacity", base);
    f = fopen(p, "r");
    if (f) { if (fgets(buf, sizeof(buf), f)) bat.capacity = atoi(buf); fclose(f); }

    snprintf(p, sizeof(p), "%s/status", base);
    f = fopen(p, "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\n")] = 0;
            snprintf(bat.status, sizeof(bat.status), "%.15s", buf);
            bat.charging = (strcmp(buf, "Charging") == 0);
        }
        fclose(f);
    }
    return bat;
}

typedef struct {
    int has_gpu;
    char name[64];
    int temp;
    int fan_pct;
    int gpu_util;
    int mem_util;
    int mem_used_mb;
    int mem_total_mb;
    int power_w;
    int power_max_w;
} gpu_info_t;

static gpu_info_t read_gpu(void) {
    gpu_info_t g = {0};
    FILE *f = popen("nvidia-smi --query-gpu=name,temperature.gpu,fan.speed,utilization.gpu,"
                    "utilization.memory,memory.used,memory.total,power.draw,power.limit "
                    "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!f) return g;
    char line[512];
    if (fgets(line, sizeof(line), f)) {
        char name[64];
        int temp, fan, gpu_u, mem_u, mem_used, mem_total, pwr, pwr_max;
        int n = sscanf(line, "%63[^,], %d, %d, %d, %d, %d, %d, %d, %d",
                       name, &temp, &fan, &gpu_u, &mem_u, &mem_used, &mem_total, &pwr, &pwr_max);
        if (n >= 7) {
            g.has_gpu = 1;
            snprintf(g.name, 64, "%.63s", name);
            g.temp = temp;
            g.fan_pct = fan;
            g.gpu_util = gpu_u;
            g.mem_util = mem_u;
            g.mem_used_mb = mem_used;
            g.mem_total_mb = mem_total;
            g.power_w = (n >= 8) ? pwr : 0;
            g.power_max_w = (n >= 9) ? pwr_max : 0;
        }
    }
    pclose(f);
    return g;
}

static int read_docker(docker_info_t *containers, int max) {
    FILE *f = popen("docker ps --format '{{.Names}}\\t{{.ID}}\\t{{.Status}}' 2>/dev/null", "r");
    if (!f) return 0;
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < max) {
        line[strcspn(line, "\n")] = 0;
        char name[64], id[16], status[64];
        int n = sscanf(line, "%63[^\t]\t%15[^\t]\t%63[^\n]", name, id, status);
        if (n < 2) continue;
        snprintf(containers[count].name, 64, "%.63s", name);
        snprintf(containers[count].id, 16, "%.12s", id);
        snprintf(containers[count].status, 16, "%.15s", n >= 3 ? status : "?");
        containers[count].cpu_pct = 0;
        containers[count].mem_mb = 0;
        count++;
    }
    pclose(f);

    if (count > 0) {
        f = popen("docker stats --no-stream --format '{{.Name}}\\t{{.CPUPerc}}\\t{{.MemUsage}}' 2>/dev/null", "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                char name[64], cpu_s[16], mem_s[64];
                if (sscanf(line, "%63[^\t]\t%15[^\t]\t%63[^\n]", name, cpu_s, mem_s) < 2) continue;
                double cpu = atof(cpu_s);
                double mem = 0;
                char *slash = strchr(mem_s, '/');
                if (slash) {
                    mem = atof(mem_s);
                    if (strstr(mem_s, "GiB")) mem *= 1024;
                }
                for (int i = 0; i < count; i++) {
                    if (strcmp(containers[i].name, name) == 0) {
                        containers[i].cpu_pct = cpu;
                        containers[i].mem_mb = mem;
                        break;
                    }
                }
            }
            pclose(f);
        }
    }
    return count;
}

/* ── Theme setup ─────────────────────────────────────────── */

static void setup_theme(void) {
    use_default_colors();
    switch (g_theme) {
    case THEME_NEON:
        init_pair(CLR_GREEN, COLOR_GREEN, -1);
        init_pair(CLR_YELLOW, COLOR_YELLOW, -1);
        init_pair(CLR_RED, COLOR_RED, -1);
        init_pair(CLR_CYAN, COLOR_CYAN, -1);
        init_pair(CLR_MAGENTA, COLOR_MAGENTA, -1);
        init_pair(CLR_BLUE, COLOR_BLUE, -1);
        init_pair(CLR_DIM, 8, -1);
        init_pair(CLR_HEADER, COLOR_MAGENTA, -1);
        init_pair(CLR_WHITE, COLOR_WHITE, -1);
        init_pair(CLR_ALERT, COLOR_RED, COLOR_YELLOW);
        break;
    case THEME_LIGHT:
        init_pair(CLR_GREEN, COLOR_GREEN, -1);
        init_pair(CLR_YELLOW, COLOR_YELLOW, -1);
        init_pair(CLR_RED, COLOR_RED, -1);
        init_pair(CLR_CYAN, COLOR_BLUE, -1);
        init_pair(CLR_MAGENTA, COLOR_MAGENTA, -1);
        init_pair(CLR_BLUE, COLOR_BLUE, -1);
        init_pair(CLR_DIM, COLOR_WHITE, -1);
        init_pair(CLR_HEADER, COLOR_BLUE, -1);
        init_pair(CLR_WHITE, COLOR_BLACK, -1);
        init_pair(CLR_ALERT, COLOR_WHITE, COLOR_RED);
        break;
    default:
        init_pair(CLR_GREEN, COLOR_GREEN, -1);
        init_pair(CLR_YELLOW, COLOR_YELLOW, -1);
        init_pair(CLR_RED, COLOR_RED, -1);
        init_pair(CLR_CYAN, COLOR_CYAN, -1);
        init_pair(CLR_MAGENTA, COLOR_MAGENTA, -1);
        init_pair(CLR_BLUE, COLOR_BLUE, -1);
        init_pair(CLR_DIM, 8, -1);
        init_pair(CLR_HEADER, COLOR_CYAN, -1);
        init_pair(CLR_WHITE, COLOR_WHITE, -1);
        init_pair(CLR_ALERT, COLOR_WHITE, COLOR_RED);
        break;
    }
}

/* ── Header ──────────────────────────────────────────────── */

static void draw_header(WINDOW *w, int cols, double cpu_avg, double mem_pct, int alert) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%a %b %d  %H:%M:%S", tm);
    struct sysinfo si;
    sysinfo(&si);
    int days = si.uptime / 86400;
    int hours = (si.uptime % 86400) / 3600;
    int mins = (si.uptime % 3600) / 60;

    if (alert && g_alert_flash) {
        wattron(w, COLOR_PAIR(CLR_ALERT) | A_BOLD | A_BLINK);
        mvwhline(w, 0, 0, ' ', cols);
        mvwprintw(w, 0, 2, " !! ALERT ");
        wattroff(w, A_BLINK);
    } else {
        wattron(w, COLOR_PAIR(CLR_HEADER) | A_BOLD);
        mvwhline(w, 0, 0, ' ', cols);
        mvwprintw(w, 0, 2, " CUTEDASH ");
    }
    wattroff(w, A_BOLD);
    wattron(w, COLOR_PAIR(CLR_DIM)); wprintw(w, " | "); wattroff(w, COLOR_PAIR(CLR_DIM));
    wattron(w, COLOR_PAIR(CLR_HEADER)); wprintw(w, "%s", timebuf); wattroff(w, COLOR_PAIR(CLR_HEADER));
    wattron(w, COLOR_PAIR(CLR_DIM)); wprintw(w, "  |  "); wattroff(w, COLOR_PAIR(CLR_DIM));
    wprintw(w, "up %dd %dh %dm", days, hours, mins);
    wattron(w, COLOR_PAIR(CLR_DIM)); wprintw(w, "  |  "); wattroff(w, COLOR_PAIR(CLR_DIM));
    wprintw(w, "CPU ");
    wattron(w, COLOR_PAIR(color_for_pct(cpu_avg)) | A_BOLD); wprintw(w, "%.0f%%", cpu_avg); wattroff(w, A_BOLD | COLOR_PAIR(color_for_pct(cpu_avg)));
    wprintw(w, "  MEM ");
    wattron(w, COLOR_PAIR(color_for_pct(mem_pct)) | A_BOLD); wprintw(w, "%.0f%%", mem_pct); wattroff(w, A_BOLD | COLOR_PAIR(color_for_pct(mem_pct)));

    const char *sort_labels[] = {"cpu", "mem", "pid"};
    wattron(w, COLOR_PAIR(CLR_DIM));
    mvwprintw(w, 0, cols - 40, "sort:%s  t:theme  q:exit ", sort_labels[g_sort]);
    wattroff(w, COLOR_PAIR(CLR_DIM) | COLOR_PAIR(CLR_HEADER) | COLOR_PAIR(CLR_ALERT));
}

/* ── Snapshot mode (--once) ──────────────────────────────── */

static void print_snapshot(void) {
    read_cpu_stats(prev_cpu, &num_cores);
    num_cores--;
    usleep(500000);
    cpu_stat_t cur_cpu[MAX_CORES + 1];
    int cur_count;
    read_cpu_stats(cur_cpu, &cur_count);
    double cpu_avg = calc_cpu_pct(&cur_cpu[0], &prev_cpu[0]);

    unsigned long mt = 0, ma = 0, mu = 0, mb = 0, mc = 0, st = 0, sf = 0;
    read_mem(&mt, &ma, &mu, &mb, &mc, &st, &sf);
    double mem_pct = (mt > 0) ? (double)mu / mt * 100.0 : 0;

    struct sysinfo si;
    sysinfo(&si);
    time_t now = time(NULL);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

    printf("=== CUTEDASH SNAPSHOT === %s\n\n", timebuf);
    printf("Uptime: %ldd %ldh %ldm\n\n", si.uptime / 86400, (si.uptime % 86400) / 3600, (si.uptime % 3600) / 60);

    printf("-- CPU --\n");
    for (int i = 0; i < num_cores; i++)
        printf("  Core %d: %5.1f%%\n", i, calc_cpu_pct(&cur_cpu[i + 1], &prev_cpu[i + 1]));
    printf("  Average: %.1f%%\n", cpu_avg);
    double l1, l5, l15;
    FILE *lf = fopen("/proc/loadavg", "r");
    if (lf) { (void)fscanf(lf, "%lf %lf %lf", &l1, &l5, &l15); fclose(lf); printf("  Load: %.2f / %.2f / %.2f\n", l1, l5, l15); }

    printf("\n-- MEMORY --\n");
    printf("  Used: %.1f / %.1f GB (%.1f%%)\n", mu / 1048576.0, mt / 1048576.0, mem_pct);
    printf("  Available: %.1f GB\n", ma / 1048576.0);
    printf("  Cached: %.1f GB  Buffers: %.1f GB\n", mc / 1048576.0, mb / 1048576.0);
    if (st > 0) printf("  Swap: %.1f / %.1f GB\n", (st - sf) / 1048576.0, st / 1048576.0);

    char t_labels[32][32]; double t_vals[32], t_highs[32], t_crits[32];
    int tc = read_temps(t_labels, t_vals, t_highs, t_crits, 32);
    if (tc > 0) {
        printf("\n-- TEMPS --\n");
        for (int i = 0; i < tc; i++) printf("  %-16s %4.0f\u00b0C\n", t_labels[i], t_vals[i]);
    }

    fan_info_t fans[16];
    int fc = read_fans(fans, 16);
    if (fc > 0) {
        printf("\n-- FANS --\n");
        for (int i = 0; i < fc; i++) printf("  %-16s %d RPM\n", fans[i].label, fans[i].rpm);
    }

    gpu_info_t gpu = read_gpu();
    if (gpu.has_gpu) {
        printf("\n-- GPU --\n");
        printf("  %s\n", gpu.name);
        printf("  Util: %d%%  Mem: %d/%d MB  Temp: %d\u00b0C", gpu.gpu_util, gpu.mem_used_mb, gpu.mem_total_mb, gpu.temp);
        if (gpu.power_w > 0) printf("  Power: %dW/%dW", gpu.power_w, gpu.power_max_w);
        printf("\n");
    }

    battery_t bat = read_battery();
    if (bat.present) printf("\n-- BATTERY --\n  %d%% (%s)\n", bat.capacity, bat.status);

    printf("\n-- DISK --\n");
    FILE *mf = fopen("/proc/mounts", "r");
    if (mf) {
        char mline[512];
        while (fgets(mline, sizeof(mline), mf)) {
            char dev[128], mount[128];
            sscanf(mline, "%127s %127s", dev, mount);
            if (strncmp(dev, "/dev/", 5) != 0 || strstr(dev, "loop") || strstr(mount, "/snap")) continue;
            struct statvfs svfs;
            if (statvfs(mount, &svfs) != 0) continue;
            double tot = (double)svfs.f_blocks * svfs.f_frsize;
            double used = tot - (double)svfs.f_bfree * svfs.f_frsize;
            char ub[16], tb[16];
            fmt_bytes(ub, 16, used); fmt_bytes(tb, 16, tot);
            printf("  %-20s %s / %s (%.0f%%)\n", mount, ub, tb, (tot > 0) ? used / tot * 100 : 0);
        }
        fclose(mf);
    }

    docker_info_t dk[MAX_DOCKER];
    int dc = read_docker(dk, MAX_DOCKER);
    if (dc > 0) {
        printf("\n-- DOCKER (%d containers) --\n", dc);
        for (int i = 0; i < dc; i++)
            printf("  %-24s %s  CPU: %.1f%%  Mem: %.0f MB\n", dk[i].name, dk[i].status, dk[i].cpu_pct, dk[i].mem_mb);
    }

    printf("\n");
}

/* ── Main ────────────────────────────────────────────────── */

static void usage(void) {
    printf("cutedash - terminal system dashboard\n\n"
           "Usage: stats [OPTIONS]\n\n"
           "Options:\n"
           "  --once           Print snapshot and exit\n"
           "  --theme THEME    Color theme: default, neon, light\n"
           "  --alert-cpu N    CPU alert threshold (default: 90)\n"
           "  --alert-temp N   Temp alert threshold (default: 85)\n"
           "  -h, --help       Show this help\n\n"
           "Keys:\n"
           "  c/m/p  Sort processes by CPU/MEM/PID\n"
           "  t      Cycle color theme\n"
           "  q      Quit\n");
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");

    static struct option long_opts[] = {
        {"once", no_argument, NULL, 'o'},
        {"theme", required_argument, NULL, 't'},
        {"alert-cpu", required_argument, NULL, 'C'},
        {"alert-temp", required_argument, NULL, 'T'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "oth", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'o': g_once = 1; break;
        case 't':
            if (strcmp(optarg, "neon") == 0) g_theme = THEME_NEON;
            else if (strcmp(optarg, "light") == 0) g_theme = THEME_LIGHT;
            else g_theme = THEME_DEFAULT;
            break;
        case 'C': g_alert_cpu = atoi(optarg); break;
        case 'T': g_alert_temp = atoi(optarg); break;
        case 'h': usage(); return 0;
        default: usage(); return 1;
        }
    }

    if (g_once) { print_snapshot(); return 0; }

    signal(SIGWINCH, handle_resize);

    read_cpu_stats(prev_cpu, &num_cores);
    num_cores--;
    num_ifaces = read_ifaces(ifaces, MAX_IFACES);
    read_disk_io(&disk_io);
    usleep(200000);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    start_color();
    setup_theme();

    while (1) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        if (ch == 'c' || ch == 'C') g_sort = SORT_CPU;
        if (ch == 'm' || ch == 'M') g_sort = SORT_MEM;
        if (ch == 'p' || ch == 'P') g_sort = SORT_PID;
        if (ch == 't' || ch == 'T') { g_theme = (g_theme + 1) % THEME_COUNT; setup_theme(); }

        if (g_resize) { g_resize = 0; endwin(); refresh(); clear(); }

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();

        /* ── Read all data ── */
        cpu_stat_t cur_cpu[MAX_CORES + 1];
        int cur_count;
        read_cpu_stats(cur_cpu, &cur_count);
        double core_pcts[MAX_CORES];
        double cpu_avg = calc_cpu_pct(&cur_cpu[0], &prev_cpu[0]);
        for (int i = 0; i < num_cores; i++)
            core_pcts[i] = calc_cpu_pct(&cur_cpu[i + 1], &prev_cpu[i + 1]);
        memcpy(prev_cpu, cur_cpu, sizeof(prev_cpu));

        cpu_history[cpu_hist_pos] = cpu_avg;
        cpu_hist_pos = (cpu_hist_pos + 1) % HISTORY_LEN;
        if (cpu_hist_len < HISTORY_LEN) cpu_hist_len++;

        unsigned long mem_total = 0, mem_avail = 0, mem_used = 0, mem_buf = 0, mem_cached = 0, sw_total = 0, sw_free = 0;
        read_mem(&mem_total, &mem_avail, &mem_used, &mem_buf, &mem_cached, &sw_total, &sw_free);
        double mem_pct = (mem_total > 0) ? (double)mem_used / mem_total * 100.0 : 0;

        char t_labels[32][32]; double t_vals[32], t_highs[32], t_crits[32];
        int t_count = read_temps(t_labels, t_vals, t_highs, t_crits, 32);
        fan_info_t fans[16];
        int fan_count = read_fans(fans, 16);

        iface_t new_ifaces[MAX_IFACES];
        int new_nifaces = read_ifaces(new_ifaces, MAX_IFACES);
        double total_rx_speed = 0, total_tx_speed = 0;
        for (int i = 0; i < new_nifaces; i++) {
            total_rx_speed += new_ifaces[i].rx_speed;
            total_tx_speed += new_ifaces[i].tx_speed;
        }
        memcpy(ifaces, new_ifaces, sizeof(ifaces));
        num_ifaces = new_nifaces;

        net_rx_hist[net_hist_pos] = total_rx_speed;
        net_tx_hist[net_hist_pos] = total_tx_speed;
        net_hist_pos = (net_hist_pos + 1) % HISTORY_LEN;
        if (net_hist_len < HISTORY_LEN) net_hist_len++;

        read_disk_io(&disk_io);

        proc_info_t procs[MAX_PROCS];
        int nprocs = read_procs_with_cpu(procs, MAX_PROCS, mem_total, prev_procs, prev_nprocs);
        memcpy(prev_procs, procs, nprocs * sizeof(proc_info_t));
        prev_nprocs = nprocs;

        if (g_sort == SORT_CPU) qsort(procs, nprocs, sizeof(proc_info_t), proc_cmp_cpu);
        else if (g_sort == SORT_MEM) qsort(procs, nprocs, sizeof(proc_info_t), proc_cmp_mem);
        else qsort(procs, nprocs, sizeof(proc_info_t), proc_cmp_pid);

        gpu_info_t gpu = {0};
        static int gpu_tick = 0;
        static gpu_info_t cached_gpu = {0};
        if (gpu_tick % 3 == 0) cached_gpu = read_gpu();
        gpu_tick++;
        gpu = cached_gpu;

        static int docker_tick = 0;
        static docker_info_t cached_docker[MAX_DOCKER];
        static int cached_docker_count = 0;
        if (docker_tick % 5 == 0) cached_docker_count = read_docker(cached_docker, MAX_DOCKER);
        docker_tick++;

        battery_t bat = {0};
        static int bat_tick = 0;
        static battery_t cached_bat = {0};
        if (bat_tick % 10 == 0) cached_bat = read_battery();
        bat_tick++;
        bat = cached_bat;

        int alert = (cpu_avg >= g_alert_cpu);
        for (int i = 0; i < t_count && !alert; i++)
            if (t_vals[i] >= g_alert_temp) alert = 1;
        g_alert_flash = alert;

        /* ── Layout ── */
        draw_header(stdscr, cols, cpu_avg, mem_pct, alert);

        int has_gpu = gpu.has_gpu;
        int has_docker = (cached_docker_count > 0);
        int has_battery = bat.present;

        int top_h = (rows - 2) * 3 / 5;
        int bot_h = rows - 2 - top_h;

        int ncols_top = 3 + has_gpu;
        int col_w = cols / ncols_top;
        int last_col_w = cols - col_w * (ncols_top - 1);

        int by = 2;

        /* ── CPU panel ── */
        {
            int pw = col_w;
            draw_box(stdscr, by, 0, top_h, pw, CLR_CYAN, "CPU");
            int bar_w = pw / 2 - 12;
            if (bar_w < 8) bar_w = 8;
            if (bar_w > 30) bar_w = 30;
            int cy = by + 2;
            for (int i = 0; i < num_cores && cy < by + top_h - 5; i += 2) {
                for (int j = 0; j < 2 && (i + j) < num_cores; j++) {
                    int cx = 3 + j * (bar_w + 11);
                    int core = i + j;
                    wattron(stdscr, COLOR_PAIR(CLR_DIM));
                    mvwprintw(stdscr, cy, cx, "C%-2d", core);
                    wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                    draw_bar(stdscr, cy, cx + 4, bar_w, core_pcts[core], color_for_pct(core_pcts[core]));
                    wattron(stdscr, COLOR_PAIR(color_for_pct(core_pcts[core])) | A_BOLD);
                    wprintw(stdscr, " %5.1f%%", core_pcts[core]);
                    wattroff(stdscr, COLOR_PAIR(color_for_pct(core_pcts[core])) | A_BOLD);
                }
                cy++;
            }
            cy++;
            int ac = color_for_pct(cpu_avg);
            wattron(stdscr, A_BOLD); mvwprintw(stdscr, cy, 3, "AVG"); wattroff(stdscr, A_BOLD);
            draw_bar(stdscr, cy, 7, bar_w, cpu_avg, ac);
            wattron(stdscr, COLOR_PAIR(ac) | A_BOLD); wprintw(stdscr, " %5.1f%%", cpu_avg); wattroff(stdscr, COLOR_PAIR(ac) | A_BOLD);
            cy++;
            int sw = pw - 10;
            if (sw > HISTORY_LEN) sw = HISTORY_LEN;
            if (sw < 10) sw = 10;
            draw_sparkline(stdscr, cy, 7, cpu_history, cpu_hist_len, cpu_hist_pos, HISTORY_LEN, sw);
            cy++;
            double l1 = 0, l5 = 0, l15 = 0;
            FILE *lf = fopen("/proc/loadavg", "r");
            if (lf) { (void)fscanf(lf, "%lf %lf %lf", &l1, &l5, &l15); fclose(lf); }
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, cy, 3, "Load:"); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            wattron(stdscr, COLOR_PAIR(color_for_pct(l1 / num_cores * 100)));
            wprintw(stdscr, " %.2f", l1);
            wattroff(stdscr, COLOR_PAIR(color_for_pct(l1 / num_cores * 100)));
            wattron(stdscr, COLOR_PAIR(CLR_DIM));
            wprintw(stdscr, " / %.2f / %.2f  %d cores", l5, l15, num_cores);
            wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        }

        /* ── Memory panel ── */
        {
            int px = col_w;
            int pw = col_w;
            draw_box(stdscr, by, px, top_h, pw, CLR_MAGENTA, "MEMORY");
            int mbw = pw - 12;
            if (mbw < 8) mbw = 8; if (mbw > 35) mbw = 35;
            int my = by + 2;
            draw_bar(stdscr, my, px + 3, mbw, mem_pct, color_for_pct(mem_pct));
            my += 2;
            wattron(stdscr, A_BOLD); mvwprintw(stdscr, my, px + 3, "%.1f", mem_used / 1048576.0); wattroff(stdscr, A_BOLD);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " GB used of "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            wattron(stdscr, A_BOLD); wprintw(stdscr, "%.1f", mem_total / 1048576.0); wattroff(stdscr, A_BOLD);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " GB"); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            my++;
            wattron(stdscr, COLOR_PAIR(CLR_GREEN)); mvwprintw(stdscr, my, px + 3, "%.1f", mem_avail / 1048576.0); wattroff(stdscr, COLOR_PAIR(CLR_GREEN));
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " GB available"); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            my += 2;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, my, px + 3, "Cached  "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            wattron(stdscr, A_BOLD); wprintw(stdscr, "%.1f", mem_cached / 1048576.0); wattroff(stdscr, A_BOLD);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " GB"); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            my++;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, my, px + 3, "Buffers "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            wattron(stdscr, A_BOLD); wprintw(stdscr, "%.1f", mem_buf / 1048576.0); wattroff(stdscr, A_BOLD);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " GB"); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            if (sw_total > 0) {
                my += 2;
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, my, px + 3, "Swap    "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                wprintw(stdscr, "%.1f / %.1f GB", (sw_total - sw_free) / 1048576.0, sw_total / 1048576.0);
            }
            if (has_battery) {
                my += 2;
                int bc = bat.capacity > 50 ? CLR_GREEN : bat.capacity > 20 ? CLR_YELLOW : CLR_RED;
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, my, px + 3, "Battery "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                wattron(stdscr, COLOR_PAIR(bc) | A_BOLD); wprintw(stdscr, "%d%%", bat.capacity); wattroff(stdscr, COLOR_PAIR(bc) | A_BOLD);
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " %s", bat.status); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            }
        }

        /* ── Temps + Fans panel ── */
        {
            int px = col_w * 2;
            int pw = has_gpu ? col_w : last_col_w;
            draw_box(stdscr, by, px, top_h, pw, CLR_RED, "TEMPS / FANS");
            int ty = by + 2;
            int tbw = pw - 28;
            if (tbw < 6) tbw = 6; if (tbw > 20) tbw = 20;
            if (t_count == 0) {
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, ty, px + 3, "No sensors"); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            }
            for (int i = 0; i < t_count && ty < by + top_h - fan_count - 3; i++) {
                double t = t_vals[i];
                int tc = color_for_pct(t > 40 ? t : 0);
                mvwprintw(stdscr, ty, px + 3, "%-12.12s", t_labels[i]);
                draw_bar(stdscr, ty, px + 16, tbw, t, tc);
                wattron(stdscr, COLOR_PAIR(tc) | A_BOLD); wprintw(stdscr, " %3.0f\u00b0C", t); wattroff(stdscr, COLOR_PAIR(tc) | A_BOLD);
                if (t_highs[i] > 0) { wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " H:%.0f", t_highs[i]); wattroff(stdscr, COLOR_PAIR(CLR_DIM)); }
                ty++;
            }
            if (fan_count > 0) {
                ty++;
                wattron(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
                mvwprintw(stdscr, ty, px + 3, "Fans");
                wattroff(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
                ty++;
                for (int i = 0; i < fan_count && ty < by + top_h - 1; i++) {
                    mvwprintw(stdscr, ty, px + 3, "%-12.12s", fans[i].label);
                    int fc_clr = fans[i].rpm > 3000 ? CLR_RED : fans[i].rpm > 1500 ? CLR_YELLOW : CLR_GREEN;
                    wattron(stdscr, COLOR_PAIR(fc_clr) | A_BOLD);
                    wprintw(stdscr, " %d RPM", fans[i].rpm);
                    wattroff(stdscr, COLOR_PAIR(fc_clr) | A_BOLD);
                    ty++;
                }
            }
        }

        /* ── GPU panel (if present) ── */
        if (has_gpu) {
            int px = col_w * 3;
            int pw = last_col_w;
            draw_box(stdscr, by, px, top_h, pw, CLR_GREEN, "GPU");
            int gy = by + 2;
            int gbw = pw - 16;
            if (gbw < 8) gbw = 8; if (gbw > 25) gbw = 25;

            wattron(stdscr, A_BOLD); mvwprintw(stdscr, gy, px + 3, "%.20s", gpu.name); wattroff(stdscr, A_BOLD);
            gy += 2;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, gy, px + 3, "GPU  "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            draw_bar(stdscr, gy, px + 8, gbw, gpu.gpu_util, color_for_pct(gpu.gpu_util));
            wattron(stdscr, COLOR_PAIR(color_for_pct(gpu.gpu_util)) | A_BOLD); wprintw(stdscr, " %3d%%", gpu.gpu_util); wattroff(stdscr, A_BOLD);
            gy++;
            double gpu_mem_pct = gpu.mem_total_mb > 0 ? (double)gpu.mem_used_mb / gpu.mem_total_mb * 100.0 : 0;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, gy, px + 3, "VRAM "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            draw_bar(stdscr, gy, px + 8, gbw, gpu_mem_pct, color_for_pct(gpu_mem_pct));
            wattron(stdscr, COLOR_PAIR(color_for_pct(gpu_mem_pct)) | A_BOLD); wprintw(stdscr, " %3d%%", gpu.mem_util); wattroff(stdscr, A_BOLD);
            gy += 2;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, gy, px + 3, "Mem: "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            wattron(stdscr, A_BOLD); wprintw(stdscr, "%d", gpu.mem_used_mb); wattroff(stdscr, A_BOLD);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " / %d MB", gpu.mem_total_mb); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            gy++;
            int tc = color_for_pct(gpu.temp > 40 ? gpu.temp : 0);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, gy, px + 3, "Temp: "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            wattron(stdscr, COLOR_PAIR(tc) | A_BOLD); wprintw(stdscr, "%d\u00b0C", gpu.temp); wattroff(stdscr, COLOR_PAIR(tc) | A_BOLD);
            if (gpu.fan_pct >= 0) {
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, "  Fan: "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                wprintw(stdscr, "%d%%", gpu.fan_pct);
            }
            if (gpu.power_w > 0) {
                gy++;
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, gy, px + 3, "Power: "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                wprintw(stdscr, "%dW / %dW", gpu.power_w, gpu.power_max_w);
            }
        }

        /* ── Bottom row ── */
        int bot_y = by + top_h;
        int ncols_bot = 3 + has_docker;
        int bcol_w = cols / ncols_bot;
        int blast_w = cols - bcol_w * (ncols_bot - 1);

        /* ── Processes ── */
        {
            int pw = bcol_w;
            draw_box(stdscr, bot_y, 0, bot_h, pw, CLR_GREEN, "PROCESSES [c/m/p]");
            int py = bot_y + 1;
            const char *sort_labels[] = {"CPU%", "MEM%", "PID"};
            wattron(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
            mvwprintw(stdscr, py, 3, "%-7s %-16s %7s %7s", "PID", "PROCESS", "CPU%", "MEM%");
            wattroff(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
            py++;
            int max_show = bot_h - 4;
            if (max_show > 20) max_show = 20;
            for (int i = 0; i < max_show && i < nprocs && py < bot_y + bot_h - 1; i++) {
                if (procs[i].cpu_pct < 0.05 && procs[i].mem_pct < 0.05) continue;
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, py, 3, "%-7d", procs[i].pid); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                mvwprintw(stdscr, py, 11, "%-16.16s", procs[i].name);
                int cc = color_for_pct(procs[i].cpu_pct);
                wattron(stdscr, COLOR_PAIR(cc)); wprintw(stdscr, " %6.1f%%", procs[i].cpu_pct); wattroff(stdscr, COLOR_PAIR(cc));
                int mc = color_for_pct(procs[i].mem_pct * 2);
                wattron(stdscr, COLOR_PAIR(mc)); wprintw(stdscr, " %6.1f%%", procs[i].mem_pct); wattroff(stdscr, COLOR_PAIR(mc));

                int mini = (int)(procs[i].cpu_pct / 10);
                if (mini > 8) mini = 8;
                wprintw(stdscr, " ");
                wattron(stdscr, COLOR_PAIR(cc));
                for (int b = 0; b < mini; b++) wprintw(stdscr, "\u2588");
                wattroff(stdscr, COLOR_PAIR(cc));
                py++;
            }
            (void)sort_labels;
            wattron(stdscr, COLOR_PAIR(CLR_DIM));
            mvwprintw(stdscr, bot_y + bot_h - 2, 3, "%d processes", nprocs);
            wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        }

        /* ── Network (per-interface) ── */
        {
            int px = bcol_w;
            int pw = bcol_w;
            draw_box(stdscr, bot_y, px, bot_h, pw, CLR_BLUE, "NETWORK");
            int ny = bot_y + 2;
            char sb[32], tb[32];

            wattron(stdscr, COLOR_PAIR(CLR_GREEN)); mvwprintw(stdscr, ny, px + 3, "\u25b2 UP  "); wattroff(stdscr, COLOR_PAIR(CLR_GREEN));
            fmt_speed(sb, sizeof(sb), total_tx_speed);
            unsigned long long total_tx = 0;
            for (int i = 0; i < num_ifaces; i++) total_tx += ifaces[i].tx;
            fmt_bytes(tb, sizeof(tb), (double)total_tx);
            wprintw(stdscr, "%12s", sb);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, "  %s", tb); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            ny++;

            wattron(stdscr, COLOR_PAIR(CLR_BLUE)); mvwprintw(stdscr, ny, px + 3, "\u25bc DN  "); wattroff(stdscr, COLOR_PAIR(CLR_BLUE));
            fmt_speed(sb, sizeof(sb), total_rx_speed);
            unsigned long long total_rx = 0;
            for (int i = 0; i < num_ifaces; i++) total_rx += ifaces[i].rx;
            fmt_bytes(tb, sizeof(tb), (double)total_rx);
            wprintw(stdscr, "%12s", sb);
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, "  %s", tb); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            ny += 2;

            int nsw = pw - 14;
            if (nsw > HISTORY_LEN) nsw = HISTORY_LEN;
            if (nsw < 8) nsw = 8;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, ny, px + 3, "Up   "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            draw_sparkline(stdscr, ny, px + 8, net_tx_hist, net_hist_len, net_hist_pos, HISTORY_LEN, nsw);
            ny++;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, ny, px + 3, "Down "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            draw_sparkline(stdscr, ny, px + 8, net_rx_hist, net_hist_len, net_hist_pos, HISTORY_LEN, nsw);
            ny += 2;

            if (num_ifaces > 1 && ny < bot_y + bot_h - 2) {
                wattron(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
                mvwprintw(stdscr, ny, px + 3, "%-10s %10s %10s", "iface", "RX", "TX");
                wattroff(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
                ny++;
                for (int i = 0; i < num_ifaces && ny < bot_y + bot_h - 1; i++) {
                    char rxs[16], txs[16];
                    fmt_speed(rxs, 16, ifaces[i].rx_speed);
                    fmt_speed(txs, 16, ifaces[i].tx_speed);
                    wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, ny, px + 3, "%-10.10s", ifaces[i].name); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                    wprintw(stdscr, " %10s %10s", rxs, txs);
                    ny++;
                }
            }
        }

        /* ── Disk + I/O ── */
        {
            int px = bcol_w * 2;
            int pw = has_docker ? bcol_w : blast_w;
            draw_box(stdscr, bot_y, px, bot_h, pw, CLR_YELLOW, "DISK");
            int dy = bot_y + 2;
            int dbw = pw - 26;
            if (dbw < 6) dbw = 6; if (dbw > 25) dbw = 25;

            FILE *mf = fopen("/proc/mounts", "r");
            if (mf) {
                char mline[512];
                while (fgets(mline, sizeof(mline), mf) && dy < bot_y + bot_h - 5) {
                    char dev[128], mount[128];
                    sscanf(mline, "%127s %127s", dev, mount);
                    if (strncmp(dev, "/dev/", 5) != 0 || strstr(dev, "loop") || strstr(mount, "/snap")) continue;
                    struct statvfs st;
                    if (statvfs(mount, &st) != 0) continue;
                    double tot = (double)st.f_blocks * st.f_frsize;
                    double used = tot - (double)st.f_bfree * st.f_frsize;
                    double pct = (tot > 0) ? used / tot * 100.0 : 0;
                    const char *label = mount;
                    if (strcmp(mount, "/") == 0) label = "/";
                    else if (strstr(mount, "home")) label = "~";
                    else if (strstr(mount, "boot")) label = "boot";

                    wattron(stdscr, A_BOLD); mvwprintw(stdscr, dy, px + 3, "%-6.6s", label); wattroff(stdscr, A_BOLD);
                    draw_bar(stdscr, dy, px + 10, dbw, pct, color_for_pct(pct));
                    char ub[16], tbb[16];
                    fmt_bytes(ub, 16, used); fmt_bytes(tbb, 16, tot);
                    wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " %s/%s", ub, tbb); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                    dy++;
                }
                fclose(mf);
            }
            dy++;
            char rs[16], ws[16];
            fmt_speed(rs, 16, disk_io.read_speed);
            fmt_speed(ws, 16, disk_io.write_speed);
            wattron(stdscr, COLOR_PAIR(CLR_GREEN)); mvwprintw(stdscr, dy, px + 3, "\u25b2 Write "); wattroff(stdscr, COLOR_PAIR(CLR_GREEN));
            wprintw(stdscr, "%s", ws);
            dy++;
            wattron(stdscr, COLOR_PAIR(CLR_BLUE)); mvwprintw(stdscr, dy, px + 3, "\u25bc Read  "); wattroff(stdscr, COLOR_PAIR(CLR_BLUE));
            wprintw(stdscr, "%s", rs);
            dy++;
            int dsw = pw - 12;
            if (dsw > HISTORY_LEN) dsw = HISTORY_LEN;
            if (dsw < 8) dsw = 8;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, dy, px + 3, "W "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            draw_sparkline(stdscr, dy, px + 5, disk_io.write_hist, disk_io.hist_len, disk_io.hist_pos, HISTORY_LEN, dsw);
            dy++;
            wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, dy, px + 3, "R "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            draw_sparkline(stdscr, dy, px + 5, disk_io.read_hist, disk_io.hist_len, disk_io.hist_pos, HISTORY_LEN, dsw);
        }

        /* ── Docker panel (if present) ── */
        if (has_docker) {
            int px = bcol_w * 3;
            int pw = blast_w;
            draw_box(stdscr, bot_y, px, bot_h, pw, CLR_CYAN, "DOCKER");
            int dky = bot_y + 2;
            wattron(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
            mvwprintw(stdscr, dky, px + 3, "%-18s %7s %8s %s", "CONTAINER", "CPU%", "MEM", "STATUS");
            wattroff(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
            dky++;
            for (int i = 0; i < cached_docker_count && dky < bot_y + bot_h - 1; i++) {
                mvwprintw(stdscr, dky, px + 3, "%-18.18s", cached_docker[i].name);
                int cc = color_for_pct(cached_docker[i].cpu_pct);
                wattron(stdscr, COLOR_PAIR(cc)); wprintw(stdscr, " %6.1f%%", cached_docker[i].cpu_pct); wattroff(stdscr, COLOR_PAIR(cc));
                wprintw(stdscr, " %6.0fMB", cached_docker[i].mem_mb);
                wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " %s", cached_docker[i].status); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                dky++;
            }
        }

        refresh();
        usleep(REFRESH_MS * 1000);
    }

    endwin();
    return 0;
}

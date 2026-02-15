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

#define MAX_CORES 64
#define MAX_PROCS 256
#define HISTORY_LEN 60
#define REFRESH_MS 1000
#define BAR_CHAR_FULL "\u2501"
#define BAR_CHAR_DIM  "\u2500"

#define BLOCKS "\u2581\u2582\u2583\u2584\u2585\u2586\u2587\u2588"

static const char *SPARK_BLOCKS[] = {
    "\u2581", "\u2582", "\u2583", "\u2584",
    "\u2585", "\u2586", "\u2587", "\u2588"
};

enum { CLR_GREEN = 1, CLR_YELLOW, CLR_RED, CLR_CYAN, CLR_MAGENTA, CLR_BLUE, CLR_DIM, CLR_HEADER };

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total, busy;
} cpu_stat_t;

typedef struct {
    int pid;
    char name[64];
    double cpu_pct;
    double mem_pct;
} proc_info_t;

static cpu_stat_t prev_cpu[MAX_CORES + 1];
static int num_cores = 0;
static double cpu_history[HISTORY_LEN];
static int hist_len = 0;
static int hist_pos = 0;
static unsigned long long prev_net_rx = 0, prev_net_tx = 0;
static double net_rx_speed = 0, net_tx_speed = 0;
static double net_rx_hist[HISTORY_LEN], net_tx_hist[HISTORY_LEN];
static int net_hist_len = 0, net_hist_pos = 0;

static int color_for_pct(double pct) {
    if (pct < 50.0) return CLR_GREEN;
    if (pct < 80.0) return CLR_YELLOW;
    return CLR_RED;
}

static void draw_bar(WINDOW *w, int y, int x, int width, double pct, int color) {
    int filled = (int)(pct / 100.0 * width);
    if (filled > width) filled = width;
    int empty = width - filled;

    wmove(w, y, x);
    wattron(w, COLOR_PAIR(color) | A_BOLD);
    for (int i = 0; i < filled; i++) wprintw(w, BAR_CHAR_FULL);
    wattroff(w, A_BOLD);
    wattron(w, COLOR_PAIR(CLR_DIM));
    for (int i = 0; i < empty; i++) wprintw(w, BAR_CHAR_DIM);
    wattroff(w, COLOR_PAIR(CLR_DIM) | COLOR_PAIR(color));
}

static void draw_sparkline(WINDOW *w, int y, int x, double *data, int len, int pos, int total, int width) {
    wmove(w, y, x);
    if (len < width) {
        wattron(w, COLOR_PAIR(CLR_DIM));
        for (int i = 0; i < width - len; i++) wprintw(w, BAR_CHAR_DIM);
        wattroff(w, COLOR_PAIR(CLR_DIM));
    }

    double mn = 1e18, mx = -1e18;
    for (int i = 0; i < len && i < width; i++) {
        int idx = (pos - len + i + total) % total;
        if (data[idx] < mn) mn = data[idx];
        if (data[idx] > mx) mx = data[idx];
    }
    double rng = (mx - mn > 0.001) ? mx - mn : 1.0;

    int count = (len < width) ? len : width;
    for (int i = 0; i < count; i++) {
        int idx = (pos - count + i + total) % total;
        int si = (int)((data[idx] - mn) / rng * 7);
        if (si < 0) si = 0;
        if (si > 7) si = 7;
        double pct_val = (mx <= 100.0) ? data[idx] : (data[idx] - mn) / rng * 100.0;
        wattron(w, COLOR_PAIR(color_for_pct(pct_val)));
        wprintw(w, "%s", SPARK_BLOCKS[si]);
        wattroff(w, COLOR_PAIR(color_for_pct(pct_val)));
    }
}

static void read_cpu_stats(cpu_stat_t *stats, int *count) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[512];
    *count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        cpu_stat_t *s = &stats[*count];
        if (line[3] == ' ') {
            sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &s->user, &s->nice, &s->system, &s->idle,
                   &s->iowait, &s->irq, &s->softirq, &s->steal);
        } else {
            sscanf(line + 3, "%*d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &s->user, &s->nice, &s->system, &s->idle,
                   &s->iowait, &s->irq, &s->softirq, &s->steal);
        }
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
                     unsigned long *swap_total, unsigned long *swap_free) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    unsigned long mem_free = 0;
    *total = *avail = *buffers = *cached = *swap_total = *swap_free = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line + 9, "%lu", total);
        else if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line + 13, "%lu", avail);
        else if (strncmp(line, "MemFree:", 8) == 0) sscanf(line + 8, "%lu", &mem_free);
        else if (strncmp(line, "Buffers:", 8) == 0) sscanf(line + 8, "%lu", buffers);
        else if (strncmp(line, "Cached:", 7) == 0) sscanf(line + 7, "%lu", cached);
        else if (strncmp(line, "SwapTotal:", 10) == 0) sscanf(line + 10, "%lu", swap_total);
        else if (strncmp(line, "SwapFree:", 9) == 0) sscanf(line + 9, "%lu", swap_free);
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
            snprintf(path, sizeof(path), "%s/temp%d_input", base, i);
            FILE *f = fopen(path, "r");
            if (!f) break;
            if (fgets(buf, sizeof(buf), f)) temps[count] = atof(buf) / 1000.0;
            fclose(f);

            snprintf(path, sizeof(path), "%s/temp%d_label", base, i);
            f = fopen(path, "r");
            if (f) {
                if (fgets(buf, sizeof(buf), f)) {
                    buf[strcspn(buf, "\n")] = 0;
                    snprintf(labels[count], 32, "%s", buf);
                }
                fclose(f);
            } else {
                snprintf(path, sizeof(path), "%s/name", base);
                f = fopen(path, "r");
                if (f) {
                    if (fgets(buf, sizeof(buf), f)) {
                        buf[strcspn(buf, "\n")] = 0;
                        snprintf(labels[count], 32, "%.24s #%d", buf, i);
                    }
                    fclose(f);
                } else {
                    snprintf(labels[count], 31, "sensor%d", count);
                }
            }

            highs[count] = 0;
            snprintf(path, sizeof(path), "%s/temp%d_max", base, i);
            f = fopen(path, "r");
            if (f) { if (fgets(buf, sizeof(buf), f)) highs[count] = atof(buf) / 1000.0; fclose(f); }

            crits[count] = 0;
            snprintf(path, sizeof(path), "%s/temp%d_crit", base, i);
            f = fopen(path, "r");
            if (f) { if (fgets(buf, sizeof(buf), f)) crits[count] = atof(buf) / 1000.0; fclose(f); }

            count++;
        }
    }
    closedir(hwmon);
    return count;
}

static void read_net(unsigned long long *rx, unsigned long long *tx) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    *rx = *tx = 0;
    (void)fgets(line, sizeof(line), f);
    (void)fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char iface[64] = {0};
        int len = colon - line;
        strncpy(iface, line, len);
        iface[len] = 0;
        char *p = iface;
        while (*p == ' ') p++;
        if (strcmp(p, "lo") == 0) continue;
        unsigned long long r, t;
        sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t);
        *rx += r;
        *tx += t;
    }
    fclose(f);
}

static int read_procs(proc_info_t *procs, int max, unsigned long mem_total_kb) {
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(proc_dir)) && count < max) {
        if (!isdigit(de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        char path[256], line[512];

        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(line, sizeof(line), f)) { fclose(f); continue; }
        fclose(f);

        char *name_start = strchr(line, '(');
        char *name_end = strrchr(line, ')');
        if (!name_start || !name_end) continue;

        procs[count].pid = pid;
        int nlen = name_end - name_start - 1;
        if (nlen > 63) nlen = 63;
        strncpy(procs[count].name, name_start + 1, nlen);
        procs[count].name[nlen] = 0;

        unsigned long rss = 0;
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        f = fopen(path, "r");
        if (f) {
            unsigned long size;
            (void)fscanf(f, "%lu %lu", &size, &rss);
            fclose(f);
        }
        long page_size = sysconf(_SC_PAGESIZE);
        double mem_kb = (double)rss * page_size / 1024.0;
        procs[count].mem_pct = (mem_total_kb > 0) ? mem_kb / mem_total_kb * 100.0 : 0;
        procs[count].cpu_pct = 0;
        count++;
    }
    closedir(proc_dir);
    return count;
}

static int proc_cmp(const void *a, const void *b) {
    const proc_info_t *pa = a, *pb = b;
    if (pb->mem_pct > pa->mem_pct) return 1;
    if (pb->mem_pct < pa->mem_pct) return -1;
    return 0;
}

static void fmt_bytes(char *buf, size_t sz, double bytes) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (bytes >= 1024.0 && u < 4) { bytes /= 1024.0; u++; }
    snprintf(buf, sz, "%.1f %s", bytes, units[u]);
}

static void fmt_speed(char *buf, size_t sz, double bps) {
    const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int u = 0;
    while (bps >= 1024.0 && u < 3) { bps /= 1024.0; u++; }
    snprintf(buf, sz, "%.1f %s", bps, units[u]);
}

static void draw_box(WINDOW *w, int y, int x, int h, int width, int color, const char *title) {
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

static void draw_header(WINDOW *w, int width, double cpu_avg, double mem_pct) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%a %b %d  %H:%M:%S", tm);

    struct sysinfo si;
    sysinfo(&si);
    int days = si.uptime / 86400;
    int hours = (si.uptime % 86400) / 3600;
    int mins = (si.uptime % 3600) / 60;

    wattron(w, COLOR_PAIR(CLR_HEADER) | A_BOLD);
    mvwhline(w, 0, 0, ' ', width);
    mvwprintw(w, 0, 2, " CUTEDASH ");
    wattroff(w, A_BOLD);
    wattron(w, COLOR_PAIR(CLR_DIM));
    wprintw(w, " | ");
    wattroff(w, COLOR_PAIR(CLR_DIM));
    wattron(w, COLOR_PAIR(CLR_HEADER));
    wprintw(w, "%s", timebuf);
    wattroff(w, COLOR_PAIR(CLR_HEADER));

    wattron(w, COLOR_PAIR(CLR_DIM));
    wprintw(w, "  |  ");
    wattroff(w, COLOR_PAIR(CLR_DIM));
    wprintw(w, "up %dd %dh %dm", days, hours, mins);

    wattron(w, COLOR_PAIR(CLR_DIM));
    wprintw(w, "  |  ");
    wattroff(w, COLOR_PAIR(CLR_DIM));
    wprintw(w, "CPU ");
    wattron(w, COLOR_PAIR(color_for_pct(cpu_avg)) | A_BOLD);
    wprintw(w, "%.0f%%", cpu_avg);
    wattroff(w, COLOR_PAIR(color_for_pct(cpu_avg)) | A_BOLD);
    wprintw(w, "  MEM ");
    wattron(w, COLOR_PAIR(color_for_pct(mem_pct)) | A_BOLD);
    wprintw(w, "%.0f%%", mem_pct);
    wattroff(w, COLOR_PAIR(color_for_pct(mem_pct)) | A_BOLD);

    wattron(w, COLOR_PAIR(CLR_DIM));
    mvwprintw(w, 0, width - 18, " q to exit ");
    wattroff(w, COLOR_PAIR(CLR_DIM) | COLOR_PAIR(CLR_HEADER));
}

int main(void) {
    setlocale(LC_ALL, "");

    read_cpu_stats(prev_cpu, &num_cores);
    num_cores--;

    unsigned long long init_rx, init_tx;
    read_net(&init_rx, &init_tx);
    prev_net_rx = init_rx;
    prev_net_tx = init_tx;

    usleep(200000);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    start_color();
    use_default_colors();

    init_pair(CLR_GREEN, COLOR_GREEN, -1);
    init_pair(CLR_YELLOW, COLOR_YELLOW, -1);
    init_pair(CLR_RED, COLOR_RED, -1);
    init_pair(CLR_CYAN, COLOR_CYAN, -1);
    init_pair(CLR_MAGENTA, COLOR_MAGENTA, -1);
    init_pair(CLR_BLUE, COLOR_BLUE, -1);
    init_pair(CLR_DIM, 8, -1);
    init_pair(CLR_HEADER, COLOR_CYAN, -1);

    while (1) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();

        cpu_stat_t cur_cpu[MAX_CORES + 1];
        int cur_count;
        read_cpu_stats(cur_cpu, &cur_count);

        double core_pcts[MAX_CORES];
        double cpu_avg = calc_cpu_pct(&cur_cpu[0], &prev_cpu[0]);
        for (int i = 0; i < num_cores; i++)
            core_pcts[i] = calc_cpu_pct(&cur_cpu[i + 1], &prev_cpu[i + 1]);
        memcpy(prev_cpu, cur_cpu, sizeof(prev_cpu));

        cpu_history[hist_pos] = cpu_avg;
        hist_pos = (hist_pos + 1) % HISTORY_LEN;
        if (hist_len < HISTORY_LEN) hist_len++;

        unsigned long mem_total = 0, mem_avail = 0, mem_used = 0, mem_buf = 0, mem_cached = 0, sw_total = 0, sw_free = 0;
        read_mem(&mem_total, &mem_avail, &mem_used, &mem_buf, &mem_cached, &sw_total, &sw_free);
        double mem_pct = (mem_total > 0) ? (double)mem_used / mem_total * 100.0 : 0;

        draw_header(stdscr, cols, cpu_avg, mem_pct);

        int top_h = (rows - 2) * 3 / 5;
        int bot_h = rows - 2 - top_h;
        int left_w = cols * 3 / 7;
        int mid_w = cols * 2 / 7;
        int right_w = cols - left_w - mid_w;

        int by = 2;

        // === CPU panel ===
        int cpu_h = top_h;
        draw_box(stdscr, by, 0, cpu_h, left_w, CLR_CYAN, "CPU");

        int bar_w = left_w / 2 - 12;
        if (bar_w < 10) bar_w = 10;
        if (bar_w > 30) bar_w = 30;

        int cy = by + 2;
        for (int i = 0; i < num_cores && cy < by + cpu_h - 5; i += 2) {
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
        int avg_color = color_for_pct(cpu_avg);
        wattron(stdscr, A_BOLD);
        mvwprintw(stdscr, cy, 3, "AVG");
        wattroff(stdscr, A_BOLD);
        draw_bar(stdscr, cy, 7, bar_w, cpu_avg, avg_color);
        wattron(stdscr, COLOR_PAIR(avg_color) | A_BOLD);
        wprintw(stdscr, " %5.1f%%", cpu_avg);
        wattroff(stdscr, COLOR_PAIR(avg_color) | A_BOLD);

        int spark_w = left_w - 10;
        if (spark_w > HISTORY_LEN) spark_w = HISTORY_LEN;
        if (spark_w < 10) spark_w = 10;
        cy++;
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        mvwprintw(stdscr, cy, 3, "      ");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        draw_sparkline(stdscr, cy, 7, cpu_history, hist_len, hist_pos, HISTORY_LEN, spark_w);

        cy++;
        double load1, load5, load15;
        {
            FILE *f = fopen("/proc/loadavg", "r");
            if (f) { (void)fscanf(f, "%lf %lf %lf", &load1, &load5, &load15); fclose(f); }
        }
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        mvwprintw(stdscr, cy, 3, "Load:");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        wattron(stdscr, COLOR_PAIR(color_for_pct(load1 / num_cores * 100)));
        wprintw(stdscr, " %.2f", load1);
        wattroff(stdscr, COLOR_PAIR(color_for_pct(load1 / num_cores * 100)));
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, " / %.2f / %.2f  Cores: %d", load5, load15, num_cores);
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        // === Memory panel ===
        draw_box(stdscr, by, left_w, top_h, mid_w, CLR_MAGENTA, "MEMORY");
        int my = by + 2;
        int mem_bar_w = mid_w - 12;
        if (mem_bar_w < 8) mem_bar_w = 8;
        if (mem_bar_w > 35) mem_bar_w = 35;

        mvwprintw(stdscr, my, left_w + 3, "  ");
        draw_bar(stdscr, my, left_w + 3, mem_bar_w, mem_pct, color_for_pct(mem_pct));
        my += 2;
        wattron(stdscr, A_BOLD);
        mvwprintw(stdscr, my, left_w + 3, "%.1f", mem_used / 1024.0 / 1024.0);
        wattroff(stdscr, A_BOLD);
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, " GB used of ");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        wattron(stdscr, A_BOLD);
        wprintw(stdscr, "%.1f", mem_total / 1024.0 / 1024.0);
        wattroff(stdscr, A_BOLD);
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, " GB");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        my++;
        wattron(stdscr, COLOR_PAIR(CLR_GREEN));
        mvwprintw(stdscr, my, left_w + 3, "%.1f", mem_avail / 1024.0 / 1024.0);
        wattroff(stdscr, COLOR_PAIR(CLR_GREEN));
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, " GB available");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        my += 2;
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        mvwprintw(stdscr, my, left_w + 3, "Cached  ");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        wattron(stdscr, A_BOLD);
        wprintw(stdscr, "%.1f", mem_cached / 1024.0 / 1024.0);
        wattroff(stdscr, A_BOLD);
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, " GB");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        my++;
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        mvwprintw(stdscr, my, left_w + 3, "Buffers ");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        wattron(stdscr, A_BOLD);
        wprintw(stdscr, "%.1f", mem_buf / 1024.0 / 1024.0);
        wattroff(stdscr, A_BOLD);
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, " GB");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        if (sw_total > 0) {
            my += 2;
            double sw_used = (sw_total - sw_free) / 1024.0 / 1024.0;
            wattron(stdscr, COLOR_PAIR(CLR_DIM));
            mvwprintw(stdscr, my, left_w + 3, "Swap    ");
            wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            wprintw(stdscr, "%.1f / %.1f GB", sw_used, sw_total / 1024.0 / 1024.0);
        }

        // === Temps panel ===
        draw_box(stdscr, by, left_w + mid_w, top_h, right_w, CLR_RED, "TEMPS");
        char t_labels[32][32];
        double t_vals[32], t_highs[32], t_crits[32];
        int t_count = read_temps(t_labels, t_vals, t_highs, t_crits, 32);
        int ty = by + 2;
        int t_bar_w = right_w - 28;
        if (t_bar_w < 8) t_bar_w = 8;
        if (t_bar_w > 20) t_bar_w = 20;

        if (t_count == 0) {
            wattron(stdscr, COLOR_PAIR(CLR_DIM));
            mvwprintw(stdscr, ty, left_w + mid_w + 3, "No sensors detected");
            wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        }
        for (int i = 0; i < t_count && ty < by + top_h - 1; i++) {
            double t = t_vals[i];
            int tc = color_for_pct(t > 40 ? t : 0);
            mvwprintw(stdscr, ty, left_w + mid_w + 3, "%-14.14s", t_labels[i]);
            draw_bar(stdscr, ty, left_w + mid_w + 18, t_bar_w, t, tc);
            wattron(stdscr, COLOR_PAIR(tc) | A_BOLD);
            wprintw(stdscr, " %3.0f\u00b0C", t);
            wattroff(stdscr, COLOR_PAIR(tc) | A_BOLD);
            if (t_highs[i] > 0) {
                wattron(stdscr, COLOR_PAIR(CLR_DIM));
                wprintw(stdscr, " H:%.0f", t_highs[i]);
                wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            }
            ty++;
        }

        // === Processes panel (bottom left) ===
        int bot_y = by + top_h;
        draw_box(stdscr, bot_y, 0, bot_h, left_w, CLR_GREEN, "PROCESSES");

        proc_info_t procs[MAX_PROCS];
        int nprocs = read_procs(procs, MAX_PROCS, mem_total);
        qsort(procs, nprocs, sizeof(proc_info_t), proc_cmp);

        int py = bot_y + 1;
        wattron(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
        mvwprintw(stdscr, py, 3, "%-7s %-20s %8s %8s", "PID", "PROCESS", "MEM%", "");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
        py++;

        int max_show = bot_h - 4;
        if (max_show > 15) max_show = 15;
        for (int i = 0; i < max_show && i < nprocs && py < bot_y + bot_h - 1; i++) {
            wattron(stdscr, COLOR_PAIR(CLR_DIM));
            mvwprintw(stdscr, py, 3, "%-7d", procs[i].pid);
            wattroff(stdscr, COLOR_PAIR(CLR_DIM));
            mvwprintw(stdscr, py, 11, "%-20.20s", procs[i].name);

            int mc = color_for_pct(procs[i].mem_pct * 2);
            wattron(stdscr, COLOR_PAIR(mc));
            wprintw(stdscr, " %5.1f%%", procs[i].mem_pct);
            wattroff(stdscr, COLOR_PAIR(mc));

            int mini_w = (int)(procs[i].mem_pct / 2);
            if (mini_w > 10) mini_w = 10;
            wprintw(stdscr, " ");
            wattron(stdscr, COLOR_PAIR(mc));
            for (int b = 0; b < mini_w; b++) wprintw(stdscr, "\u2588");
            wattroff(stdscr, COLOR_PAIR(mc));

            py++;
        }

        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        mvwprintw(stdscr, bot_y + bot_h - 2, 3, "%d active processes", nprocs);
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        // === Network panel (bottom mid) ===
        draw_box(stdscr, bot_y, left_w, bot_h, mid_w, CLR_BLUE, "NETWORK");

        unsigned long long net_rx, net_tx;
        read_net(&net_rx, &net_tx);
        net_rx_speed = (double)(net_rx - prev_net_rx) / (REFRESH_MS / 1000.0);
        net_tx_speed = (double)(net_tx - prev_net_tx) / (REFRESH_MS / 1000.0);
        prev_net_rx = net_rx;
        prev_net_tx = net_tx;

        net_rx_hist[net_hist_pos] = net_rx_speed;
        net_tx_hist[net_hist_pos] = net_tx_speed;
        net_hist_pos = (net_hist_pos + 1) % HISTORY_LEN;
        if (net_hist_len < HISTORY_LEN) net_hist_len++;

        int ny = bot_y + 2;
        char spd_buf[32], tot_buf[32];

        wattron(stdscr, COLOR_PAIR(CLR_GREEN));
        mvwprintw(stdscr, ny, left_w + 3, "\u25b2 UP  ");
        wattroff(stdscr, COLOR_PAIR(CLR_GREEN));
        fmt_speed(spd_buf, sizeof(spd_buf), net_tx_speed);
        fmt_bytes(tot_buf, sizeof(tot_buf), (double)net_tx);
        wprintw(stdscr, "%12s", spd_buf);
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, "  total: %s", tot_buf);
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        ny++;
        wattron(stdscr, COLOR_PAIR(CLR_BLUE));
        mvwprintw(stdscr, ny, left_w + 3, "\u25bc DN  ");
        wattroff(stdscr, COLOR_PAIR(CLR_BLUE));
        fmt_speed(spd_buf, sizeof(spd_buf), net_rx_speed);
        fmt_bytes(tot_buf, sizeof(tot_buf), (double)net_rx);
        wprintw(stdscr, "%12s", spd_buf);
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        wprintw(stdscr, "  total: %s", tot_buf);
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));

        ny += 2;
        int net_spark_w = mid_w - 14;
        if (net_spark_w > HISTORY_LEN) net_spark_w = HISTORY_LEN;
        if (net_spark_w < 8) net_spark_w = 8;
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        mvwprintw(stdscr, ny, left_w + 3, "Upload  ");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        draw_sparkline(stdscr, ny, left_w + 11, net_tx_hist, net_hist_len, net_hist_pos, HISTORY_LEN, net_spark_w);

        ny++;
        wattron(stdscr, COLOR_PAIR(CLR_DIM));
        mvwprintw(stdscr, ny, left_w + 3, "Download");
        wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        draw_sparkline(stdscr, ny, left_w + 11, net_rx_hist, net_hist_len, net_hist_pos, HISTORY_LEN, net_spark_w);

        // === Disk panel (bottom right) ===
        draw_box(stdscr, bot_y, left_w + mid_w, bot_h, right_w, CLR_YELLOW, "DISK");
        int dy = bot_y + 2;
        int d_bar_w = right_w - 26;
        if (d_bar_w < 8) d_bar_w = 8;
        if (d_bar_w > 25) d_bar_w = 25;

        FILE *mf = fopen("/proc/mounts", "r");
        if (mf) {
            char mline[512];
            while (fgets(mline, sizeof(mline), mf) && dy < bot_y + bot_h - 3) {
                char dev[128], mount[128], fstype[64];
                sscanf(mline, "%127s %127s %63s", dev, mount, fstype);
                if (strncmp(dev, "/dev/", 5) != 0) continue;
                if (strstr(dev, "loop")) continue;
                if (strstr(mount, "/snap")) continue;

                struct statvfs st;
                if (statvfs(mount, &st) != 0) continue;
                double total_b = (double)st.f_blocks * st.f_frsize;
                double free_b = (double)st.f_bfree * st.f_frsize;
                double used_b = total_b - free_b;
                double pct = (total_b > 0) ? used_b / total_b * 100.0 : 0;

                const char *label = mount;
                if (strcmp(mount, "/") == 0) label = "/";
                else if (strstr(mount, "home")) label = "~";
                else if (strstr(mount, "boot")) label = "boot";

                wattron(stdscr, A_BOLD);
                mvwprintw(stdscr, dy, left_w + mid_w + 3, "%-6.6s", label);
                wattroff(stdscr, A_BOLD);
                draw_bar(stdscr, dy, left_w + mid_w + 10, d_bar_w, pct, color_for_pct(pct));

                char u_buf[16], t_buf[16];
                fmt_bytes(u_buf, sizeof(u_buf), used_b);
                fmt_bytes(t_buf, sizeof(t_buf), total_b);
                wattron(stdscr, COLOR_PAIR(CLR_DIM));
                wprintw(stdscr, " %s/%s", u_buf, t_buf);
                wattroff(stdscr, COLOR_PAIR(CLR_DIM));
                dy++;
            }
            fclose(mf);
        }

        refresh();
        usleep(REFRESH_MS * 1000);
    }

    endwin();
    return 0;
}

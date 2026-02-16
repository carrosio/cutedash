#include "cutedash.h"

int g_theme = THEME_DEFAULT;
int g_sort = SORT_CPU;
int g_once = 0;
int g_alert_cpu = 90;
int g_alert_temp = 85;
int g_alert_flash = 0;
volatile int g_resize = 0;

cpu_stat_t prev_cpu[MAX_CORES + 1];
int num_cores = 0;
double cpu_history[HISTORY_LEN];
int cpu_hist_len = 0, cpu_hist_pos = 0;

iface_t ifaces[MAX_IFACES];
int num_ifaces = 0;
double net_rx_hist[HISTORY_LEN], net_tx_hist[HISTORY_LEN];
int net_hist_len = 0, net_hist_pos = 0;

disk_io_t disk_io = {0};

proc_info_t prev_procs[MAX_PROCS];
int prev_nprocs = 0;

static void handle_resize(int sig) { (void)sig; g_resize = 1; }

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

        draw_header(stdscr, cols, cpu_avg, mem_pct, alert);

        int has_gpu = gpu.has_gpu;
        int has_docker = (cached_docker_count > 0);

        int top_h = (rows - 2) * 3 / 5;
        int bot_h = rows - 2 - top_h;

        int ncols_top = 3 + has_gpu;
        int col_w = cols / ncols_top;
        int last_col_w = cols - col_w * (ncols_top - 1);

        int by = 2;

        draw_cpu_panel(by, top_h, col_w, core_pcts, cpu_avg);
        draw_memory_panel(by, top_h, col_w, col_w, mem_total, mem_avail, mem_used, mem_buf, mem_cached, sw_total, sw_free, bat);
        draw_temps_panel(by, top_h, col_w * 2, has_gpu ? col_w : last_col_w, t_labels, t_vals, t_highs, t_count, fans, fan_count);
        if (has_gpu) draw_gpu_panel(by, top_h, col_w * 3, last_col_w, gpu);

        int bot_y = by + top_h;
        int ncols_bot = 3 + has_docker;
        int bcol_w = cols / ncols_bot;
        int blast_w = cols - bcol_w * (ncols_bot - 1);

        draw_processes_panel(bot_y, bot_h, bcol_w, procs, nprocs);
        draw_network_panel(bot_y, bot_h, bcol_w, bcol_w, total_rx_speed, total_tx_speed);
        draw_disk_panel(bot_y, bot_h, bcol_w * 2, has_docker ? bcol_w : blast_w, has_docker);
        if (has_docker) draw_docker_panel(bot_y, bot_h, bcol_w * 3, blast_w, cached_docker, cached_docker_count);

        refresh();
        usleep(REFRESH_MS * 1000);
    }

    endwin();
    return 0;
}

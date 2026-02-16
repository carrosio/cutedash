#include "cutedash.h"

void draw_cpu_panel(int by, int top_h, int pw, double *core_pcts, double cpu_avg) {
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

void draw_memory_panel(int by, int top_h, int px, int pw,
                       unsigned long mem_total, unsigned long mem_avail, unsigned long mem_used,
                       unsigned long mem_buf, unsigned long mem_cached,
                       unsigned long sw_total, unsigned long sw_free, battery_t bat) {
    double mem_pct = (mem_total > 0) ? (double)mem_used / mem_total * 100.0 : 0;
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
    if (bat.present) {
        my += 2;
        int bc = bat.capacity > 50 ? CLR_GREEN : bat.capacity > 20 ? CLR_YELLOW : CLR_RED;
        wattron(stdscr, COLOR_PAIR(CLR_DIM)); mvwprintw(stdscr, my, px + 3, "Battery "); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        wattron(stdscr, COLOR_PAIR(bc) | A_BOLD); wprintw(stdscr, "%d%%", bat.capacity); wattroff(stdscr, COLOR_PAIR(bc) | A_BOLD);
        wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " %s", bat.status); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
    }
}

void draw_temps_panel(int by, int top_h, int px, int pw,
                      char t_labels[][32], double *t_vals, double *t_highs, int t_count,
                      fan_info_t *fans, int fan_count) {
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

void draw_gpu_panel(int by, int top_h, int px, int pw, gpu_info_t gpu) {
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

void draw_processes_panel(int bot_y, int bot_h, int pw,
                          proc_info_t *procs, int nprocs) {
    draw_box(stdscr, bot_y, 0, bot_h, pw, CLR_GREEN, "PROCESSES [c/m/p]");
    int py = bot_y + 1;
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
    wattron(stdscr, COLOR_PAIR(CLR_DIM));
    mvwprintw(stdscr, bot_y + bot_h - 2, 3, "%d processes", nprocs);
    wattroff(stdscr, COLOR_PAIR(CLR_DIM));
}

void draw_network_panel(int bot_y, int bot_h, int px, int pw,
                        double total_rx_speed, double total_tx_speed) {
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

void draw_disk_panel(int bot_y, int bot_h, int px, int pw, int has_docker) {
    (void)has_docker;
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

void draw_docker_panel(int bot_y, int bot_h, int px, int pw,
                       docker_info_t *containers, int count) {
    draw_box(stdscr, bot_y, px, bot_h, pw, CLR_CYAN, "DOCKER");
    int dky = bot_y + 2;
    wattron(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
    mvwprintw(stdscr, dky, px + 3, "%-18s %7s %8s %s", "CONTAINER", "CPU%", "MEM", "STATUS");
    wattroff(stdscr, COLOR_PAIR(CLR_DIM) | A_BOLD);
    dky++;
    for (int i = 0; i < count && dky < bot_y + bot_h - 1; i++) {
        mvwprintw(stdscr, dky, px + 3, "%-18.18s", containers[i].name);
        int cc = color_for_pct(containers[i].cpu_pct);
        wattron(stdscr, COLOR_PAIR(cc)); wprintw(stdscr, " %6.1f%%", containers[i].cpu_pct); wattroff(stdscr, COLOR_PAIR(cc));
        wprintw(stdscr, " %6.0fMB", containers[i].mem_mb);
        wattron(stdscr, COLOR_PAIR(CLR_DIM)); wprintw(stdscr, " %s", containers[i].status); wattroff(stdscr, COLOR_PAIR(CLR_DIM));
        dky++;
    }
}

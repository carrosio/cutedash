#include "cutedash.h"

const char *SPARK[] = {
    "\u2581", "\u2582", "\u2583", "\u2584",
    "\u2585", "\u2586", "\u2587", "\u2588"
};

int color_for_pct(double pct) {
    if (pct < 50.0) return CLR_GREEN;
    if (pct < 80.0) return CLR_YELLOW;
    return CLR_RED;
}

void draw_bar(WINDOW *w, int y, int x, int width, double pct, int color) {
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

void draw_sparkline(WINDOW *w, int y, int x, double *data, int len, int pos, int total, int width) {
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

void fmt_bytes(char *buf, size_t sz, double b) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    snprintf(buf, sz, "%.1f %s", b, u[i]);
}

void fmt_speed(char *buf, size_t sz, double b) {
    const char *u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int i = 0;
    while (b >= 1024.0 && i < 3) { b /= 1024.0; i++; }
    snprintf(buf, sz, "%.1f %s", b, u[i]);
}

void draw_box(WINDOW *w, int y, int x, int h, int width, int color, const char *title) {
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

void setup_theme(void) {
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

void draw_header(WINDOW *w, int cols, double cpu_avg, double mem_pct, int alert) {
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

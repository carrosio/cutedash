#ifndef CUTEDASH_H
#define CUTEDASH_H

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

extern const char *SPARK[];

enum {
    CLR_GREEN = 1, CLR_YELLOW, CLR_RED, CLR_CYAN,
    CLR_MAGENTA, CLR_BLUE, CLR_DIM, CLR_HEADER,
    CLR_WHITE, CLR_ALERT
};

enum { THEME_DEFAULT = 0, THEME_NEON, THEME_LIGHT, THEME_COUNT };
enum { SORT_CPU = 0, SORT_MEM, SORT_PID };

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

extern int g_theme;
extern int g_sort;
extern int g_once;
extern int g_alert_cpu;
extern int g_alert_temp;
extern int g_alert_flash;
extern volatile int g_resize;

extern cpu_stat_t prev_cpu[MAX_CORES + 1];
extern int num_cores;
extern double cpu_history[HISTORY_LEN];
extern int cpu_hist_len, cpu_hist_pos;

extern iface_t ifaces[MAX_IFACES];
extern int num_ifaces;
extern double net_rx_hist[HISTORY_LEN], net_tx_hist[HISTORY_LEN];
extern int net_hist_len, net_hist_pos;

extern disk_io_t disk_io;

extern proc_info_t prev_procs[MAX_PROCS];
extern int prev_nprocs;

void read_cpu_stats(cpu_stat_t *stats, int *count);
double calc_cpu_pct(cpu_stat_t *cur, cpu_stat_t *prev);
void read_mem(unsigned long *total, unsigned long *avail, unsigned long *used,
              unsigned long *buffers, unsigned long *cached,
              unsigned long *sw_total, unsigned long *sw_free);
int read_temps(char labels[][32], double *temps, double *highs, double *crits, int max);
int read_fans(fan_info_t *fans, int max);
int read_ifaces(iface_t *ifs, int max);
void read_disk_io(disk_io_t *dio);
int read_procs_with_cpu(proc_info_t *procs, int max, unsigned long mem_total_kb,
                        proc_info_t *prev, int prev_count);
int proc_cmp_cpu(const void *a, const void *b);
int proc_cmp_mem(const void *a, const void *b);
int proc_cmp_pid(const void *a, const void *b);
battery_t read_battery(void);
gpu_info_t read_gpu(void);
int read_docker(docker_info_t *containers, int max);

void fmt_bytes(char *buf, size_t sz, double b);
void fmt_speed(char *buf, size_t sz, double b);

int color_for_pct(double pct);
void draw_bar(WINDOW *w, int y, int x, int width, double pct, int color);
void draw_sparkline(WINDOW *w, int y, int x, double *data, int len, int pos, int total, int width);
void draw_box(WINDOW *w, int y, int x, int h, int width, int color, const char *title);
void draw_header(WINDOW *w, int cols, double cpu_avg, double mem_pct, int alert);
void setup_theme(void);

void draw_cpu_panel(int by, int top_h, int pw, double *core_pcts, double cpu_avg);
void draw_memory_panel(int by, int top_h, int px, int pw,
                       unsigned long mem_total, unsigned long mem_avail, unsigned long mem_used,
                       unsigned long mem_buf, unsigned long mem_cached,
                       unsigned long sw_total, unsigned long sw_free, battery_t bat);
void draw_temps_panel(int by, int top_h, int px, int pw,
                      char t_labels[][32], double *t_vals, double *t_highs, int t_count,
                      fan_info_t *fans, int fan_count);
void draw_gpu_panel(int by, int top_h, int px, int pw, gpu_info_t gpu);
void draw_processes_panel(int bot_y, int bot_h, int pw,
                          proc_info_t *procs, int nprocs);
void draw_network_panel(int bot_y, int bot_h, int px, int pw,
                        double total_rx_speed, double total_tx_speed);
void draw_disk_panel(int bot_y, int bot_h, int px, int pw, int has_docker);
void draw_docker_panel(int bot_y, int bot_h, int px, int pw,
                       docker_info_t *containers, int count);

#endif

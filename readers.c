#include "cutedash.h"

void read_cpu_stats(cpu_stat_t *stats, int *count) {
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

double calc_cpu_pct(cpu_stat_t *cur, cpu_stat_t *prev) {
    unsigned long long dt = cur->total - prev->total;
    unsigned long long db = cur->busy - prev->busy;
    if (dt == 0) return 0.0;
    return (double)db / dt * 100.0;
}

void read_mem(unsigned long *total, unsigned long *avail, unsigned long *used,
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

int read_temps(char labels[][32], double *temps, double *highs, double *crits, int max) {
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

int read_fans(fan_info_t *fans, int max) {
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

int read_ifaces(iface_t *ifs, int max) {
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

void read_disk_io(disk_io_t *dio) {
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

int read_procs_with_cpu(proc_info_t *procs, int max, unsigned long mem_total_kb,
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

int proc_cmp_cpu(const void *a, const void *b) {
    double da = ((const proc_info_t *)a)->cpu_pct, db = ((const proc_info_t *)b)->cpu_pct;
    return (db > da) - (db < da);
}

int proc_cmp_mem(const void *a, const void *b) {
    double da = ((const proc_info_t *)a)->mem_pct, db = ((const proc_info_t *)b)->mem_pct;
    return (db > da) - (db < da);
}

int proc_cmp_pid(const void *a, const void *b) {
    return ((const proc_info_t *)b)->pid - ((const proc_info_t *)a)->pid;
}

battery_t read_battery(void) {
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

gpu_info_t read_gpu(void) {
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

int read_docker(docker_info_t *containers, int max) {
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

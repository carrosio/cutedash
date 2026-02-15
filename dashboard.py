import os
import time
from collections import deque
from datetime import datetime

import psutil
from rich.console import Console, Group
from rich.layout import Layout
from rich.live import Live
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

REFRESH_RATE = 1.0
SPARKLINE_LEN = 40

BLOCKS = " ▁▂▃▄▅▆▇█"
GRADIENT_GOOD = ["#22c55e", "#4ade80", "#86efac"]
GRADIENT_WARN = ["#eab308", "#facc15", "#fde047"]
GRADIENT_CRIT = ["#ef4444", "#f87171", "#fca5a5"]

cpu_history = deque(maxlen=SPARKLINE_LEN)
net_sent_prev = None
net_recv_prev = None
net_up_history = deque(maxlen=SPARKLINE_LEN)
net_down_history = deque(maxlen=SPARKLINE_LEN)


def sparkline(values, width=SPARKLINE_LEN):
    if not values:
        return "[dim]" + "─" * width + "[/]"
    mn, mx = min(values), max(values)
    rng = mx - mn if mx != mn else 1
    chars = []
    for v in values:
        idx = int((v - mn) / rng * (len(BLOCKS) - 1))
        pct = v if mx <= 100 else (v - mn) / rng * 100
        color = pick_color(pct)
        chars.append(f"[{color}]{BLOCKS[idx]}[/]")
    pad = width - len(values)
    return "[dim]" + "─" * pad + "[/]" + "".join(chars)


def pick_color(pct):
    if pct < 50:
        return GRADIENT_GOOD[0]
    if pct < 80:
        return GRADIENT_WARN[0]
    return GRADIENT_CRIT[0]


def make_bar(pct, width=30):
    filled = int(pct / 100 * width)
    empty = width - filled
    color = pick_color(pct)
    return f"[{color}]{'━' * filled}[dim #444444]{'─' * empty}[/] {pct:5.1f}%"


def fmt_bytes(b):
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if abs(b) < 1024:
            return f"{b:.1f} {unit}"
        b /= 1024
    return f"{b:.1f} PB"


def fmt_speed(bps):
    for unit in ["B/s", "KB/s", "MB/s", "GB/s"]:
        if abs(bps) < 1024:
            return f"{bps:.1f} {unit}"
        bps /= 1024
    return f"{bps:.1f} TB/s"


def get_header():
    now = datetime.now().strftime("%a %b %d  %H:%M:%S")
    uptime_secs = time.time() - psutil.boot_time()
    days = int(uptime_secs // 86400)
    hours = int((uptime_secs % 86400) // 3600)
    mins = int((uptime_secs % 3600) // 60)

    avg = psutil.cpu_percent()
    vm = psutil.virtual_memory()

    cpu_color = pick_color(avg)
    mem_color = pick_color(vm.percent)

    left = f"[bold #00d4ff]  SYSTEM DASHBOARD[/]  [dim]│[/]  {now}"
    right = f"[dim]up [/][bold]{days}d {hours}h {mins}m[/]  [dim]│[/]  CPU [{cpu_color}]{avg:.0f}%[/]  MEM [{mem_color}]{vm.percent:.0f}%[/]  [dim]│  q/Ctrl+C to exit[/]"

    return Panel(
        Text.from_markup(f"{left}    {right}"),
        style="#333333",
        border_style="#555555",
        height=3,
    )


def get_cpu_panel():
    cpu_percent = psutil.cpu_percent(percpu=True)
    avg = psutil.cpu_percent()
    freq = psutil.cpu_freq()
    cpu_history.append(avg)

    BAR_W = 25
    num_cores = len(cpu_percent)
    core_pad = 3 if num_cores >= 10 else 2

    lines = []
    for row_start in range(0, num_cores, 2):
        cores_in_row = cpu_percent[row_start:row_start + 2]
        parts = []
        for i, pct in enumerate(cores_in_row):
            core_num = row_start + i
            color = pick_color(pct)
            filled = int(pct / 100 * BAR_W)
            bar = f"[{color}]{'━' * filled}[dim #333]{'─' * (BAR_W - filled)}[/]"
            parts.append(f"[dim]C{core_num:<{core_pad}}[/]{bar} [{color}]{pct:5.1f}%[/]")
        lines.append("   ".join(parts))

    lines.append("")

    avg_color = pick_color(avg)
    avg_filled = int(avg / 100 * BAR_W)
    avg_bar = f"[{avg_color}]{'━' * avg_filled}[dim #333]{'─' * (BAR_W - avg_filled)}[/]"
    spark = sparkline(cpu_history)
    lines.append(f"[bold]AVG [/]{avg_bar} [{avg_color}]{avg:5.1f}%[/]  {spark}")

    info_parts = []
    if freq:
        info_parts.append(f"[dim]Freq:[/] [bold]{freq.current:.0f}[/] [dim]MHz[/]")
    info_parts.append(f"[dim]Cores:[/] [bold]{psutil.cpu_count()}[/]")
    load1, load5, load15 = os.getloadavg()
    cores = psutil.cpu_count()
    l1_color = pick_color(load1 / cores * 100)
    info_parts.append(f"[dim]Load:[/] [{l1_color}]{load1:.2f}[/] [dim]/[/] {load5:.2f} [dim]/[/] {load15:.2f}")
    lines.append("  [dim]│[/]  ".join(info_parts))

    return Panel(
        "\n".join(lines),
        title="[bold #00d4ff] CPU [/]",
        title_align="left",
        border_style="#00d4ff",
        padding=(1, 1),
    )


def get_memory_panel():
    vm = psutil.virtual_memory()
    sw = psutil.swap_memory()

    used_gb = vm.used / (1024 ** 3)
    total_gb = vm.total / (1024 ** 3)
    avail_gb = vm.available / (1024 ** 3)
    cached_gb = vm.cached / (1024 ** 3)
    buffers_gb = vm.buffers / (1024 ** 3)

    bar = make_bar(vm.percent, 35)
    lines = [
        f"  {bar}",
        "",
        f"  [bold]{used_gb:.1f}[/] [dim]GB used of[/] [bold]{total_gb:.1f}[/] [dim]GB[/]",
        f"  [#86efac]{avail_gb:.1f}[/] [dim]GB available[/]",
        "",
        f"  [dim]Cached  [/] [bold]{cached_gb:.1f}[/] [dim]GB[/]   [dim]Buffers [/] [bold]{buffers_gb:.1f}[/] [dim]GB[/]",
    ]

    if sw.total > 0:
        sw_bar = make_bar(sw.percent, 20)
        lines.append(f"  [dim]Swap    [/] {sw.used / (1024**3):.1f} [dim]/[/] {sw.total / (1024**3):.1f} [dim]GB[/]  {sw_bar}")

    return Panel(
        "\n".join(lines),
        title="[bold #c084fc] MEMORY [/]",
        title_align="left",
        border_style="#c084fc",
        padding=(1, 1),
    )


def get_temp_panel():
    temps = psutil.sensors_temperatures()
    if not temps:
        return Panel(
            "\n  [dim]No sensors detected[/]\n",
            title="[bold #f87171] TEMPS [/]",
            title_align="left",
            border_style="#f87171",
            padding=(1, 1),
        )

    lines = []
    for name, entries in temps.items():
        for entry in entries:
            label = (entry.label or name)[:16]
            t = entry.current
            color = pick_color(t if t > 40 else 0)

            filled = min(int(t / 100 * 20), 20)
            bar = f"[{color}]{'━' * filled}[dim #333]{'─' * (20 - filled)}[/]"

            high_str = f"[dim]H:{entry.high:.0f}°[/]" if entry.high else ""
            crit_str = f"[dim]C:{entry.critical:.0f}°[/]" if entry.critical else ""
            lines.append(f"  {label:<16} {bar} [{color}]{t:4.0f}°C[/]  {high_str} {crit_str}")

    return Panel(
        "\n".join(lines) if lines else "  [dim]No data[/]",
        title="[bold #f87171] TEMPS [/]",
        title_align="left",
        border_style="#f87171",
        padding=(1, 1),
    )


def get_disk_panel():
    lines = []
    for part in psutil.disk_partitions():
        if "loop" in part.device or "snap" in part.mountpoint:
            continue
        try:
            usage = psutil.disk_usage(part.mountpoint)
        except PermissionError:
            continue

        pct = usage.percent
        mount = part.mountpoint
        if mount == "/":
            icon = "/"
        elif "home" in mount:
            icon = "~"
        elif "boot" in mount:
            icon = "B"
        else:
            icon = mount[-6:]

        bar = make_bar(pct, 20)
        lines.append(f"  [bold]{icon:<8}[/] {bar}  [dim]{fmt_bytes(usage.used)} / {fmt_bytes(usage.total)}[/]")

    io = psutil.disk_io_counters()
    if io:
        lines.append("")
        lines.append(f"  [dim]IO:[/]  [#4ade80]▲[/] {fmt_bytes(io.write_bytes)}  [#60a5fa]▼[/] {fmt_bytes(io.read_bytes)}")

    return Panel(
        "\n".join(lines) if lines else "  [dim]No disks[/]",
        title="[bold #facc15] DISK [/]",
        title_align="left",
        border_style="#facc15",
        padding=(1, 1),
    )


def get_network_panel():
    global net_sent_prev, net_recv_prev

    net = psutil.net_io_counters()

    if net_sent_prev is not None:
        up_speed = (net.bytes_sent - net_sent_prev) / REFRESH_RATE
        down_speed = (net.bytes_recv - net_recv_prev) / REFRESH_RATE
    else:
        up_speed = 0
        down_speed = 0

    net_sent_prev = net.bytes_sent
    net_recv_prev = net.bytes_recv

    net_up_history.append(up_speed)
    net_down_history.append(down_speed)

    try:
        conns = psutil.net_connections(kind="inet")
        established = sum(1 for c in conns if c.status == "ESTABLISHED")
        listening = sum(1 for c in conns if c.status == "LISTEN")
    except psutil.AccessDenied:
        established = listening = 0

    lines = [
        f"  [#4ade80]▲ UP  [/] {fmt_speed(up_speed):>12}   [dim]total:[/] {fmt_bytes(net.bytes_sent)}",
        f"  [#60a5fa]▼ DN  [/] {fmt_speed(down_speed):>12}   [dim]total:[/] {fmt_bytes(net.bytes_recv)}",
        "",
        f"  [dim]Upload  [/] {sparkline(net_up_history, 30)}",
        f"  [dim]Download[/] {sparkline(net_down_history, 30)}",
        "",
        f"  [dim]Connections:[/] [bold]{established}[/] [dim]established[/]  [bold]{listening}[/] [dim]listening[/]",
    ]

    if net.errin + net.errout + net.dropin + net.dropout > 0:
        lines.append(f"  [#f87171]Errors:[/] in:{net.errin} out:{net.errout}  [#f87171]Drops:[/] {net.dropin}/{net.dropout}")

    return Panel(
        "\n".join(lines),
        title="[bold #60a5fa] NETWORK [/]",
        title_align="left",
        border_style="#60a5fa",
        padding=(1, 1),
    )


def get_processes_panel():
    procs = []
    for p in psutil.process_iter(["pid", "name", "cpu_percent", "memory_percent", "status"]):
        try:
            info = p.info
            if info.get("cpu_percent", 0) or info.get("memory_percent", 0):
                procs.append(info)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    procs.sort(key=lambda x: (x.get("cpu_percent") or 0), reverse=True)

    table = Table(
        show_header=True,
        expand=True,
        padding=(0, 1),
        header_style="bold dim",
        border_style="#444444",
        show_edge=False,
        show_lines=False,
    )
    table.add_column("PID", justify="right", width=7, style="dim")
    table.add_column("PROCESS", ratio=1)
    table.add_column("CPU", justify="right", width=8)
    table.add_column("MEM", justify="right", width=8)
    table.add_column("", width=10)

    for p in procs[:12]:
        cpu = p.get("cpu_percent") or 0
        mem = p.get("memory_percent") or 0
        cpu_color = pick_color(cpu)
        mem_color = pick_color(mem * 2)

        cpu_bar_w = min(int(cpu / 10), 8)
        cpu_mini = f"[{cpu_color}]{'█' * cpu_bar_w}[/]"

        table.add_row(
            str(p["pid"]),
            (p["name"] or "?")[:20],
            f"[{cpu_color}]{cpu:5.1f}%[/]",
            f"[{mem_color}]{mem:5.1f}%[/]",
            cpu_mini,
        )

    total = len(procs)
    footer = f"\n  [dim]{total} active processes[/]"

    return Panel(
        Group(table, Text.from_markup(footer)),
        title="[bold #4ade80] PROCESSES [/]",
        title_align="left",
        border_style="#4ade80",
        padding=(1, 0),
    )


def build_layout():
    layout = Layout()
    layout.split_column(
        Layout(name="header", size=3),
        Layout(name="top", ratio=3),
        Layout(name="bottom", ratio=2),
    )
    layout["top"].split_row(
        Layout(name="cpu", ratio=3),
        Layout(name="memory", ratio=2),
        Layout(name="temps", ratio=2),
    )
    layout["bottom"].split_row(
        Layout(name="processes", ratio=3),
        Layout(name="network", ratio=2),
        Layout(name="disk", ratio=2),
    )
    return layout


def update_layout(layout):
    layout["header"].update(get_header())
    layout["cpu"].update(get_cpu_panel())
    layout["memory"].update(get_memory_panel())
    layout["temps"].update(get_temp_panel())
    layout["processes"].update(get_processes_panel())
    layout["network"].update(get_network_panel())
    layout["disk"].update(get_disk_panel())


def main():
    console = Console()
    layout = build_layout()

    psutil.cpu_percent(percpu=True)
    psutil.cpu_percent()

    try:
        with Live(layout, console=console, refresh_per_second=2, screen=True):
            while True:
                update_layout(layout)
                time.sleep(REFRESH_RATE)
    except KeyboardInterrupt:
        console.print("\n[dim]Dashboard stopped.[/]")


if __name__ == "__main__":
    main()

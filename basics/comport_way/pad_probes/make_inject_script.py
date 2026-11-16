# make_inject_script.py - сценарии для pad_probe_inject.exe --script.
#
#   C:\anaconda3\python.exe -I make_inject_script.py periods data\script_periods.txt
#       рампа, шум и синус при командах раз в 1, 2, 3, 4, 5, 6 мс: как период команд меняет выход платы
#   C:\anaconda3\python.exe -I make_inject_script.py replay data\rec2 data\script_replay.txt [--from 17.5] [--len 7]
#       повтор живой записи правого стика (data\rec2_board.csv, время в секундах от начала записи):
#       как есть раз в 1, 2 и 3 мс и сглаженной раз в 1 и 2 мс
#   C:\anaconda3\python.exe -I make_inject_script.py inverse data\rec2 data\script_inverse.txt [--from 17.5] [--len 7]
#       команды раз в 2 мс с опережением u = d + k*(d - d_пред): шум при k = 0, 0.5, 1, 1.5, лесенка, рампа,
#       живая запись через отсчёт и сглаженная; желаемый выход - в data\script_inverse.txt.desired.csv
#
# Формат файла: "seg ИМЯ ДЛИТЕЛЬНОСТЬ_МС ПАУЗА_МС", затем строки "такт rx ry" в координатах платы (Y вниз).
import math
import random
import sys

import numpy as np


def write(path, segs):
    with open(path, "w", encoding="ascii") as f:
        for name, dur, gap, cmds in segs:
            f.write(f"seg {name} {dur} {gap}\n")
            for k, x, y in cmds:
                f.write(f"{k} {int(x)} {int(y)}\n")
    total = sum(d + g for _, d, g, _ in segs) / 1000.0
    print(f"{path}: отрезков {len(segs)}, всего {total:.1f} с")


def periods(path):
    rnd = random.Random(7)
    segs = []
    for p in (1, 2, 3, 4, 5, 6):
        segs.append((f"rampP{p}", 900, 400, [(k, 2000 + 20 * k, -1) for k in range(0, 900, p)]))
    for p in (1, 2, 3, 4):
        segs.append((f"noiseP{p}", 1500, 400, [(k, 15000 + rnd.randint(-200, 200), -5000 + rnd.randint(-200, 200)) for k in range(0, 1500, p)]))
    for p in (1, 2, 3, 4):
        segs.append((f"sine5P{p}", 1000, 400, [(k, round(12000 * math.sin(2 * math.pi * 5 * k / 1000.0)), -1) for k in range(0, 1000, p)]))
    write(path, segs)


def load_board(prefix):
    with open(prefix + "_board.csv", "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        rows = [line.strip().split(",") for line in f if line.strip()]
    col = {n: i for i, n in enumerate(head)}
    t = np.array([float(r[col["t_us"]]) for r in rows])
    rx = np.array([int(r[col["rx"]]) for r in rows])
    ry = np.array([int(r[col["ry"]]) for r in rows])
    return (t - t[0]) / 1e6, rx, ry


def smooth(v, sigma_ms):
    n = int(sigma_ms * 4)
    k = np.exp(-0.5 * (np.arange(-n, n + 1) / sigma_ms) ** 2)
    k /= k.sum()
    return np.convolve(np.pad(v.astype(float), n, mode="edge"), k, mode="valid")


def replay(prefix, path, t_from, length):
    t, rx, ry = load_board(prefix)
    grid = t_from + np.arange(int(length * 1000)) / 1000.0
    idx = np.clip(np.searchsorted(t, grid, side="right") - 1, 0, len(t) - 1)
    x, y = rx[idx], ry[idx]
    n = len(grid)
    sx, sy = np.rint(smooth(x, 5.0)).astype(int), np.rint(smooth(y, 5.0)).astype(int)
    segs = []
    for p in (1, 2, 3):
        segs.append((f"rawP{p}", n, 500, [(k, x[k], y[k]) for k in range(0, n, p)]))
    for p in (1, 2):
        segs.append((f"smoothP{p}", n, 500, [(k, sx[k], sy[k]) for k in range(0, n, p)]))
    write(path, segs)
    np.savetxt(path + ".source.csv", np.column_stack([np.arange(n), x, y, sx, sy]), fmt="%d", delimiter=",", header="k,rx,ry,smooth_rx,smooth_ry", comments="")


def lead(d, kappa):
    """Опережение под сглаживание платы: u[k] = d[k] + kappa * (d[k] - d[k-1]), с обрезкой по диапазону."""
    d = np.asarray(d, dtype=float)
    prev = np.concatenate([[d[0]], d[:-1]])
    return np.clip(np.rint(d + kappa * (d - prev)), -32768, 32767).astype(int)


def inverse(prefix, path, t_from, length):
    """Команды раз в 2 мс с опережением: выход платы должен совпасть с желаемым рядом d.
    Рядом пишется ФАЙЛ.desired.csv: seg,k,dx,dy - желаемый выход (координаты платы) на каждую команду."""
    rnd = random.Random(11)
    segs, desired = [], []

    def add(name, dur, gap, ks, dx, dy, kappa):
        ux, uy = lead(dx, kappa), lead(dy, kappa)
        segs.append((name, dur, gap, [(int(k), ux[i], uy[i]) for i, k in enumerate(ks)]))
        desired.extend((name, int(k), int(dx[i]), int(dy[i])) for i, k in enumerate(ks))

    ks = np.arange(0, 1500, 2)
    nx = np.array([15000 + rnd.randint(-200, 200) for _ in ks])
    ny = np.array([-5000 + rnd.randint(-200, 200) for _ in ks])
    for kappa in (0.0, 0.5, 1.0, 1.5):
        add(f"noiseK{int(kappa * 100)}", 1500, 400, ks, nx, ny, kappa)
    # лесенка с полками по 100 мс: точность и выброс
    levels = [5000, 5200, 5201, 5210, 9000, 3000, 3001, 20000, 19990]
    ks = np.arange(0, 100 * len(levels), 2)
    sx = np.array([levels[k // 100] for k in ks])
    add("stepsK100", 100 * len(levels), 400, ks, sx, np.full(len(ks), -1), 1.0)
    add("stepsK0", 100 * len(levels), 400, ks, sx, np.full(len(ks), -1), 0.0)
    ks = np.arange(0, 900, 2)
    add("rampK100", 900, 400, ks, 2000 + 20 * ks, np.full(len(ks), -1), 1.0)
    # живая запись: каждый второй отсчёт
    t, rx, ry = load_board(prefix)
    grid = t_from + np.arange(int(length * 1000)) / 1000.0
    idx = np.clip(np.searchsorted(t, grid, side="right") - 1, 0, len(t) - 1)
    x, y = rx[idx], ry[idx]
    n = len(grid)
    ks = np.arange(0, n, 2)
    add("rawK100", n, 500, ks, x[ks], y[ks], 1.0)
    add("rawK100odd", n, 500, ks, x[np.minimum(ks + 1, n - 1)], y[np.minimum(ks + 1, n - 1)], 1.0)
    smx, smy = np.rint(smooth(x, 5.0)).astype(int), np.rint(smooth(y, 5.0)).astype(int)
    add("smoothK100", n, 500, ks, smx[ks], smy[ks], 1.0)
    write(path, segs)
    with open(path + ".desired.csv", "w", encoding="ascii") as f:
        f.write("seg,k,dx,dy\n")
        for row in desired:
            f.write(",".join(str(v) for v in row) + "\n")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv[1:]
    if a and a[0] == "periods":
        periods(a[1])
    elif a and a[0] == "inverse":
        t_from = float(a[a.index("--from") + 1]) if "--from" in a else 17.5
        length = float(a[a.index("--len") + 1]) if "--len" in a else 7.0
        inverse(a[1], a[2], t_from, length)
    elif a and a[0] == "replay":
        t_from = float(a[a.index("--from") + 1]) if "--from" in a else 17.5
        length = float(a[a.index("--len") + 1]) if "--len" in a else 7.0
        replay(a[1], a[2], t_from, length)
    else:
        print(__doc__ or "см. шапку файла")

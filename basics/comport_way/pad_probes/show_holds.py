# show_holds.py - где у живой руки полки при быстром движении (участок записи pad_probe_record).
#
#   C:\anaconda3\python.exe -I show_holds.py data\rec2 6 7 [сколько_примеров]
#
# Печатает: скорость на участке, долю полок, расстояния между полками, чётность seq полок, положение стика на полках
# (у упора оси или нет) и примеры приращений вокруг полок.
import sys

import numpy as np

sys.stdout.reconfigure(encoding="utf-8")
prefix, t_from, t_len = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
examples = int(sys.argv[4]) if len(sys.argv) > 4 else 8
d = np.genfromtxt(prefix + "_board.csv", delimiter=",", names=True)
t = (d["t_us"] - d["t_us"][0]) / 1e6
m = (t >= t_from) & (t < t_from + t_len)
x = d["rx"][m].astype(np.int64)
y = d["ry"][m].astype(np.int64)
seq = d["seq"][m].astype(np.int64) if "seq" in d.dtype.names else np.arange(m.sum())
dx, dy = np.diff(x), np.diff(y)
norm = np.hypot(dx, dy)
hold = (dx == 0) & (dy == 0)
print(f"участок {t_from}..{t_from + t_len} с: отчётов {len(x)}, шагов seq {dict(zip(*np.unique(np.diff(seq), return_counts=True)))}")
print(f"скорость (норма приращения): среднее {norm.mean():.1f}, медиана {np.median(norm):.1f}, p10 {np.percentile(norm, 10):.1f}, p90 {np.percentile(norm, 90):.1f}")
print(f"радиус: медиана {np.median(np.hypot(x, y)):.0f}, p10 {np.percentile(np.hypot(x, y), 10):.0f}; у упора оси (|v| >= 32767): X {np.mean(np.abs(x) >= 32767) * 100:.1f} %, Y {np.mean(np.abs(y) >= 32767) * 100:.1f} %")
idx = np.nonzero(hold)[0]
print(f"полок {len(idx)} ({hold.mean() * 100:.1f} %)")
if len(idx):
    gaps = np.diff(idx)
    print(f"расстояние между полками: {dict(sorted(zip(*np.unique(np.minimum(gaps, 40), return_counts=True))))} (40 = 40 и больше)")
    print(f"чётность seq полки: {dict(zip(*np.unique(seq[idx + 1] % 2, return_counts=True)))}")
    xa, ya = np.abs(x[idx]) >= 32767, np.abs(y[idx]) >= 32767
    print(f"на полке одна ось у упора: X {xa.mean() * 100:.1f} %, Y {ya.mean() * 100:.1f} %, ни одна {np.mean(~xa & ~ya) * 100:.1f} %")
    # скорость вокруг полки (без самой полки)
    loc = [np.median(norm[max(0, i - 6):i + 7][norm[max(0, i - 6):i + 7] > 0]) if (norm[max(0, i - 6):i + 7] > 0).any() else 0.0 for i in idx]
    print(f"скорость вокруг полок: медиана {np.median(loc):.1f}, p10 {np.percentile(loc, 10):.1f}, p90 {np.percentile(loc, 90):.1f}")
    step = max(1, len(idx) // examples)
    for i in idx[::step][:examples]:
        a, b = max(0, i - 8), min(len(dx), i + 9)
        print(f"   отчёт {i}: x {x[i]}, y {y[i]}")
        print("      dX " + " ".join(f"{v:5d}" for v in dx[a:b]))
        print("      dY " + " ".join(f"{v:5d}" for v in dy[a:b]))

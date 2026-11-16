# pad_texture_check.py - тот же вывод, что pad_texture_check.exe, но эталоном pad_texture.py.
#
# Запуск: C:\anaconda3\python.exe -I pad_texture_check.py FILE.csv FROM_S LEN_S FROM2_S LEN2_S
# Сверка: вывод двух программ должен совпасть построчно (9 знаков; допуск на последний знак из-за порядка сложения в ДПФ).
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I не добавляет папку скрипта
import pad_texture as pt  # noqa: E402


def load_range(path, t_from, length):
    with open(path, "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        it, ix, iy = head.index("t_us"), head.index("rx"), head.index("ry")
        rows = [line.strip().split(",") for line in f if line.strip()]
    t = np.array([float(r[it]) for r in rows])
    x = np.array([float(r[ix]) for r in rows])
    y = np.array([float(r[iy]) for r in rows])
    s = (t - t[0]) / 1e6
    m = (s >= t_from) & (s < t_from + length)
    return x[m], y[m]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    path = sys.argv[1]
    a = load_range(path, float(sys.argv[2]), float(sys.argv[3]))
    b = load_range(path, float(sys.argv[4]), float(sys.argv[5]))
    fa, fb = pt.windows(a[0], a[1]), pt.windows(b[0], b[1])
    w = pt.w1(fa, fb)
    print(f"rows {len(a[0])} {len(b[0])} windows {len(fa)} {len(fb)}")
    for c, name in enumerate(pt.NAMES):
        print(f"{name} {np.median(fa[:, c]):.9f} {np.median(fb[:, c]):.9f} {w[c]:.9f}")
    print(f"total {w.sum():.9f}")


if __name__ == "__main__":
    main()

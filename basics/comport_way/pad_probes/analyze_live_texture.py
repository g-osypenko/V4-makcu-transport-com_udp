# analyze_live_texture.py - из чего состоит текстура живого стика на шаге 1 мс (для рендерера под сглаживание Off).
#
# Запуск: C:\anaconda3\python.exe -I analyze_live_texture.py data\rec2 [--from 17.5 --len 7]
#
# Вход: PREFIX_board.csv записи pad_probe_record (снимки 0x54: t_us, phase, seq, ..., rx, ry). Берутся только
# отчёты с seq подряд (шаг 1), чтобы приращения были "за один отчёт геймпада".
# Печатает:
#   1. сетку уровней: сколько разных значений, шаги между соседними уровнями (это разрядность АЦП после калибровки);
#   2. чётность: как приращения делятся между чётными и нечётными отчётами (АЦП 500 Гц при отчётах 1000 Гц);
#   3. длины полок и долю полок в зависимости от скорости;
#   4. отношение меньшего приращения к сумме пары (как геймпад делит шаг АЦП на два отчёта).
import sys
from collections import Counter

import numpy as np


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        rows = [line.strip().split(",") for line in f if line.strip()]
    col = {n: i for i, n in enumerate(head)}
    out = {n: np.array([float(r[col[n]]) for r in rows]) for n in ("t_us", "seq", "rx", "ry")}
    out["phase"] = np.array([r[col["phase"]] for r in rows])
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv[1:]
    d = load(a[0] + "_board.csv")
    t = (d["t_us"] - d["t_us"][0]) / 1e6
    if "--from" in a:
        f0 = float(a[a.index("--from") + 1])
        ln = float(a[a.index("--len") + 1]) if "--len" in a else 7.0
        m = (t >= f0) & (t < f0 + ln)
    else:
        m = np.ones(len(t), dtype=bool)
    seq, x, y = d["seq"][m].astype(np.int64), d["rx"][m], d["ry"][m]
    print(f"отчётов {len(seq)}, шагов seq: {dict(Counter(np.diff(seq).tolist()).most_common(4))}")
    for name, v in (("X", x), ("Y", y)):
        u = np.unique(v)
        gaps = np.diff(u)
        small = gaps[gaps <= 64]
        print(f"== ось {name}: разных значений {len(u)}, размах {u[0]:.0f}..{u[-1]:.0f}")
        print(f"   шаги между соседними уровнями (до 64): {dict(sorted(Counter(small.astype(int).tolist()).items())[:24])}")
        if len(small):
            print(f"   медиана шага {np.median(small):.0f}, среднее {np.mean(small):.2f}")
        par = seq % 2
        dv = np.diff(v)
        ok = np.diff(seq) == 1
        for p in (0, 1):
            sel = ok & (par[1:] == p)
            nz = dv[sel] != 0
            print(f"   отчёты с seq %% 2 == {p}: всего {int(sel.sum())}, с изменением {int(nz.sum())} ({100.0 * nz.mean():.1f} %), средний |d| {np.mean(np.abs(dv[sel])):.1f}")
        nzd = np.abs(dv[ok & (dv != 0)]).astype(int)
        print(f"   ненулевые |d|: {dict(sorted(Counter(nzd.tolist()).items())[:20])}")
    # пары: отчёт p и p+1 (по чётности с большей долей изменений начинается пара)
    dx, dy = np.diff(x), np.diff(y)
    ok = np.diff(seq) == 1
    act = (dx != 0) | (dy != 0)
    par = (seq % 2)[1:]
    print("== полки (нет изменения ни по X, ни по Y)")
    print(f"   доля полок {1.0 - act[ok].mean():.3f}; по чётности seq: 0 -> {1.0 - act[ok & (par == 0)].mean():.3f}, 1 -> {1.0 - act[ok & (par == 1)].mean():.3f}")
    runs = Counter()
    n = 0
    for v in act:
        if not v:
            n += 1
        elif n:
            runs[n] += 1
            n = 0
    print(f"   длины полок: {dict(sorted(runs.items())[:16])}")
    # скорость по окну 16 отчётов и доля полок в зависимости от неё
    norm = np.hypot(dx, dy)
    k = 16
    sp = np.convolve(norm, np.ones(k) / k, mode="same")
    for lo, hi in ((0, 0.5), (0.5, 2), (2, 8), (8, 30), (30, 100), (100, 400), (400, 1e9)):
        sel = (sp >= lo) & (sp < hi)
        if sel.sum() > 20:
            print(f"   скорость {lo:>5}..{hi:<6} ед/мс: отчётов {int(sel.sum()):5d}, доля полок {1.0 - act[sel].mean():.3f}")
    # деление шага АЦП между двумя отчётами пары
    for name, dv in (("X", dx), ("Y", dy)):
        best = None
        for p in (0, 1):
            i = np.where(ok[:-1] & ok[1:] & (par[:-1] == p))[0]
            a1, a2 = dv[i], dv[i + 1]
            s = a1 + a2
            sel = (np.abs(s) >= 30) & (a1 * a2 >= 0)
            if sel.sum() < 20:
                continue
            ratio = a1[sel] / s[sel]
            spread = float(np.std(ratio))
            if best is None or spread < best[0]:
                best = (spread, p, ratio)
        if best:
            r = best[2]
            print(f"== ось {name}: пара начинается с seq %% 2 == {best[1]}: доля первого отчёта в шаге пары: медиана {np.median(r):.3f}, "
                  f"p10 {np.percentile(r, 10):.3f}, p90 {np.percentile(r, 90):.3f}, СКО {best[0]:.3f} (пар {len(r)})")
    # пример ряда приращений X: медленное и быстрое движение
    for title, lo, hi in (("медленно (2..8 ед/мс)", 2, 8), ("средне (30..100 ед/мс)", 30, 100), ("быстро (>200 ед/мс)", 200, 1e9)):
        idx = np.where((sp >= lo) & (sp < hi))[0]
        if len(idx) > 60:
            s0 = idx[len(idx) // 2]
            print(f"   пример, {title}, с отчёта {s0}: dX " + " ".join(f"{int(v)}" for v in dx[s0:s0 + 40]))
            print(f"   {'':>30} dY " + " ".join(f"{int(v)}" for v in dy[s0:s0 + 40]))


if __name__ == "__main__":
    main()

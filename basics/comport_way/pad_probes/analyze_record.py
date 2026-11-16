# analyze_record.py - разбор записи pad_probe_record.exe: плата (0x54) против Windows (XInput).
#
# Запуск: C:\anaconda3\python.exe -I analyze_record.py data\rec1
#   читает data\rec1_board.csv и data\rec1_xinput.csv
#
# Что считает:
#   1. по фазам: сколько снимков/пакетов в секунду, распределение dt_uframes, пропуски sequence;
#   2. сетку значений стика: сколько разных уровней, наибольший общий делитель приращений, самые частые шаги;
#   3. полярность: средние значения осей в фазах "вверх" и "вправо" на плате и в Windows;
#   4. соответствие плата -> Windows: какое преобразование (x, -x, -x-1) даёт наименьшую ошибку;
#   5. задержку тракта: сдвиг, при котором ряд Windows лучше всего совпадает с рядом платы (сетка 0.25 мс).
import sys
from collections import Counter
from functools import reduce
from math import gcd

import numpy as np


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        rows = [line.strip().split(",") for line in f if line.strip()]
    cols = {name: [r[i] for r in rows] for i, name in enumerate(head)}
    out = {"phase": np.array(cols["phase"])}
    for name in head:
        if name == "phase":
            continue
        out[name] = np.array([float(v) for v in cols[name]])
    return out


def zoh(t, v, grid):
    """Значение ряда (t, v) на моменты grid: последний отсчёт не позже момента."""
    idx = np.searchsorted(t, grid, side="right") - 1
    idx = np.clip(idx, 0, len(v) - 1)
    return v[idx]


def top(counter, n=10):
    return ", ".join(f"{k:g} x{c}" for k, c in counter.most_common(n))


def grid_report(name, v):
    v = v.astype(np.int64)
    d = np.diff(v)
    nz = np.abs(d[d != 0])
    if len(nz) == 0:
        print(f"    {name}: значение не менялось ({v[0] if len(v) else '-'})")
        return
    g = reduce(gcd, [int(x) for x in nz])
    levels = np.unique(v)
    ld = np.diff(levels)
    print(f"    {name}: разных уровней {len(levels)}, диапазон {v.min()}..{v.max()}, НОД приращений {g}")
    print(f"        самые частые |шаги| между соседними отчётами: {top(Counter(nz.tolist()))}")
    print(f"        расстояния между соседними уровнями: {top(Counter(ld.tolist()))}; наименьшее {ld.min()}")
    print(f"        остатки уровней по модулю 256: {top(Counter((levels % 256).tolist()), 6)}; по модулю 64: {top(Counter((levels % 64).tolist()), 6)}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # python -I не смотрит на PYTHONIOENCODING
    prefix = sys.argv[1] if len(sys.argv) > 1 else "data/rec1"
    b = load(prefix + "_board.csv")
    x = load(prefix + "_xinput.csv")
    phases = []
    for p in b["phase"]:
        if p not in phases:
            phases.append(p)
    print(f"== 1. Темп по фазам ({prefix}) ==")
    for ph in phases:
        mb = b["phase"] == ph
        mx = x["phase"] == ph
        tb = b["t_us"][mb]
        tx = x["t_us"][mx]
        dur = (tb[-1] - tb[0]) / 1e6 if len(tb) > 1 else 0.0
        seq = b["seq"][mb].astype(np.int64)
        dseq = np.diff(seq)
        timing = b["timing"][mb].astype(np.int64)
        dt = timing & 0x3FFF
        flags = Counter(((timing >> 14) & 3).tolist())
        usb = np.diff(b["usb_time"][mb].astype(np.int64))
        print(f"  фаза {ph}: {dur:.2f} с; плата {len(tb)} снимков = {len(tb) / dur if dur else 0:.0f}/с; Windows {len(tx)} пакетов = "
              f"{len(tx) / dur if dur else 0:.0f}/с")
        print(f"    шаг sequence: {top(Counter(dseq.tolist()), 6)}")
        print(f"    dt_uframes: {top(Counter(dt.tolist()), 6)}; флаги timing (1 baseline, 2 invalid): {dict(flags)}")
        print(f"    шаг usb_timestamp между снимками, x125 мкс: {top(Counter(usb.tolist()), 6)}")
        if len(tb) > 2:
            gaps = np.diff(tb) / 1000.0
            print(f"    промежуток между новыми снимками по QPC, мс: медиана {np.median(gaps):.3f}, p99 {np.percentile(gaps, 99):.3f}, max {gaps.max():.3f}")
        if len(tx) > 2:
            gaps = np.diff(tx) / 1000.0
            print(f"    промежуток между пакетами XInput, мс: медиана {np.median(gaps):.3f}, p99 {np.percentile(gaps, 99):.3f}, max {gaps.max():.3f}")

    print("== 2. Сетка значений (все фазы с движением) ==")
    mb = b["phase"] != "rest"
    mx = x["phase"] != "rest"
    for axis in ("rx", "ry", "lx", "ly"):
        grid_report("плата " + axis, b[axis][mb])
    for axis in ("rx", "ry"):
        grid_report("Windows " + axis, x[axis][mx])
    print("  покой:")
    mr = b["phase"] == "rest"
    for axis in ("rx", "ry", "lx", "ly"):
        v = b[axis][mr]
        if len(v):
            print(f"    плата {axis}: {top(Counter(v.astype(int).tolist()), 8)}")
    mr = x["phase"] == "rest"
    for axis in ("rx", "ry", "lx", "ly"):
        v = x[axis][mr]
        if len(v):
            print(f"    Windows {axis}: {top(Counter(v.astype(int).tolist()), 8)}")

    print("== 3. Полярность ==")
    for ph in ("up", "right"):
        mb = b["phase"] == ph
        mx = x["phase"] == ph
        if mb.sum() < 10 or mx.sum() < 10:
            continue
        # вторая половина фазы: стик уже у упора
        tb = b["t_us"][mb]
        half = tb[0] + (tb[-1] - tb[0]) / 2
        sb = mb & (b["t_us"] > half)
        sx = mx & (x["t_us"] > half)
        print(f"  фаза {ph}: плата rx {np.median(b['rx'][sb]):.0f}, ry {np.median(b['ry'][sb]):.0f} | Windows rx {np.median(x['rx'][sx]):.0f}, "
              f"ry {np.median(x['ry'][sx]):.0f}")

    print("== 4 и 5. Соответствие и задержка плата -> Windows ==")
    for ph in ("circle", "aim"):
        mb = b["phase"] == ph
        mx = x["phase"] == ph
        if mb.sum() < 100 or mx.sum() < 100:
            continue
        tb, tx = b["t_us"][mb], x["t_us"][mx]
        t0, t1 = max(tb[0], tx[0]) + 300e3, min(tb[-1], tx[-1]) - 300e3
        step = 250.0
        grid = np.arange(t0, t1, step)
        for axis in ("rx", "ry"):
            bv = zoh(tb, b[axis][mb], grid)
            best = None
            for name, f in (("x", lambda v: v), ("-x", lambda v: -v), ("-x-1", lambda v: -v - 1)):
                want = f(bv)
                for lag in range(-40, 241):  # -10..+60 мс
                    xv = zoh(tx, x[axis][mx], grid + lag * step)
                    err = float(np.mean(np.abs(xv - want)))
                    if best is None or err < best[0]:
                        best = (err, name, lag * step / 1000.0, float(np.max(np.abs(xv - want))), float(np.mean(xv == want)))
            err0 = float(np.mean(np.abs(zoh(tx, x[axis][mx], grid) - bv)))
            print(f"  фаза {ph}, {axis}: лучше всего Windows = {best[1]} от платы со сдвигом {best[2]:+.2f} мс: средняя ошибка {best[0]:.1f}, "
                  f"max {best[3]:.0f}, точных совпадений {best[4] * 100:.1f} % (без сдвига и преобразования ошибка {err0:.1f})")


if __name__ == "__main__":
    main()

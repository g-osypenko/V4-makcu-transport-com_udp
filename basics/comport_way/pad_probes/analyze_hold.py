# analyze_hold.py - сколько плата держит заданную цель правого стика (сценарий data\script_hold2s.txt и подобные).
#
# Запуск: C:\anaconda3\python.exe -I analyze_hold.py data\inj6
#
# По каждому отрезку: участки времени, где Windows X около заданного (в пределах 30 единиц) и где нет;
# куда выход ушёл и когда вернулся. Физика 0x54 в это время (стик не трогали: ждём 0, -1).
import sys

import numpy as np


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        rows = [line.strip().split(",") for line in f if line.strip()]
    out = {"seg": np.array([r[1] for r in rows])}
    for i, name in enumerate(head):
        if name != "seg":
            out[name] = np.array([float(r[i]) for r in rows])
    return out


def zoh(t, v, grid):
    idx = np.clip(np.searchsorted(t, grid, side="right") - 1, 0, len(v) - 1)
    return v[idx]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    prefix = sys.argv[1]
    tx, bd, xi = load(prefix + "_tx.csv"), load(prefix + "_board.csv"), load(prefix + "_xinput.csv")
    segs = []
    for s in tx["seg"]:
        if s not in segs:
            segs.append(s)
    for seg in segs:
        mt, mx, mb = tx["seg"] == seg, xi["seg"] == seg, bd["seg"] == seg
        rel = tx["release"][mt].astype(int)
        tc, cx = tx["t_us"][mt][rel == 0], tx["rx"][mt][rel == 0]
        t_rel = tx["t_us"][mt][rel == 1][0]
        tw, wx, wy = xi["t_us"][mx], xi["rx"][mx], xi["ry"][mx]
        sel = (tw >= tc[0] + 30e3) & (tw < t_rel)
        want = zoh(tc, cx, tw[sel] - 6500.0)
        near = np.abs(wx[sel] - want) <= 30
        t = (tw[sel] - tc[0]) / 1000.0
        print(f"== {seg}: команд {len(tc)}, значения цели {int(cx.min())}..{int(cx.max())}, длительность {(t_rel - tc[0]) / 1000.0:.0f} мс ==")
        # участки
        runs, start, state = [], t[0], near[0]
        for i in range(1, len(t)):
            if near[i] != state:
                runs.append((start, t[i], state))
                start, state = t[i], near[i]
        runs.append((start, t[-1], state))
        for a, b, st in runs:
            if b - a < 3.0:
                continue
            m = (t >= a) & (t < b)
            print(f"   {a:7.1f}..{b:7.1f} мс: {'на цели' if st else 'НЕ на цели'}; Windows X {int(wx[sel][m].min())}..{int(wx[sel][m].max())}, Y {int(wy[sel][m].min())}..{int(wy[sel][m].max())}")
        pb = (bd["t_us"][mb] >= tc[0]) & (bd["t_us"][mb] < t_rel)
        if pb.sum():
            print(f"   физика 0x54: X {int(bd['rx'][mb][pb].min())}..{int(bd['rx'][mb][pb].max())}, Y {int(bd['ry'][mb][pb].min())}..{int(bd['ry'][mb][pb].max())}")
        # ход вокруг первого ухода с цели
        gone = [r for r in runs if not r[2] and r[1] - r[0] >= 3.0]
        if gone:
            a = gone[0][0]
            m = (t >= a - 3.0) & (t < a + 16.0)
            print("   ход Windows вокруг первого ухода (мс от начала: X,Y): " + " | ".join(f"{tt:.1f}: {int(x)},{int(y)}" for tt, x, y in zip(t[m], wx[sel][m], wy[sel][m])))


if __name__ == "__main__":
    main()

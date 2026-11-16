# analyze_replay.py - повтор живой записи стика через инжект: насколько выход платы в Windows похож на руку.
#
# Запуск: C:\anaconda3\python.exe -I analyze_replay.py data\inj4 data\script_replay.txt.source.csv [--live data\rec2 --from 17.5 --len 7]
#
# Для каждого отрезка: отставание и ошибка слежения за исходным рядом, панель PadTexture19 выхода Windows
# (медиана по окнам 512 отчётов) и расстояние W1 до живой руки (тот же участок записи pad_probe_record в Windows).
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I не добавляет папку скрипта
import pad_texture as pt  # noqa: E402


def load(path, key="seg"):
    with open(path, "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        rows = [line.strip().split(",") for line in f if line.strip()]
    out = {key: np.array([r[head.index(key)] for r in rows])}
    for i, name in enumerate(head):
        if name != key:
            out[name] = np.array([float(r[i]) for r in rows])
    return out


def zoh(t, v, grid):
    idx = np.clip(np.searchsorted(t, grid, side="right") - 1, 0, len(v) - 1)
    return v[idx]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv[1:]
    prefix, src_path = a[0], a[1]
    live_prefix = a[a.index("--live") + 1] if "--live" in a else None
    t_from = float(a[a.index("--from") + 1]) if "--from" in a else 17.5
    length = float(a[a.index("--len") + 1]) if "--len" in a else 7.0
    src = np.loadtxt(src_path, delimiter=",", skiprows=1)
    tx, xi = load(prefix + "_tx.csv"), load(prefix + "_xinput.csv")
    sets = {}
    if live_prefix:
        lv = load(live_prefix + "_xinput.csv", key="phase")
        t = (lv["t_us"] - lv["t_us"][0]) / 1e6
        # начало записи платы и XInput совпадает с точностью до миллисекунд
        m = (t >= t_from) & (t < t_from + length)
        sets["живая рука"] = (lv["rx"][m], lv["ry"][m])
    segs = []
    for s in tx["seg"]:
        if s not in segs:
            segs.append(s)
    print("== слежение за исходным рядом ==")
    for seg in segs:
        mt, mx = (tx["seg"] == seg) & (tx["release"] == 0), xi["seg"] == seg
        tc = tx["t_us"][mt]
        t0 = tc[0]
        t_rel = tx["t_us"][(tx["seg"] == seg) & (tx["release"] == 1)][0]
        tw = xi["t_us"][mx]
        sel = (tw >= t0) & (tw < t_rel)
        wx, wy = xi["rx"][mx][sel], xi["ry"][mx][sel]
        sets[seg] = (wx, wy)
        col = (3, 4) if seg.startswith("smooth") else (1, 2)
        sx, sy = src[:, col[0]], -src[:, col[1]] - 1  # в координатах Windows
        ts = t0 + src[:, 0] * 1000.0
        grid = np.arange(t0 + 100e3, t_rel - 30e3, 250.0)
        best = None
        for lag in range(0, 81):
            ex = zoh(tw[sel], wx, grid) - zoh(ts, sx, grid - lag * 250.0)
            ey = zoh(tw[sel], wy, grid) - zoh(ts, sy, grid - lag * 250.0)
            err = float(np.mean(np.hypot(ex, ey)))
            if best is None or err < best[0]:
                best = (err, lag * 0.25, float(np.percentile(np.hypot(ex, ey), 99)))
        print(f"  {seg:9s}: команд {int(mt.sum())}, пакетов {int(sel.sum())}; отставание {best[1]:.2f} мс, ошибка средняя {best[0]:.1f}, p99 {best[2]:.0f}")
    print("== PadTexture19: медиана по окнам 512 отчётов ==")
    feats = {k: pt.windows(v[0], v[1]) for k, v in sets.items()}
    keys = list(sets.keys())
    print(f"  {'описатель':26s}" + "".join(f"{k[:11]:>12s}" for k in keys))
    for i, name in enumerate(pt.NAMES):
        print(f"  {name:26s}" + "".join(f"{np.median(feats[k][:, i]) if len(feats[k]) else float('nan'):12.3f}" for k in keys))
    print(f"  {'окон':26s}" + "".join(f"{len(feats[k]):12d}" for k in keys))
    if "живая рука" in feats and len(feats["живая рука"]):
        print("== W1 до живой руки (по описателям и сумма) ==")
        ref = feats["живая рука"]
        others = [k for k in keys if k != "живая рука"]
        tab = {k: pt.w1(ref, feats[k]) for k in others if len(feats[k])}
        print(f"  {'описатель':26s}" + "".join(f"{k[:11]:>12s}" for k in tab))
        for i, name in enumerate(pt.NAMES):
            print(f"  {name:26s}" + "".join(f"{tab[k][i]:12.3f}" for k in tab))
        print(f"  {'СУММА':26s}" + "".join(f"{tab[k].sum():12.3f}" for k in tab))


if __name__ == "__main__":
    main()

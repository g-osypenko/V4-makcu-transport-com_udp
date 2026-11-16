# analyze_desired.py - выход платы в Windows против желаемого ряда (сценарии make_inject_script.py inverse).
#
# Запуск: C:\anaconda3\python.exe -I analyze_desired.py data\inj5 data\script_inverse.txt.desired.csv [--live data\rec2 --from 17.5 --len 7]
#
# Для каждого отрезка пакеты Windows делятся на две фазы по времени после последней команды (0..1 мс и 1..2 мс).
# Для каждой фазы ищется сдвиг в командах, при котором пакет лучше всего совпадает с желаемым значением:
# доля точных совпадений, средняя и наибольшая ошибка. Затем панель PadTexture19 и W1 до живой руки.
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


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv[1:]
    prefix, des_path = a[0], a[1]
    live_prefix = a[a.index("--live") + 1] if "--live" in a else None
    t_from = float(a[a.index("--from") + 1]) if "--from" in a else 17.5
    length = float(a[a.index("--len") + 1]) if "--len" in a else 7.0
    tx, xi, des = load(prefix + "_tx.csv"), load(prefix + "_xinput.csv"), load(des_path)
    sets = {}
    if live_prefix:
        lv = load(live_prefix + "_xinput.csv", key="phase")
        t = (lv["t_us"] - lv["t_us"][0]) / 1e6
        m = (t >= t_from) & (t < t_from + length)
        sets["живая рука"] = (lv["rx"][m], lv["ry"][m])
    segs = []
    for s in tx["seg"]:
        if s not in segs:
            segs.append(s)
    print("== выход Windows против желаемого ==")
    for seg in segs:
        mt, mx, md = (tx["seg"] == seg) & (tx["release"] == 0), xi["seg"] == seg, des["seg"] == seg
        tc = tx["t_us"][mt]
        t_rel = tx["t_us"][(tx["seg"] == seg) & (tx["release"] == 1)][0]
        tw = xi["t_us"][mx]
        sel = (tw >= tc[0]) & (tw < t_rel)
        tw, wx, wy = tw[sel], xi["rx"][mx][sel], xi["ry"][mx][sel]
        sets[seg] = (wx, wy)
        dx, dy = des["dx"][md], -des["dy"][md] - 1  # желаемое в координатах Windows
        period = float(np.median(np.diff(tc)))
        k_of = np.searchsorted(tc, tw, side="right") - 1
        age = (tw - tc[np.clip(k_of, 0, len(tc) - 1)]) / 1000.0
        print(f"  {seg}: команд {len(tc)} раз в {period / 1000.0:.2f} мс, пакетов {len(tw)}")
        for ph in range(int(round(period / 1000.0))):
            m = (np.floor(age) == ph) & (tw > tc[0] + 60e3)
            if m.sum() < 20:
                continue
            best = None
            for lag in range(0, 8):
                kk = k_of[m] - lag
                ok = kk >= 0
                ex, ey = wx[m][ok] - dx[kk[ok]], wy[m][ok] - dy[kk[ok]]
                e = np.maximum(np.abs(ex), np.abs(ey))
                cand = (float(np.mean(e)), lag, float(np.mean(e == 0)), float(np.mean(e <= 1)), float(np.percentile(e, 99)), float(e.max()))
                if best is None or cand[0] < best[0]:
                    best = cand
            print(f"     пакеты через {ph}..{ph + 1} мс после команды ({int(m.sum())}): лучший сдвиг {best[1]} команд = {best[1] * period / 1000.0 + ph + 0.5:.1f} мс; "
                  f"точно {best[2] * 100:.1f} %, в пределах 1: {best[3] * 100:.1f} %, ошибка средняя {best[0]:.1f}, p99 {best[4]:.0f}, max {best[5]:.0f}")
    feats = {k: pt.windows(v[0], v[1]) for k, v in sets.items()}
    keys = [k for k in sets if len(feats[k])]
    print("== PadTexture19: медиана по окнам 512 отчётов ==")
    print(f"  {'описатель':26s}" + "".join(f"{k[:11]:>12s}" for k in keys))
    for i, name in enumerate(pt.NAMES):
        print(f"  {name:26s}" + "".join(f"{np.median(feats[k][:, i]):12.3f}" for k in keys))
    print(f"  {'окон':26s}" + "".join(f"{len(feats[k]):12d}" for k in keys))
    if "живая рука" in keys:
        ref = feats["живая рука"]
        tab = {k: pt.w1(ref, feats[k]) for k in keys if k != "живая рука"}
        print("== W1 до живой руки ==")
        print(f"  {'описатель':26s}" + "".join(f"{k[:11]:>12s}" for k in tab))
        for i, name in enumerate(pt.NAMES):
            print(f"  {name:26s}" + "".join(f"{tab[k][i]:12.3f}" for k in tab))
        print(f"  {'СУММА':26s}" + "".join(f"{tab[k].sum():12.3f}" for k in tab))


if __name__ == "__main__":
    main()

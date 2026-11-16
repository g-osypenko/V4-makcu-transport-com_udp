# analyze_inject.py - разбор записи pad_probe_inject.exe: команда SET 0x40 -> что получил Windows (XInput).
#
# Запуск: C:\anaconda3\python.exe -I analyze_inject.py data\inj1 [--trace ИМЯ[,ИМЯ]] [--ms 40]
#
# По каждому отрезку:
#   - задержка: от первой команды до первого изменения в Windows и до 50 % и 90 % хода;
#   - установившееся значение в Windows против заданного (Windows X = X, Windows Y = -Y-1);
#   - отпускание: от команды с нулями до первого изменения и до возврата к покою;
#   - сколько разных пакетов Windows на одну команду; что показывала физика 0x54 во время инжекта.
# --trace печатает ход Windows по миллисекундам после первой команды и после отпускания.
import sys
from collections import Counter

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


def order(seq):
    seen = []
    for s in seq:
        if s not in seen:
            seen.append(s)
    return seen


def zoh(t, v, grid):
    idx = np.clip(np.searchsorted(t, grid, side="right") - 1, 0, len(v) - 1)
    return v[idx]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    prefix = args[0]
    trace = []
    ms = 40
    if "--trace" in args:
        trace = args[args.index("--trace") + 1].split(",")
    if "--ms" in args:
        ms = int(args[args.index("--ms") + 1])
    tx, bd, xi = load(prefix + "_tx.csv"), load(prefix + "_board.csv"), load(prefix + "_xinput.csv")
    for seg in order(tx["seg"]):
        mt, mb, mx = tx["seg"] == seg, bd["seg"] == seg, xi["seg"] == seg
        t_tx, rel = tx["t_us"][mt], tx["release"][mt].astype(int)
        cx, cy = tx["rx"][mt].astype(int), tx["ry"][mt].astype(int)
        act = rel == 0
        t_first = t_tx[act][0]
        t_rel = t_tx[rel == 1][0]
        t_x, xr, yr = xi["t_us"][mx], xi["rx"][mx].astype(int), xi["ry"][mx].astype(int)
        n_cmd = int(act.sum())
        dur = (t_rel - t_first) / 1000.0
        during = (t_x >= t_first) & (t_x < t_rel)
        print(f"== {seg}: команд {n_cmd} за {dur:.0f} мс; пакетов Windows за это время {int(during.sum())} ({during.sum() / max(dur, 1) * 1000:.0f}/с) ==")
        # физика во время инжекта
        pb = (bd["t_us"][mb] >= t_first) & (bd["t_us"][mb] < t_rel)
        if pb.sum():
            prx, pry = bd["rx"][mb][pb].astype(int), bd["ry"][mb][pb].astype(int)
            print(f"   физика 0x54 во время инжекта: rx {dict(Counter(prx.tolist()).most_common(3))}, ry {dict(Counter(pry.tolist()).most_common(3))}; "
                  f"снимков {int(pb.sum())} ({pb.sum() / max(dur, 1) * 1000:.0f}/с)")
        # цель в координатах Windows
        wx, wy = cx[act], -cy[act] - 1
        # какая ось движется
        axis_x = np.ptp(wx) + abs(int(wx[0])) >= np.ptp(wy) + abs(int(wy[0]))
        w = xr if axis_x else yr
        target0 = int(wx[0] if axis_x else wy[0])
        const = np.ptp(wx) == 0 and np.ptp(wy) == 0
        before = w[t_x < t_first]
        base = int(before[-1]) if len(before) else 0
        aft = t_x >= t_first
        ch = np.nonzero(aft & (w != base))[0]
        if len(ch):
            print(f"   первая команда -> первое изменение в Windows: {(t_x[ch[0]] - t_first) / 1000.0:.2f} мс (значение {w[ch[0]]})")
        if const:
            settled = w[(t_x > t_first + (t_rel - t_first) * 0.6) & (t_x < t_rel)]
            if len(settled):
                print(f"   установилось: Windows {'X' if axis_x else 'Y'} = {int(np.median(settled))} [{settled.min()}..{settled.max()}], задано {target0}; "
                      f"другая ось {int(np.median((yr if axis_x else xr)[(t_x > t_first + (t_rel - t_first) * 0.6) & (t_x < t_rel)]))}")
            for frac in (0.1, 0.5, 0.9, 0.99):
                lvl = base + (target0 - base) * frac
                hit = np.nonzero(aft & ((w - lvl) * np.sign(target0 - base) >= 0))[0]
                if len(hit):
                    print(f"   {frac * 100:.0f} % хода через {(t_x[hit[0]] - t_first) / 1000.0:.2f} мс", end=";")
            print()
        else:
            # движущаяся цель: сдвиг и ошибка на сетке 0.25 мс
            grid = np.arange(t_first + 100e3, t_rel - 20e3, 250.0)
            if len(grid) > 50:
                cmd_t = t_tx[act]
                best = None
                for lag in range(0, 161):
                    want = zoh(cmd_t, (wx if axis_x else wy).astype(float), grid - lag * 250.0)
                    got = zoh(t_x, w.astype(float), grid)
                    err = float(np.mean(np.abs(got - want)))
                    if best is None or err < best[0]:
                        best = (err, lag * 0.25, float(np.max(np.abs(got - want))))
                got = zoh(t_x, w.astype(float), grid)
                print(f"   движущаяся цель: Windows отстаёт от команды на {best[1]:.2f} мс (средняя ошибка при этом сдвиге {best[0]:.1f}, max {best[2]:.0f}); "
                      f"размах Windows {got.min():.0f}..{got.max():.0f}, размах команды {(wx if axis_x else wy).min()}..{(wx if axis_x else wy).max()}")
            dw = np.diff(w[during])
            if len(dw):
                print(f"   приращения Windows между пакетами: нулевых {np.mean(dw == 0) * 100:.1f} %, |d| p50 {np.percentile(np.abs(dw), 50):.0f}, p99 {np.percentile(np.abs(dw), 99):.0f}, "
                      f"max {np.abs(dw).max()}; смена знака {np.mean(np.sign(dw[1:]) * np.sign(dw[:-1]) < 0) * 100:.1f} %")
        # отпускание
        last = w[t_x < t_rel]
        lastv = int(last[-1]) if len(last) else 0
        ar = t_x >= t_rel
        ch = np.nonzero(ar & (w != lastv))[0]
        rest = np.nonzero(ar & (xr == 0) & (yr == 0))[0]
        s = "   отпускание: "
        if len(ch):
            s += f"первое изменение через {(t_x[ch[0]] - t_rel) / 1000.0:.2f} мс; "
        if len(rest):
            s += f"Windows (0, 0) через {(t_x[rest[0]] - t_rel) / 1000.0:.2f} мс; "
        tail = ar & (t_x > t_rel + 300e3)
        if tail.sum():
            s += f"через 300 мс: X {dict(Counter(xr[tail].tolist()).most_common(2))}, Y {dict(Counter(yr[tail].tolist()).most_common(2))}"
        print(s)
        if seg in trace or "all" in trace:
            for title, t_ref in (("после первой команды", t_first), ("после отпускания", t_rel)):
                print(f"   ход Windows {title} (мс: X, Y):")
                m = (t_x >= t_ref - 2000.0) & (t_x < t_ref + ms * 1000.0)
                line = "     "
                for tt, a, b in zip(t_x[m], xr[m], yr[m]):
                    line += f"{(tt - t_ref) / 1000.0:+.1f}: {a},{b} | "
                    if len(line) > 150:
                        print(line)
                        line = "     "
                if line.strip():
                    print(line)


if __name__ == "__main__":
    main()

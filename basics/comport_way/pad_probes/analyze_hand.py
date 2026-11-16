# analyze_hand.py - разбор pad_probe_hand.exe: инжект правого стика при движущейся руке.
#
# Запуск: C:\anaconda3\python.exe -I analyze_hand.py data\hand1
#
# Координаты: плата Y вниз; Windows Y = -Y - 1. Всё сравнивается в координатах Windows.
#   rotate: постоянный инжект (12000, -1) при вращении стика. Что на выходе: чистый инжект или смесь с рукой?
#           Остаётся ли 0x54 чистой физикой? Как выход возвращается к руке после нулей.
#   right:  стик держат вправо; инжект (-12000, -1), потом нули: ход выхода от руки к инжекту и обратно.
#   echo:   команда = физика: насколько выход отстаёт от руки без опережения и с опережением; для сравнения -
#           отставание при свободном проходе (первые секунды шага, без команд).
import sys
from collections import Counter

import numpy as np


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        head = f.readline().strip().split(",")
        rows = [line.strip().split(",") for line in f if line.strip()]
    out = {"step": np.array([r[1] for r in rows])}
    for i, name in enumerate(head):
        if name != "step":
            out[name] = np.array([float(r[i]) for r in rows])
    return out


def zoh(t, v, grid):
    idx = np.clip(np.searchsorted(t, grid, side="right") - 1, 0, len(v) - 1)
    return v[idx]


def lag_fit(t_ref, rx, ry, t_w, wx, wy, t0, t1, max_ms=30.0):
    """Сдвиг, при котором ряд Windows лучше всего совпадает с опорным (оба в координатах Windows)."""
    grid = np.arange(t0, t1, 250.0)
    best = None
    for lag in np.arange(0.0, max_ms + 0.01, 0.25):
        ex = zoh(t_w, wx, grid) - zoh(t_ref, rx, grid - lag * 1000.0)
        ey = zoh(t_w, wy, grid) - zoh(t_ref, ry, grid - lag * 1000.0)
        e = np.hypot(ex, ey)
        cand = (float(np.mean(e)), lag, float(np.percentile(e, 99)))
        if best is None or cand[0] < best[0]:
            best = cand
    return best


def trace(t_w, wx, wy, t_ref, ms=24, pre=2):
    m = (t_w >= t_ref - pre * 1000.0) & (t_w < t_ref + ms * 1000.0)
    line, out = "     ", []
    for tt, a, b in zip(t_w[m], wx[m], wy[m]):
        line += f"{(tt - t_ref) / 1000.0:+.1f}: {int(a)},{int(b)} | "
        if len(line) > 150:
            out.append(line)
            line = "     "
    if line.strip():
        out.append(line)
    return "\n".join(out)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    prefix = sys.argv[1]
    tx, bd, xi = load(prefix + "_tx.csv"), load(prefix + "_board.csv"), load(prefix + "_xinput.csv")
    for step in ("rotate", "right", "echo"):
        mb, mx, mt = bd["step"] == step, xi["step"] == step, tx["step"] == step
        if mb.sum() < 100 or mx.sum() < 100:
            print(f"== {step}: данных нет ==")
            continue
        tb, px, py = bd["t_us"][mb], bd["rx"][mb], -bd["ry"][mb] - 1  # физика в координатах Windows
        tw, wx, wy = xi["t_us"][mx], xi["rx"][mx], xi["ry"][mx]
        tc, cx, cy, rel, mode = tx["t_us"][mt], tx["rx"][mt], -tx["ry"][mt] - 1, tx["release"][mt].astype(int), tx["mode"][mt].astype(int)
        t0 = tb[0]
        print(f"== {step}: {(tb[-1] - t0) / 1e6:.1f} с, снимков платы {mb.sum()}, пакетов Windows {mx.sum()}, команд {int((rel == 0).sum())} ==")
        if not (rel == 0).any():
            continue
        t_on = tc[rel == 0][0]
        t_off = tc[rel == 1][0] if (rel == 1).any() else tc[-1]
        # свободный проход до инжекта: рука двигалась? отставание
        m0 = (tb >= t0 + 1.0e6) & (tb < t_on - 0.1e6)
        if m0.sum() > 200:
            print(f"   до инжекта: размах физики X {px[m0].min():.0f}..{px[m0].max():.0f}, Y {py[m0].min():.0f}..{py[m0].max():.0f}")
            if np.ptp(px[m0]) + np.ptp(py[m0]) > 4000:
                b = lag_fit(tb, px, py, tw, wx, wy, t0 + 1.2e6, t_on - 0.2e6, 10.0)
                print(f"   свободный проход (без команд): Windows отстаёт от 0x54 на {b[1]:.2f} мс, ошибка средняя {b[0]:.1f}, p99 {b[2]:.0f}")
        if step in ("rotate", "right"):
            mi = (tw >= t_on + 0.3e6) & (tw < t_off - 0.05e6)
            mbi = (tb >= t_on + 0.3e6) & (tb < t_off - 0.05e6)
            want = (int(cx[rel == 0][0]), int(cy[rel == 0][0]))
            print(f"   во время инжекта {want}: Windows X {dict(Counter(wx[mi].astype(int).tolist()).most_common(3))}, Y {dict(Counter(wy[mi].astype(int).tolist()).most_common(3))}; "
                  f"разных значений X {len(np.unique(wx[mi]))}, Y {len(np.unique(wy[mi]))}")
            print(f"   физика 0x54 в это время: X {px[mbi].min():.0f}..{px[mbi].max():.0f}, Y {py[mbi].min():.0f}..{py[mbi].max():.0f}, снимков {mbi.sum()} "
                  f"({mbi.sum() / max((t_off - t_on - 0.35e6) / 1e6, 1e-9):.0f}/с)")
            if np.ptp(px[mbi]) + np.ptp(py[mbi]) > 4000 and len(np.unique(wx[mi])) > 3:
                gx = zoh(tb, px, tw[mi])
                print(f"   связь выхода с рукой во время инжекта: корреляция X {np.corrcoef(gx, wx[mi])[0, 1]:+.3f}; "
                      f"наклон (доля руки в выходе) {np.polyfit(gx - gx.mean(), wx[mi] - wx[mi].mean(), 1)[0]:+.4f}")
            print("   ход Windows после первой команды (мс: X, Y):")
            print(trace(tw, wx, wy, t_on))
            print("   физика 0x54 в те же моменты (мс: X, Y):")
            print(trace(tb, px, py, t_on))
            print("   ход Windows после отпускания нулями (мс: X, Y):")
            print(trace(tw, wx, wy, t_off, ms=30))
            print("   физика 0x54 после отпускания (мс: X, Y):")
            print(trace(tb, px, py, t_off, ms=30))
            # когда выход снова совпал с рукой
            ma = tw >= t_off
            diff = np.hypot(wx[ma] - zoh(tb, px, tw[ma] - 500.0), wy[ma] - zoh(tb, py, tw[ma] - 500.0))
            sp = np.hypot(np.gradient(zoh(tb, px, tw[ma])), np.gradient(zoh(tb, py, tw[ma])))
            ok = np.nonzero(diff <= 2.0 * sp + 40.0)[0]
            if len(ok):
                print(f"   выход снова идёт за рукой через {(tw[ma][ok[0]] - t_off) / 1000.0:.2f} мс после нулей")
            if step == "rotate" and tb[-1] - t_off > 1.5e6:
                b = lag_fit(tb, px, py, tw, wx, wy, t_off + 0.3e6, tb[-1] - 0.2e6, 10.0)
                print(f"   после отпускания: Windows отстаёт от 0x54 на {b[1]:.2f} мс, ошибка средняя {b[0]:.1f}, p99 {b[2]:.0f}")
        if step == "echo":
            for title, md in (("команда = физика", 2), ("команда = физика с опережением", 3)):
                sel = (rel == 0) & (mode == md)
                if sel.sum() < 200:
                    continue
                a, b_ = tc[sel][0] + 0.3e6, tc[sel][-1] - 0.05e6
                mbb = (tb >= a) & (tb < b_)
                print(f"   {title}: размах физики X {px[mbb].min():.0f}..{px[mbb].max():.0f}, Y {py[mbb].min():.0f}..{py[mbb].max():.0f}; снимков 0x54 {mbb.sum() / ((b_ - a) / 1e6):.0f}/с")
                r = lag_fit(tb, px, py, tw, wx, wy, a, b_, 30.0)
                print(f"      Windows отстаёт от руки (0x54) на {r[1]:.2f} мс, ошибка средняя {r[0]:.1f}, p99 {r[2]:.0f}")
                grid = np.arange(a, b_, 250.0)
                e0 = np.hypot(zoh(tw, wx, grid) - zoh(tb, px, grid), zoh(tw, wy, grid) - zoh(tb, py, grid))
                sp = np.hypot(np.gradient(zoh(tb, px, grid)), np.gradient(zoh(tb, py, grid))) * 4.0
                print(f"      без сдвига: ошибка средняя {np.mean(e0):.1f}, p99 {np.percentile(e0, 99):.0f}; скорость руки p50 {np.percentile(sp, 50):.1f}, p90 {np.percentile(sp, 90):.1f} ед/мс")


if __name__ == "__main__":
    main()

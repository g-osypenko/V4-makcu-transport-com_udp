# analyze_takeover.py - кто управляет выходом во время инжекта при движущейся руке: команда или физический стик.
#
# Запуск: C:\anaconda3\python.exe -I analyze_takeover.py data\hand2 [--bin 250]
#
# Для каждого шага pad_probe_hand время инжекта режется на корзины (250 мс). В каждой корзине:
#   ошибка "выход против команды" (команда сдвинута на 6.5 мс) и "выход против руки" (0x54 сдвинут на 7.5 мс),
#   обе в координатах Windows; кто ближе - тот и управляет. Плюс размах руки и её скорость.
# Затем: моменты перехода управления от команды к руке и обратно.
import sys

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


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv[1:]
    prefix = a[0]
    bin_ms = float(a[a.index("--bin") + 1]) if "--bin" in a else 250.0
    tx, bd, xi = load(prefix + "_tx.csv"), load(prefix + "_board.csv"), load(prefix + "_xinput.csv")
    steps = []
    for s in tx["step"]:
        if s not in steps:
            steps.append(s)
    for step in steps:
        mb, mx, mt = bd["step"] == step, xi["step"] == step, tx["step"] == step
        tb, px, py = bd["t_us"][mb], bd["rx"][mb], -bd["ry"][mb] - 1
        tw, wx, wy = xi["t_us"][mx], xi["rx"][mx], xi["ry"][mx]
        rel = tx["release"][mt].astype(int)
        tc, cx, cy = tx["t_us"][mt][rel == 0], tx["rx"][mt][rel == 0], -tx["ry"][mt][rel == 0] - 1
        if len(tc) < 10:
            continue
        t_on, t_off = tc[0], tx["t_us"][mt][rel == 1][0]
        print(f"== {step}: инжект {(t_off - t_on) / 1000.0:.0f} мс, команд {len(tc)}, цель X {int(cx.min())}..{int(cx.max())}, Y {int(cy.min())}..{int(cy.max())} ==")
        print("   время, мс | ошибка против команды | против руки | управляет | размах руки X, Y | скорость руки, ед/мс")
        owner_prev, changes = None, []
        sel = (tw >= t_on + 20e3) & (tw < t_off)
        ec_all = np.hypot(wx[sel] - zoh(tc, cx, tw[sel] - 6500.0), wy[sel] - zoh(tc, cy, tw[sel] - 6500.0))
        ep_all = np.hypot(wx[sel] - zoh(tb, px, tw[sel] - 7500.0), wy[sel] - zoh(tb, py, tw[sel] - 7500.0))
        tt = (tw[sel] - t_on) / 1000.0
        for b0 in np.arange(0.0, (t_off - t_on) / 1000.0, bin_ms):
            m = (tt >= b0) & (tt < b0 + bin_ms)
            if m.sum() < 10:
                continue
            ec, ep = float(np.mean(ec_all[m])), float(np.mean(ep_all[m]))
            mp = (tb >= t_on + b0 * 1000.0) & (tb < t_on + (b0 + bin_ms) * 1000.0)
            rng = (np.ptp(px[mp]), np.ptp(py[mp])) if mp.sum() > 2 else (0, 0)
            spd = float(np.sum(np.hypot(np.diff(px[mp]), np.diff(py[mp]))) / bin_ms) if mp.sum() > 2 else 0.0
            owner = "команда" if ec <= ep else "РУКА"
            if ec < 60 and ep < 60:
                owner = "совпадают"
            print(f"   {b0:6.0f} | {ec:8.0f} | {ep:8.0f} | {owner:9s} | {rng[0]:6.0f} {rng[1]:6.0f} | {spd:7.1f}")
            if owner != "совпадают" and owner != owner_prev:
                changes.append((b0, owner))
                owner_prev = owner
        # точные моменты перехода: по пакетам, порог - ошибка против команды больше 400 на 5 пакетах подряд
        far = ec_all > 400
        state, runs = False, []
        i = 0
        while i < len(far):
            if not state and far[i:i + 5].all() and i + 5 <= len(far):
                state = True
                runs.append(["рука с", tt[i]])
            elif state and (~far[i:i + 5]).all() and i + 5 <= len(far):
                state = False
                runs.append(["команда с", tt[i]])
            i += 1
        print("   переходы (мс от начала инжекта): " + ("; ".join(f"{k} {v:.1f}" for k, v in runs) if runs else "нет: выход всё время шёл за командой"))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""pace-analyse.py -- turn an AZ_PACE trace into the four claims it can settle.

The trace (src/common/pace.h) is deliberately dumb: raw events, one clock, no
derived quantities. Everything that could be argued about lives here instead,
where it can be read.

Four sections, because "animations feel laggy" is four different claims:

  PRESENT   how evenly frames reached the screen, per output, in units of that
            output's own OBSERVED period -- not its nominal one. The headless
            backend rounds its frame timer to whole milliseconds, so a fixture
            asking for 144Hz free-runs at ~165Hz; classifying its intervals
            against 6.944ms would report a permanent 15% surplus that is an
            artefact of the fixture and nothing else.

  RENDER    what each render pass cost, and how many of them committed
            NOTHING. A render that produces no frame is invisible from the
            present side (there is no present) and invisible in a frame-rate
            counter (the rate looks fine) -- it shows up only as a wakeup.

  ANIM      per animation: configured duration against measured wall clock,
            when motion actually started and stopped, how many ticks each
            output contributed, and how many targets arrived mid-flight.

  QUANT     the gap between the real-valued position the curve asked for and
            the integer one the scene node stored, as a per-tick series and as
            a run-length distribution of stalled ticks. A curve can be
            perfectly smooth and still arrive as 0,0,1,0,1 pixels.

Usage: pace-analyse.py LOG [--anim-detail N] [--json]
"""
import sys
import re
import json
from collections import defaultdict

TICK = re.compile(
    r"azpace anim tick c=(\S+) mon=(\S+) action=(\d+) dur=(\d+) t_ms=(-?\d+) "
    r"lin=(\S+) factor=(\S+) ideal=(\S+),(\S+),(\S+)x(\S+) "
    r"geom=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) t_ns=(\d+)")
START = re.compile(
    r"azpace anim start c=(\S+) action=(\d+) dur=(\d+) retarget=(\d+) "
    r"from=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) to=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) "
    r"cur=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) t_ns=(\d+)")
END = re.compile(
    r"azpace anim end c=(\S+) action=(\d+) dur=(\d+) t_ms=(-?\d+) "
    r"converged=(\d) t_ns=(\d+)")
PRESENT = re.compile(
    r"azpace present mon=(\S+) seq=(\d+) t_ns=(\d+) delta_us=(-?\d+)")
RENDER = re.compile(
    r"azpace render mon=(\S+) dur_us=(-?\d+) needed=(\d) committed=(\d) "
    r"more=(\d) damage_px=(\d+) damage_rects=(-?\d+) "
    r"damage_ext=(-?\d+),(-?\d+),(-?\d+)x(-?\d+) t_ns=(\d+)")

ACTIONS = {0: "NONE", 1: "OPEN", 2: "MOVE", 3: "CLOSE", 4: "TAG", 5: "FOCUS",
           6: "OPAFADEIN", 7: "OPAFADEOUT", 8: "OVERVIEW"}


def pct(xs, p):
    if not xs:
        return 0
    s = sorted(xs)
    i = min(len(s) - 1, max(0, int(round(p * (len(s) - 1)))))
    return s[i]


def parse(path, since_ns=0):
    presents = defaultdict(list)   # mon -> [(t_ns, delta_us)]
    renders = defaultdict(list)    # mon -> [dict]
    events = []                    # ordered (kind, dict)
    tns = re.compile(r"t_ns=(\d+)")
    for line in open(path, errors="replace"):
        if "azpace " not in line:
            continue
        # Startup is a different workload: every window OPENs, every layer
        # arrives, the first frame damages everything. Including it would put
        # a population the test never asked about into every percentile.
        if since_ns:
            m = tns.search(line)
            if not m or int(m.group(1)) < since_ns:
                continue
        m = TICK.search(line)
        if m:
            g = m.groups()
            events.append(("tick", dict(
                c=g[0], mon=g[1], action=int(g[2]), dur=int(g[3]),
                t_ms=int(g[4]), lin=float(g[5]), factor=float(g[6]),
                ix=float(g[7]), iy=float(g[8]), iw=float(g[9]), ih=float(g[10]),
                x=int(g[11]), y=int(g[12]), w=int(g[13]), h=int(g[14]),
                t_ns=int(g[15]))))
            continue
        m = START.search(line)
        if m:
            g = m.groups()
            events.append(("start", dict(
                c=g[0], action=int(g[1]), dur=int(g[2]), retarget=int(g[3]),
                fx=int(g[4]), fy=int(g[5]), fw=int(g[6]), fh=int(g[7]),
                tx=int(g[8]), ty=int(g[9]), tw=int(g[10]), th=int(g[11]),
                cx=int(g[12]), cy=int(g[13]), cw=int(g[14]), ch=int(g[15]),
                t_ns=int(g[16]))))
            continue
        m = END.search(line)
        if m:
            g = m.groups()
            events.append(("end", dict(c=g[0], action=int(g[1]),
                                       dur=int(g[2]), t_ms=int(g[3]),
                                       converged=int(g[4]),
                                       t_ns=int(g[5]))))
            continue
        m = PRESENT.search(line)
        if m:
            presents[m.group(1)].append((int(m.group(3)), int(m.group(4))))
            continue
        m = RENDER.search(line)
        if m:
            g = m.groups()
            renders[g[0]].append(dict(
                dur_us=int(g[1]), needed=int(g[2]), committed=int(g[3]),
                more=int(g[4]), damage_px=int(g[5]), rects=int(g[6]),
                ext_w=int(g[9]), ext_h=int(g[10]), t_ns=int(g[11])))
    return presents, renders, events


def build_animations(events):
    """One animation = a start, every tick before the next start or the end.

    A retarget is a start that arrives while a run is open. It ends the
    previous run (that target will never be reached) and opens a new one, so
    the durations reported are per-TARGET, which is what a user perceives --
    not per-uninterrupted-clock.
    """
    open_run = {}
    runs = []

    def close(c, why):
        r = open_run.pop(c, None)
        if r:
            r["closed_by"] = why
            runs.append(r)

    for kind, e in events:
        if kind == "start":
            if e["c"] in open_run:
                close(e["c"], "retarget")
            open_run[e["c"]] = dict(
                c=e["c"], action=e["action"], dur=e["dur"],
                retarget=e["retarget"], start_ns=e["t_ns"],
                frm=(e["fx"], e["fy"], e["fw"], e["fh"]),
                to=(e["tx"], e["ty"], e["tw"], e["th"]),
                ticks=[], closed_by="open")
        elif kind == "tick":
            r = open_run.get(e["c"])
            if r is not None:
                r["ticks"].append(e)
        elif kind == "end":
            r = open_run.get(e["c"])
            if r is not None:
                r["end_t_ms"] = e["t_ms"]
                r["end_ns"] = e["t_ns"]
                r["converged"] = e["converged"]
                close(e["c"], "completed")
    for c in list(open_run):
        close(c, "truncated")
    return runs


def summarise_run(r):
    t = r["ticks"]
    out = dict(action=ACTIONS.get(r["action"], str(r["action"])),
               configured_ms=r["dur"], retarget=bool(r["retarget"]),
               closed_by=r["closed_by"], ticks=len(t),
               frm=r["frm"], to=r["to"])
    out["by_mon"] = dict(sorted(
        (k, sum(1 for x in t if x["mon"] == k))
        for k in {x["mon"] for x in t}))
    if not t:
        return out
    out["measured_ms"] = round((t[-1]["t_ns"] - r["start_ns"]) / 1e6, 2)
    out["converged"] = r.get("converged")
    if "end_ns" in r:
        out["measured_ms"] = round((r["end_ns"] - r["start_ns"]) / 1e6, 2)
        out["completion_error_ms"] = round(out["measured_ms"] - r["dur"], 2)
    # motion: ticks where the STORED geometry differed from the previous one
    prev = None
    first_motion = last_motion = None
    moved = 0
    stalls = []
    run_len = 0
    for x in t:
        g = (x["x"], x["y"], x["w"], x["h"])
        if prev is not None and g != prev:
            moved += 1
            if first_motion is None:
                first_motion = x["t_ns"]
            last_motion = x["t_ns"]
            if run_len:
                stalls.append(run_len)
            run_len = 0
        elif prev is not None:
            run_len += 1
        prev = g
    if run_len:
        stalls.append(run_len)
    out["moved_ticks"] = moved
    out["start_delay_ms"] = (round((first_motion - r["start_ns"]) / 1e6, 2)
                             if first_motion else None)
    out["motion_done_ms"] = (round((last_motion - r["start_ns"]) / 1e6, 2)
                             if last_motion else None)
    out["dead_tail_ms"] = (round(out["measured_ms"] - out["motion_done_ms"], 2)
                           if last_motion and "measured_ms" in out else None)
    out["stall_runs_p50"] = pct(stalls, 0.5)
    out["stall_runs_max"] = max(stalls) if stalls else 0
    # quantisation: |ideal - stored| at each tick, and the per-tick step sizes
    qerr = [max(abs(x["ix"] - x["x"]), abs(x["iy"] - x["y"])) for x in t]
    out["quant_err_p50_px"] = round(pct(qerr, 0.5), 3)
    out["quant_err_max_px"] = round(max(qerr), 3) if qerr else 0
    steps = []
    prev = None
    for x in t:
        if prev is not None:
            steps.append(max(abs(x["x"] - prev[0]), abs(x["y"] - prev[1])))
        prev = (x["x"], x["y"])
    if steps:
        out["step_px_p50"] = pct(steps, 0.5)
        out["step_px_max"] = max(steps)
        out["zero_steps"] = sum(1 for s in steps if s == 0)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    since = 0
    for f in list(flags):
        if f.startswith("--since="):
            since = int(f.split("=", 1)[1])
    if not args:
        print(__doc__)
        return 2
    presents, renders, events = parse(args[0], since)
    runs = build_animations(events)
    report = {"present": {}, "render": {}, "anim": []}

    for mon, rows in sorted(presents.items()):
        d = [x[1] for x in rows if x[1] > 0]
        if not d:
            continue
        # The OBSERVED period: the mode of the lower half, which is the
        # cadence the output actually free-runs at when nothing is late.
        base = pct(d, 0.25)
        buckets = defaultdict(int)
        for v in d:
            n = int(round(v / base)) if base else 0
            buckets[min(n, 4) if n <= 4 else 5] += 1
        report["present"][mon] = dict(
            samples=len(d), observed_period_us=base,
            observed_hz=round(1e6 / base, 2) if base else 0,
            p50=pct(d, 0.5), p95=pct(d, 0.95), p99=pct(d, 0.99), max=max(d),
            one_refresh=buckets[1], two_refresh=buckets[2],
            three_refresh=buckets[3], four_refresh=buckets[4],
            five_or_more=buckets[5], sub_refresh=buckets[0])

    for mon, rows in sorted(renders.items()):
        dur = [r["dur_us"] for r in rows]
        dmg = [r["damage_px"] for r in rows if r["committed"]]
        report["render"][mon] = dict(
            passes=len(rows),
            committed=sum(r["committed"] for r in rows),
            needed_no_commit=sum(1 for r in rows
                                 if r["needed"] and not r["committed"]),
            empty=sum(1 for r in rows if not r["needed"]),
            wanted_more=sum(r["more"] for r in rows),
            # A commit that carried NO damage: the scene had nothing to
            # redraw, but something had called wlr_output_schedule_frame, so
            # needs_frame was true anyway. It costs a build, a buffer and a
            # page flip and changes not one pixel.
            committed_zero_damage=sum(1 for r in rows
                                      if r["committed"] and not r["damage_px"]),
            empty_but_wanted_more=sum(1 for r in rows
                                      if not r["needed"] and r["more"]),
            dur_us_p50=pct(dur, 0.5), dur_us_p95=pct(dur, 0.95),
            dur_us_p99=pct(dur, 0.99), dur_us_max=max(dur) if dur else 0,
            damage_px_p50=pct(dmg, 0.5), damage_px_p95=pct(dmg, 0.95),
            damage_px_max=max(dmg) if dmg else 0,
            # Frames that redrew most of the output. A moving window damages
            # its old box and its new one; anything past half the screen is
            # the effect pipeline widening it, not the motion.
            damage_gt_half=sum(1 for v in dmg
                               if v > (max(dmg) if dmg else 0) * 0.5),
            damage_nonzero=sum(1 for v in dmg if v),
            ext_px_p95=pct([r["ext_w"] * r["ext_h"] for r in rows
                            if r["committed"] and r["damage_px"]], 0.95),
            ext_max=max(((r["ext_w"], r["ext_h"]) for r in rows
                         if r["committed"] and r["damage_px"]),
                        key=lambda t: t[0] * t[1], default=(0, 0)),
            rects_max=max((r["rects"] for r in rows), default=0))

    report["anim"] = [summarise_run(r) for r in runs]

    if "--json" in flags:
        print(json.dumps(report, indent=1))
        return 0

    print("== PRESENT (per output, intervals in us) ==")
    for mon, v in report["present"].items():
        print(f"  {mon}: n={v['samples']} observed={v['observed_period_us']}us "
              f"({v['observed_hz']}Hz)")
        print(f"    p50={v['p50']} p95={v['p95']} p99={v['p99']} "
              f"max={v['max']}")
        print(f"    1x={v['one_refresh']} 2x={v['two_refresh']} "
              f"3x={v['three_refresh']} 4x={v['four_refresh']} "
              f">=5x={v['five_or_more']} sub={v['sub_refresh']}")
    print("== RENDER (per output) ==")
    for mon, v in report["render"].items():
        print(f"  {mon}: passes={v['passes']} committed={v['committed']} "
              f"(zero-damage {v['committed_zero_damage']}) "
              f"empty={v['empty']}")
        print(f"    dur_us p50={v['dur_us_p50']} p95={v['dur_us_p95']} "
              f"p99={v['dur_us_p99']} max={v['dur_us_max']}")
        print(f"    damage_px p50={v['damage_px_p50']} "
              f"p95={v['damage_px_p95']} max={v['damage_px_max']} "
              f"rects_max={v['rects_max']}")
        print(f"    damaging frames={v['damage_nonzero']} "
              f"of which >half-of-max={v['damage_gt_half']}  "
              f"ext_px_p95={v['ext_px_p95']} ext_max={v['ext_max']}")
    print("== ANIM ==")
    for a in report["anim"]:
        if a["ticks"] == 0 and a["closed_by"] == "retarget":
            continue
        mons = " ".join(f"{k}={v}" for k, v in a["by_mon"].items())
        print(f"  {a['action']:9s} cfg={a['configured_ms']}ms "
              f"measured={a.get('measured_ms')}ms "
              f"err={a.get('completion_error_ms')}ms "
              f"ticks={a['ticks']} [{mons}] "
              f"closed={a['closed_by']} retarget={a['retarget']} "
              f"converged={a.get('converged')}")
        if a["ticks"]:
            print(f"      moved={a.get('moved_ticks')} "
                  f"zero_steps={a.get('zero_steps')} "
                  f"start_delay={a.get('start_delay_ms')}ms "
                  f"motion_done={a.get('motion_done_ms')}ms "
                  f"dead_tail={a.get('dead_tail_ms')}ms")
            print(f"      step_px p50={a.get('step_px_p50')} "
                  f"max={a.get('step_px_max')}  "
                  f"quant_err p50={a.get('quant_err_p50_px')} "
                  f"max={a.get('quant_err_max_px')}  "
                  f"stall_run max={a.get('stall_runs_max')}")
            print(f"      from={a['frm']} to={a['to']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

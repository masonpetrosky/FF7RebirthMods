"""Compare skinned draws across consecutive captures (dumps from rdc_skinstate.py).

Usage: skin_frames.py <dump dir frame N> <dump dir frame N+1> [min actual motion px to list, default 0]
Draws are matched by index/vertex buffers, first index, index count and base vertex, then by nearest position.
Per matched draw (frame N -> N+1), with UE's TAA jitter removed (View_TemporalAAJitter: xy now, zw last frame):
  moved_cm    object position change between the frames
  obj_ok      frame N+1's previous transform equals frame N's transform
  pose_upd    the bones changed between the frames (the animation was updated for N+1)
  prev_ok     frame N+1's previous bones equal frame N's current bones (the motion vector's start pose is right)
  actual      per-vertex screen motion from frame N to N+1 (pixels): p90 / max
  mv          per-vertex motion vector the vertex shader computes in N+1: p90 / max
  err         |motion vector - actual motion| per vertex: p90 / max
"""
import json
import os
import sys

import numpy as np


def load(path):
    meta = json.load(open(os.path.join(path, "draws.json")))
    draws = []
    for d in meta["draws"]:
        raw = np.fromfile(os.path.join(path, "%d.bin" % d["eid"]), dtype=np.float32)
        b, v = d["bones"] * 16, d["verts"] * 4
        d["bones_cur"] = raw[:b].reshape(-1, 16)[:, [0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14]]
        d["bones_prev"] = raw[b:2 * b].reshape(-1, 16)[:, [0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14]]
        d["cur_clip"] = raw[2 * b:2 * b + v].reshape(-1, 4)
        d["prev_clip"] = raw[2 * b + v:2 * b + 2 * v].reshape(-1, 4)
        d["pos"] = np.array(d["l2w"][12:15]) if d["l2w"] else np.zeros(3)
        draws.append(d)
    return meta["view"], draws


def key(d):
    return (tuple(d["ib"]), d["first_index"], d["indices"], d["base_vertex"], tuple(tuple(v) for v in d["vbs"]))


def ndc(clip, jitter):
    w = clip[:, 3:4]
    ok = (w[:, 0] > 0)
    out = clip[:, :2] / np.where(w > 0, w, 1) - jitter
    return out, ok


def main():
    view_a, draws_a = load(sys.argv[1])
    view_b, draws_b = load(sys.argv[2])
    min_motion = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    ja, jb = np.array(view_a["View_TemporalAAJitter"]), np.array(view_b["View_TemporalAAJitter"])
    width, height = view_b["View_ViewSizeAndInvSize"][:2]
    print("jitter A %s, B %s (B.zw should equal A.xy)" % (ja, jb))
    pool = {}
    for d in draws_a:
        pool.setdefault(key(d), []).append(d)
    rows = []
    for d in draws_b:
        cands = pool.get(key(d), [])
        if not cands:
            continue
        a = min(cands, key=lambda c: np.linalg.norm(c["pos"] - d["pos"]))
        if a["verts"] != d["verts"] or a["bones"] != d["bones"]:
            continue
        moved = float(np.linalg.norm(a["pos"] - d["pos"]))
        obj_ok = float(np.abs(np.array(d["prev_l2w"]) - np.array(a["l2w"])).max())
        pose_upd = float(np.abs(d["bones_cur"] - a["bones_cur"]).max())
        prev_ok = float(np.abs(d["bones_prev"] - a["bones_cur"]).max())
        cur_a, ok_a = ndc(a["cur_clip"], ja[:2])
        cur_b, ok_b = ndc(d["cur_clip"], jb[:2])
        prev_b, ok_p = ndc(d["prev_clip"], jb[2:])
        ok = ok_a & ok_b & ok_p
        scale = np.array([0.5 * width, -0.5 * height])
        actual = (cur_b - cur_a)[ok] * scale
        mv = (cur_b - prev_b)[ok] * scale
        err = np.hypot(*(mv - actual).T)
        am, mm = np.hypot(*actual.T), np.hypot(*mv.T)
        if len(am) == 0 or am.max() < min_motion:
            continue
        sx = ((d["cur_clip"][ok, 0] / d["cur_clip"][ok, 3]) * 0.5 + 0.5) * width
        sy = (0.5 - (d["cur_clip"][ok, 1] / d["cur_clip"][ok, 3]) * 0.5) * height
        rows.append((d["eid"], a["eid"], d["indices"], d["pos"], moved, obj_ok, pose_upd, prev_ok,
                     np.percentile(am, 90), am.max(), np.percentile(mm, 90), mm.max(),
                     np.percentile(err, 90), err.max(), sx.min(), sx.max(), sy.min(), sy.max()))
    print("eidB  eidA  indices  pos                    moved_cm obj_ok  pose_upd prev_ok  "
          "actual p90/max   mv p90/max       err p90/max      screen box")
    for r in rows:
        print("%5d %5d %7d  %6.0f,%7.0f,%6.0f  %7.2f  %6.3f  %8.4f %8.4f  %6.2f/%6.2f  %6.2f/%6.2f  %6.2f/%6.2f  "
              "x%.0f-%.0f y%.0f-%.0f" % (r[0], r[1], r[2], r[3][0], r[3][1], r[3][2], r[4], r[5], r[6], r[7],
                                       r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17]))


main()

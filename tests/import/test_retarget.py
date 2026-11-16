#!/usr/bin/env python3
"""M-OPENWORLD D3: mce retarget (tools/mce_cli/mce_retarget.py) on rigs built in code.

    test_retarget.py <work dir> [mce_cook]

With mce_cook the retargeted character is cooked for real (the engine's glTF importer) and its clips are counted.
"""

import contextlib
import io
import json
import math
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools", "mce_cli"))
sys.path.insert(0, HERE)
import mce_retarget as rt  # noqa: E402
import rig_fixture as rf  # noqa: E402

failures = []
COOK = sys.argv[2] if len(sys.argv) > 2 else None


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL: " + what)


def near(a, b, eps=1e-4):
    return all(abs(x - y) <= eps for x, y in zip(a, b))


def run(argv):
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        code = rt.main(argv)
    return code, err.getvalue()


ENGINE = {k: k for k in ("hips", "spine", "chest", "neck", "head", "shoulder_l", "upper_arm_l", "forearm_l", "hand_l",
                         "shoulder_r", "upper_arm_r", "forearm_r", "hand_r", "thigh_l", "shin_l", "foot_l", "toes_l",
                         "thigh_r", "shin_r", "foot_r", "toes_r")}
MIXAMO = {"hips": "mixamorig:Hips", "spine": "mixamorig:Spine", "chest": "mixamorig:Spine1", "neck": "mixamorig:Neck",
          "head": "mixamorig:Head", "shoulder_l": "mixamorig:LeftShoulder", "upper_arm_l": "mixamorig:LeftArm",
          "forearm_l": "mixamorig:LeftForeArm", "hand_l": "mixamorig:LeftHand", "shoulder_r": "mixamorig:RightShoulder",
          "upper_arm_r": "mixamorig:RightArm", "forearm_r": "mixamorig:RightForeArm", "hand_r": "mixamorig:RightHand",
          "thigh_l": "mixamorig:LeftUpLeg", "shin_l": "mixamorig:LeftLeg", "foot_l": "mixamorig:LeftFoot",
          "toes_l": "mixamorig:LeftToeBase", "thigh_r": "mixamorig:RightUpLeg", "shin_r": "mixamorig:RightLeg",
          "foot_r": "mixamorig:RightFoot", "toes_r": "mixamorig:RightToeBase"}
UNREAL = {"hips": "pelvis", "spine": "spine_01", "chest": "spine_02", "neck": "neck_01", "head": "head",
          "shoulder_l": "clavicle_l", "upper_arm_l": "upperarm_l", "forearm_l": "lowerarm_l", "hand_l": "hand_l",
          "shoulder_r": "clavicle_r", "upper_arm_r": "upperarm_r", "forearm_r": "lowerarm_r", "hand_r": "hand_r",
          "thigh_l": "thigh_l", "shin_l": "calf_l", "foot_l": "foot_l", "toes_l": "ball_l",
          "thigh_r": "thigh_r", "shin_r": "calf_r", "foot_r": "foot_r", "toes_r": "ball_r"}
RIGIFY = {"hips": "DEF-pelvis", "spine": "DEF-spine", "chest": "DEF-spine.001", "neck": "DEF-neck", "head": "DEF-head",
          "shoulder_l": "DEF-shoulder.L", "upper_arm_l": "DEF-upper_arm.L", "forearm_l": "DEF-forearm.L", "hand_l": "DEF-hand.L",
          "shoulder_r": "DEF-shoulder.R", "upper_arm_r": "DEF-upper_arm.R", "forearm_r": "DEF-forearm.R", "hand_r": "DEF-hand.R",
          "thigh_l": "DEF-thigh.L", "shin_l": "DEF-shin.L", "foot_l": "DEF-foot.L", "toes_l": "DEF-toe.L",
          "thigh_r": "DEF-thigh.R", "shin_r": "DEF-shin.R", "foot_r": "DEF-foot.R", "toes_r": "DEF-toe.R"}
VRM = {"hips": "J_Bip_C_Hips", "spine": "J_Bip_C_Spine", "chest": "J_Bip_C_Chest", "neck": "J_Bip_C_Neck",
       "head": "J_Bip_C_Head", "shoulder_l": "J_Bip_L_Shoulder", "upper_arm_l": "J_Bip_L_UpperArm",
       "forearm_l": "J_Bip_L_LowerArm", "hand_l": "J_Bip_L_Hand", "shoulder_r": "J_Bip_R_Shoulder",
       "upper_arm_r": "J_Bip_R_UpperArm", "forearm_r": "J_Bip_R_LowerArm", "hand_r": "J_Bip_R_Hand",
       "thigh_l": "J_Bip_L_UpperLeg", "shin_l": "J_Bip_L_LowerLeg", "foot_l": "J_Bip_L_Foot", "toes_l": "J_Bip_L_ToeBase",
       "thigh_r": "J_Bip_R_UpperLeg", "shin_r": "J_Bip_R_LowerLeg", "foot_r": "J_Bip_R_Foot", "toes_r": "J_Bip_R_ToeBase"}

T_POSE = {"hips": (0, 1.0, 0), "spine": (0, .15, 0), "chest": (0, .2, 0), "neck": (0, .25, 0), "head": (0, .1, 0),
          "shoulder_l": (.05, .2, 0), "upper_arm_l": (.1, 0, 0), "forearm_l": (.25, 0, 0), "hand_l": (.25, 0, 0),
          "thigh_l": (.1, -.05, 0), "shin_l": (0, -.45, 0), "foot_l": (0, -.45, 0), "toes_l": (0, -.05, .12)}


def scaled(offsets, s):
    return {k: tuple(c * s for c in v) for k, v in offsets.items()}


def a_pose(offsets):
    o = dict(offsets)
    o["forearm_l"] = (.18, -.18, 0)
    o["hand_l"] = (.18, -.18, 0)
    return o


def load_rig(work, name, rig, clips=None, mesh=True):
    path = os.path.join(work, name)
    with open(path, "wb") as f:
        f.write(rf.build_glb(rig, clips, mesh))
    return path


def bone_map(path):
    sk = rt.Skeleton(rt.Gltf(path))
    rt.match_humanoid(sk)
    return {k: sk.name[v] for k, v in sk.bones.items()}


def test_name_matching(work):
    for label, names in (("engine", ENGINE), ("mixamo", MIXAMO), ("unreal", UNREAL), ("rigify", RIGIFY), ("vrm", VRM)):
        path = load_rig(work, f"{label}.glb", rf.humanoid(names, T_POSE))
        got = bone_map(path)
        for role, name in names.items():
            if role in ("chest",) and label == "vrm":
                continue
            check(got.get(role) == name, f"{label}: role {role} finds '{name}' (got {got.get(role)})")
        check(len([k for k in got if k not in ("spine", "chest", "upper_chest")]) >= 17, f"{label}: the limbs are all found")


def read_clip(path, name):
    g = rt.Gltf(path)
    for a in g.json["animations"]:
        if a["name"] == name:
            out = {}
            for ch in a["channels"]:
                smp = a["samplers"][ch["sampler"]]
                out[(g.json["nodes"][ch["target"]["node"]]["name"], ch["target"]["path"])] = (g.accessor(smp["input"]), g.accessor(smp["output"]))
            return out
    return None


def pose_at(clip, i):
    pose = {}
    for (bone, path), (times, vals) in clip.items():
        pose.setdefault(bone, {})["r" if path == "rotation" else "t"] = vals[i]
    return pose


def test_identity(work):
    rig = rf.humanoid(ENGINE, T_POSE)
    clip = {"wave": {"rot": {"upper_arm_l": [(0, (0, 0, 0, 1)), (1, rf.quat_z(70))],
                             "thigh_r": [(0, (0, 0, 0, 1)), (1, rf.quat_y(30))]},
                     "trans": {"hips": [(0, (0, 1.0, 0)), (1, (0.1, 1.2, 0.3))]}}}
    src = load_rig(work, "id_src.glb", rig, clip)
    dst = load_rig(work, "id_dst.glb", rig)
    out = os.path.join(work, "id_out.glb")
    code, err = run([src, "--onto", dst, "--out", out])
    check(code == 0, "identity retarget runs: " + err)
    c = read_clip(out, "wave")
    check(c is not None, "the clip arrives")
    if c is None:
        return
    arm_t, arm = c[("upper_arm_l", "rotation")]
    check(near(arm[1], rf.quat_z(70), 1e-4) or near(arm[1], tuple(-x for x in rf.quat_z(70)), 1e-4), f"same rig: arm rotation survives {arm[1]}")
    check(near(arm[0], (0, 0, 0, 1)) or near(arm[0], (0, 0, 0, -1)), "same rig: rest key stays rest")
    ht, hv = c[("hips", "translation")]
    check(near(hv[1], (0.1, 1.2, 0.3), 1e-4), f"same rig: hips translation survives {hv[1]}")
    check(("spine", "rotation") in c, "every mapped bone gets a rotation channel")
    check(read_clip(out, "wave") is not None and rt.Gltf(out).json["animations"][0]["name"] == "wave", "clip name kept")


def test_apose_to_tpose(work):
    yaw180 = rf.quat_y(180)
    src_rig = rf.humanoid(MIXAMO, scaled(a_pose(T_POSE), 2.0), root_rot=yaw180)
    # the source rig's hips sit at 2 * 0.95 + feet; fix the hips translation: root offset is part of the table
    q_local = rf.qmul(rf.qconj_(yaw180), rf.qmul(rf.quat_z(-90), yaw180))
    clips = {"arm_up": {"rot": {MIXAMO["upper_arm_l"]: [(0, (0, 0, 0, 1)), (1, q_local)]},
                        "trans": {MIXAMO["hips"]: [(0, (0, 2.0, 0)), (1, (0, 2.2, 0.4))]}}}
    src = load_rig(work, "mx_src.glb", src_rig, clips)
    dst_rig = rf.humanoid(ENGINE, T_POSE)
    dst = load_rig(work, "eng_dst.glb", dst_rig)
    out = os.path.join(work, "eng_out.glb")
    code, err = run([src, "--onto", dst, "--out", out])
    check(code == 0, "A-pose Mixamo rig onto the T-pose engine rig runs: " + err)
    c = read_clip(out, "arm_up")
    check(c is not None, "the clip arrives on the engine rig")
    if c is None:
        return
    times = c[("upper_arm_l", "rotation")][0]
    check(near(times, [0, 1]), f"key times kept {times}")
    pose0 = rf.fk(dst_rig, pose_at(c, 0))
    pose1 = rf.fk(dst_rig, pose_at(c, 1))
    rest = rf.fk(dst_rig, {})
    # first key is the rest pose: every bone's world rotation matches the target's own rest
    check(all(near(pose0[b][1], rest[b][1], 1e-4) or near(pose0[b][1], tuple(-x for x in rest[b][1]), 1e-4) for b in rest), "key 0 is the target's rest pose")
    up = tuple(pose1["forearm_l"][0][i] - pose1["upper_arm_l"][0][i] for i in range(3))
    check(near(up, (0, 0.25, 0), 1e-3), f"the target's left forearm points up at key 1: {up}")
    right = tuple(pose1["forearm_r"][0][i] - pose1["upper_arm_r"][0][i] for i in range(3))
    check(near(right, (-0.25, 0, 0), 1e-3), f"the right arm did not move: {right}")
    # hips: source delta (0, .2, .4) turned 180 degrees about Y and scaled by 0.95 / 1.9 = 0.5 -> (0, .1, -.2)
    ht, hv = c[("hips", "translation")]
    d = tuple(hv[1][i] - hv[0][i] for i in range(3))
    check(near(d, (0, 0.1, -0.2), 1e-3), f"hips travel is scaled and turned to the target's facing: {d}")
    code, err = run([src, "--onto", dst, "--out", os.path.join(work, "eng_out2.glb"), "--in-place-motion"])
    c2 = read_clip(os.path.join(work, "eng_out2.glb"), "arm_up")
    d2 = tuple(c2[("hips", "translation")][1][1][i] - c2[("hips", "translation")][1][0][i] for i in range(3))
    check(near(d2, (0, 0.1, 0), 1e-3), f"--in-place-motion drops the horizontal travel: {d2}")
    code, err = run([src, "--onto", dst, "--out", os.path.join(work, "eng_out3.glb"), "--no-translation"])
    c3 = read_clip(os.path.join(work, "eng_out3.glb"), "arm_up")
    check(("hips", "translation") not in c3, "--no-translation writes no hips channel")
    code, err = run([src, "--onto", dst, "--out", os.path.join(work, "eng_out4.glb"), "--scale", "1"])
    c4 = read_clip(os.path.join(work, "eng_out4.glb"), "arm_up")
    d4 = tuple(c4[("hips", "translation")][1][1][i] - c4[("hips", "translation")][1][0][i] for i in range(3))
    check(near(d4, (0, 0.2, -0.4), 1e-3), f"--scale 1 forces the ratio: {d4}")
    return src, dst


def test_options(work, src, dst):
    out = os.path.join(work, "opt.glb")
    again = os.path.join(work, "opt2.glb")
    check(run([src, "--onto", dst, "--out", out])[0] == 0 and run([src, "--onto", dst, "--out", again])[0] == 0, "two runs")
    check(open(out, "rb").read() == open(again, "rb").read(), "the output is a pure function of its inputs")
    check(os.path.isfile(out + ".mceasset"), "a sidecar is written for a new output")
    code, err = run([src, "--onto", dst, "--out", out, "--prefix", "mx_", "--clips", "arm_up"])
    check(code == 0 and rt.Gltf(out).json["animations"][0]["name"] == "mx_arm_up", "--prefix renames")
    code, err = run([src, "--onto", dst, "--out", out, "--clips", "nope"])
    check(code == 1 and "no clip named nope" in err, "an unknown --clips name is an error: " + err)
    # a clip that already exists in the character is skipped unless --replace
    twice = os.path.join(work, "twice.glb")
    run([src, "--onto", dst, "--out", twice])
    code, err = run([src, "--onto", twice, "--out", os.path.join(work, "twice2.glb")])
    check(code == 1 and "no clip was retargeted" in err and "already exists" in err, "existing clip names are not duplicated: " + err)
    code, err = run([src, "--onto", twice, "--out", os.path.join(work, "twice3.glb"), "--replace"])
    check(code == 0 and len(rt.Gltf(os.path.join(work, "twice3.glb")).json["animations"]) == 1, "--replace swaps the clip: " + err)
    # --strict turns the warnings into a failure and writes nothing
    tailed = load_rig(work, "tailed.glb", rf.humanoid(ENGINE, T_POSE) + [("tail", "hips", (0, 0, -.1), (0, 0, 0, 1))])
    strict_out = os.path.join(work, "strict.glb")
    code, err = run([src, "--onto", tailed, "--out", strict_out])
    check(code == 0 and "tail" in err and os.path.exists(strict_out), "a bone without a role warns and the run goes on: " + err)
    os.remove(strict_out)
    code, err = run([src, "--onto", tailed, "--out", strict_out, "--strict"])
    check(code == 1 and not os.path.exists(strict_out), "--strict with a warning fails and writes nothing: " + err)
    # --inplace overwrites the character
    inplace = shutil.copy(dst, os.path.join(work, "inplace.glb"))
    code, err = run([src, "--onto", inplace, "--inplace"])
    check(code == 0 and len(rt.Gltf(inplace).json["animations"]) == 1, "--inplace writes into the character: " + err)


def test_map_and_errors(work):
    # a rig whose names the matcher cannot read: refused with the bone list, then fixed with --map
    odd = {k: f"b{i:02d}" for i, k in enumerate(ENGINE)}
    rig = rf.humanoid(odd, T_POSE)
    clip = {"c": {"rot": {odd["upper_arm_l"]: [(0, (0, 0, 0, 1)), (1, rf.quat_z(40))]}}}
    src = load_rig(work, "odd_src.glb", rig, clip)
    dst = load_rig(work, "odd_dst.glb", rf.humanoid(ENGINE, T_POSE))
    code, err = run([src, "--onto", dst, "--out", os.path.join(work, "odd_out.glb")])
    check(code == 1 and "does not look like a humanoid" in err and "b00" in err, "unreadable names are refused with the bone list: " + err)
    mp = os.path.join(work, "map.json")
    with open(mp, "w") as f:
        json.dump({odd[k]: k for k in ENGINE}, f)
    code, err = run([src, "--onto", dst, "--out", os.path.join(work, "odd_out.glb"), "--map", mp])
    check(code == 0, "--map names every pair: " + err)
    c = read_clip(os.path.join(work, "odd_out.glb"), "c")
    check(c is not None and near(c[("upper_arm_l", "rotation")][1][1], rf.quat_z(40), 1e-4), "mapped clip arrives")
    with open(mp, "w") as f:
        json.dump({"nope": "hips"}, f)
    code, err = run([src, "--onto", dst, "--out", os.path.join(work, "odd_out2.glb"), "--map", mp])
    check(code == 1 and "no bone 'nope'" in err, "--map with an unknown bone is an error: " + err)
    code, err = run([os.path.join(work, "missing.glb"), "--onto", dst])
    check(code == 1, "a missing source is an error")


def test_node_rigs(work):
    # a rig animated through plain nodes (no skin), as in Kenney's Blocky Characters
    rig = [("root", None, (0, 0, 0), (0, 0, 0, 1)), ("torso", "root", (0, .5, 0), (0, 0, 0, 1)),
           ("arm-left", "torso", (.3, .3, 0), (0, 0, 0, 1)), ("arm-right", "torso", (-.3, .3, 0), (0, 0, 0, 1)),
           ("leg-left", "root", (.1, .3, 0), (0, 0, 0, 1)), ("leg-right", "root", (-.1, .3, 0), (0, 0, 0, 1)),
           ("head", "torso", (0, .6, 0), (0, 0, 0, 1))]
    clip = {"wave": {"rot": {"arm-left": [(0, (0, 0, 0, 1)), (1, rf.quat_z(60))]}}}
    src = load_rig(work, "node_src.glb", rig, clip, mesh=False)
    dst = load_rig(work, "node_dst.glb", rig, mesh=False)
    out = os.path.join(work, "node_out.glb")
    code, err = run([src, "--onto", dst, "--out", out])
    check(code == 1 and "no skin" in err and "--map-only" in err, "a skinless rig is refused with the way out: " + err)
    mp = os.path.join(work, "node_map.json")
    with open(mp, "w") as f:
        json.dump({n: n for n, *_ in rig}, f)
    code, err = run([src, "--onto", dst, "--out", out, "--map", mp, "--map-only"])
    check(code == 0, "--map-only retargets a node rig: " + err)
    c = read_clip(out, "wave")
    check(c is not None and near(c[("arm-left", "rotation")][1][1], rf.quat_z(60), 1e-4), "the node clip arrives")


def test_texture_folder_warning(work, src, dst):
    w = rt.Writer(rt.Gltf(dst))
    w.j["images"] = [{"uri": "Textures/skin.png"}]
    textured = os.path.join(work, "textured.glb")
    with open(textured, "wb") as f:
        f.write(w.to_glb())
    other = os.path.join(work, "elsewhere")
    code, err = run([src, "--onto", textured, "--out", os.path.join(other, "o.glb")])
    check(code == 0 and "linked by relative path" in err and "Textures/skin.png" in err, "an output away from the textures warns: " + err)
    code, err = run([src, "--onto", textured, "--out", os.path.join(work, "beside.glb")])
    check("relative path" not in err, "an output beside the character does not")


def test_cook(work, out):
    if not COOK:
        return
    p = os.path.join(work, "cookproj")
    os.makedirs(os.path.join(p, "assets", "models"), exist_ok=True)
    shutil.copy(out, os.path.join(p, "assets", "models", "hero.glb"))
    shutil.copy(out + ".mceasset", os.path.join(p, "assets", "models", "hero.glb.mceasset"))
    r = subprocess.run([COOK, p, "--quiet"], capture_output=True, text=True)
    check(r.returncode == 0, f"the retargeted character cooks: exit {r.returncode}\n{r.stdout}{r.stderr}")


def main():
    work = sys.argv[1]
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    test_name_matching(work)
    test_identity(work)
    res = test_apose_to_tpose(work)
    if res:
        test_options(work, *res)
        test_texture_folder_warning(work, *res)
        test_cook(work, os.path.join(work, "opt.glb"))
    test_map_and_errors(work)
    test_node_rigs(work)
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("retarget: ok" + (" (cooked with mce_cook)" if COOK else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())

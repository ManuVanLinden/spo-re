"""Exception-handling database: verification and summary.

Checks the criteria V1-V5 below and some structural invariants over ehdb.py's database.

Checks:
  V1  every __CxxFrameHandler3 stub resolves to a parsed FuncInfo (and every FuncInfo has a stub)
  V2  every funclet is a function entry in the canonical manifest:
      unwind actions, catch handlers, SEH filters, __finally handlers, the stubs themselves
  V3  every resume point (catch continuation, __except body, setjmp return) is a code address
      inside the function that owns the EH frame (the landing owner), per the manifest
  V4  state numbers written by each EH owner stay inside [-1, maxState)
  V5  structural FuncInfo invariants (no IP map, no ES type list, /EHs flag, unwind toState < index,
      try ranges well formed), reported by ehdb.py as problems

V3 reports 8 known landing-owner differences on the GOG exe as FAIL; they are expected, and the exit status
stays 0 (the converter checks that both output files were written).

Usage:   PYTHONPATH=tools/.deps python3 -P tools/eh/verify.py [--base]
Inputs:  scratch/eh/<tag>/ehdb.json (from ehdb.py) and the function manifest
Outputs: data/eh/resume-points.json (ep1; the translator's landing owners) and data/eh/funcinfo-summary.json (ep1),
         or scratch/eh/base/funcinfo-summary.json (--base)
When:    the converter's eh stage, after ehdb.py.
Environment: SPORE_WORK_DIR, SPORE_GAME_DIR and SPORE_EXE move the outputs and name the exe (docs/design/convert.md 4).
"""
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ehimage import Image, FuncIndex, WORK, BASE_EXE, EXE  # noqa: E402


def H(x):
    return int(x, 16)


def main():
    base = "--base" in sys.argv
    tag = "base" if base else "ep1"
    db = json.load(open(os.path.join(WORK, "scratch", "eh", tag, "ehdb.json")))
    im = Image(BASE_EXE if base else EXE)
    fi = FuncIndex(tag)
    canon = fi.canonical
    checks = {}
    fails = collections.defaultdict(list)

    # ---------------- V1
    nstub = sum(len(r["stubs"]) for r in db["funcinfo"])
    checks["V1_stubs_resolve_to_funcinfo"] = {
        "stubs": nstub, "funcinfo": len(db["funcinfo"]),
        "stubs_without_funcinfo": len(db["stubs_without_funcinfo"]),
        "funcinfo_without_stub": len(db["funcinfo_without_stub"]),
        "pass": not db["stubs_without_funcinfo"] and not db["funcinfo_without_stub"]}

    # ---------------- V2
    funclets = collections.defaultdict(set)
    for r in db["funcinfo"]:
        for s in r["stubs"]:
            funclets["handler-stub"].add(H(s))
        for to, act in r["unwind"]:
            if act:
                funclets["unwind"].add(H(act))
        for t in r["try"]:
            for c in t["catches"]:
                funclets["catch"].add(H(c["handler"]))
    for f in db["seh"]:
        for sc in f["scopes"]:
            if sc["filter"]:
                funclets["seh-filter"].add(H(sc["filter"]))
            if sc["kind"] == "__finally":
                funclets["seh-finally"].add(H(sc["handler"]))
    for t in db["throwinfo"]:
        if t["destructor"]:
            funclets["throw-destructor"].add(H(t["destructor"]))
        for c in t["catchable"]:
            if c["copyFunction"]:
                funclets["throw-copyctor"].add(H(c["copyFunction"]))
    v2 = {}
    for k, s in sorted(funclets.items()):
        missing = sorted(x for x in s if x not in canon)
        v2[k] = {"distinct": len(s), "not_in_manifest": len(missing)}
        fails["V2_" + k] += ["0x%x" % x for x in missing]
    checks["V2_funclets_in_manifest"] = {"by_kind": v2, "manifest_loaded": fi.ok,
                                        "pass": fi.ok and all(v["not_in_manifest"] == 0 for v in v2.values())}

    # ---------------- V3 resume points
    resumes = []   # (kind, resume, landing owner)
    for r in db["funcinfo"]:
        owners = {H(g["owner"]) for g in r["registrations"] if g["owner"]}
        for t in r["try"]:
            for c in t["catches"]:
                for k in c["continuations"]:
                    resumes.append(("catch-continuation", H(k), owners))
    for f in db["seh"]:
        for sc in f["scopes"]:
            if sc["kind"] == "__except":
                resumes.append(("seh-except-body", H(sc["handler"]), {H(f["owner"])}))
    for s in db["setjmp"]:
        resumes.append(("setjmp-return", H(s["resume"]), {H(s["owner"])}))
    v3 = collections.Counter()
    misattributed = []
    for kind, va, owners in resumes:
        mo = set(fi.owners(va))
        v3[kind] += 1
        if not (mo & owners):
            misattributed.append({"kind": kind, "resume": "0x%x" % va,
                                  "landing_owner": sorted("0x%x" % o for o in owners),
                                  "manifest_owner": sorted("0x%x" % o for o in mo),
                                  "manifest_label": fi.labels.get(va)})
    labels = collections.Counter(fi.labels.values())
    our = {va for _, va, _ in resumes}
    extra_labels = sorted("0x%x" % a for a, k in fi.labels.items()
                          if k in ("eh-catch-continuation", "seh-except-handler", "seh4-except-handler",
                                   "setjmp-return") and a not in our)
    missing_labels = sorted("0x%x" % va for va in our if va not in fi.labels)
    checks["V3_resume_points"] = {
        "by_kind": dict(v3), "distinct": len(our),
        "landing_functions": len(set().union(*[o for _, _, o in resumes])) if resumes else 0,
        "manifest_owner_differs_from_landing_owner": len(misattributed),
        "manifest_labels": dict(labels), "manifest_labels_not_resume_points": extra_labels,
        "resume_points_without_manifest_label": missing_labels,
        "pass": not misattributed and not missing_labels}
    fails["V3_misattributed"] = misattributed

    # ---------------- V4 state numbers
    st_re = re.compile(r"^(dword|byte) ptr \[ebp - 4\], (0x[0-9a-f]+|\d+)$")
    owner_max = collections.defaultdict(lambda: None)
    for r in db["funcinfo"]:
        for g in r["registrations"]:
            if g["owner"]:
                o = H(g["owner"])
                owner_max[o] = max(owner_max[o] or 0, r["maxState"])
    bad, nowrites, nwrites, bytewrites = [], 0, 0, 0
    for o, mx in owner_max.items():
        vals = []
        for s, e in fi.function_blocks(o):
            for ins in im.disasm(s, e - s):
                if ins.mnemonic == "mov":
                    m = st_re.match(ins.op_str)
                    if m:
                        v = int(m.group(2), 0)
                        if m.group(1) == "byte":
                            bytewrites += 1
                            v = v - 256 if v >= 0x80 else v
                        else:
                            v = v - (1 << 32) if v >= 0x80000000 else v
                        vals.append((ins.address, v))
        if not vals:
            nowrites += 1
        nwrites += len(vals)
        for a, v in vals:
            if not (-1 <= v < mx):
                bad.append({"owner": "0x%x" % o, "at": "0x%x" % a, "state": v, "maxState": mx})
    checks["V4_state_writes_in_range"] = {"owners": len(owner_max), "immediate_state_writes": nwrites,
                                         "byte_sized_writes": bytewrites,
                                         "owners_without_ebp_minus_4_immediates": nowrites,
                                         "out_of_range": len(bad), "pass": not bad}
    fails["V4_out_of_range"] = bad

    # ---------------- V5
    checks["V5_structural"] = {"problems": len(db["problems"]),
                               "kinds": dict(collections.Counter(p["kind"] for p in db["problems"])),
                               "pass": not db["problems"]}

    # ---------------- summary
    fis = db["funcinfo"]
    regs = [g for r in fis for g in r["registrations"]]
    owners_all = {g["owner"] for g in regs}
    unwind_entries = sum(r["maxState"] for r in fis)
    unwind_null = sum(1 for r in fis for to, a in r["unwind"] if not a)
    act_users = collections.Counter(a for r in fis for to, a in r["unwind"] if a)
    catches = [dict(c, funcinfo=r["va"], tryLow=t["tryLow"], tryHigh=t["tryHigh"], catchHigh=t["catchHigh"],
                    landing_owner=[g["owner"] for g in r["registrations"]])
               for r in fis for t in r["try"] for c in t["catches"]]
    throws = db["throws"]
    thrown_types = collections.Counter(t.get("type") for t in throws)
    caught_types = collections.Counter(c["type"] for c in catches)
    throw_owners = collections.Counter(t["owner"] for t in throws)
    summary = {
        "about": "Exception-handling database summary for %s, built by tools/eh/ehdb.py and "
                 "tools/eh/verify.py from the exe bytes and relocations (static only). The full database "
                 "is scratch/eh/%s/ehdb.json." % (db["exe"], tag),
        "exe": db["exe"],
        "thunks": db["thunks"],
        "cxx": {
            "funcinfo_records": len(fis),
            "magic": dict(collections.Counter(r["magic"] for r in fis)),
            "EHFlags": dict(collections.Counter(str(r["EHFlags"]) for r in fis)),
            "nIPMapEntries_nonzero": sum(1 for r in fis if r["nIPMapEntries"]),
            "pESTypeList_nonzero": sum(1 for r in fis if r["pESTypeList"] not in (None, "0x0")),
            "handler_stubs": nstub,
            "registration_sites": len(regs),
            "registration_forms": dict(collections.Counter(g["form"] for g in regs)),
            "distinct_owner_functions": len(owners_all),
            "funcinfo_shared_by_n_owners": dict(sorted(collections.Counter(
                len({g["owner"] for g in r["registrations"]}) for r in fis).items())),
            "maxState_histogram": dict(sorted(collections.Counter(
                min(r["maxState"], 33) for r in fis).items())),
            "maxState_max": max(r["maxState"] for r in fis),
            "unwind_entries": unwind_entries,
            "unwind_entries_without_action": unwind_null,
            "distinct_unwind_funclets": len(act_users),
            "unwind_funclets_shared_by_several_funcinfo": sum(1 for v in act_users.values() if v > 1),
            "funcinfo_with_try": sum(1 for r in fis if r["try"]),
            "try_blocks": sum(len(r["try"]) for r in fis),
            "catch_handlers": len(catches),
            "catch_all_handlers": sum(1 for c in catches if not c["typedesc"]),
            "caught_types": dict(caught_types),
            "catch_adjectives": dict(collections.Counter(c["adjectives"] for c in catches)),
            "catches": [{k: c[k] for k in ("funcinfo", "landing_owner", "tryLow", "tryHigh", "catchHigh",
                                           "type", "adjectives", "dispCatchObj", "handler", "continuations",
                                           "throws_new")} for c in catches],
            "throw_sites": len(throws),
            "throw_kinds": dict(collections.Counter(t["kind"] for t in throws)),
            "thrown_types": dict(thrown_types),
            "throw_owner_functions": len(throw_owners),
            "throwinfo": [{"va": t["va"], "attributes": t["attributes"], "destructor": t["destructor"],
                           "catchable": [{k: c[k] for k in ("type", "properties", "size", "copyFunction",
                                                            "thisDisp")} for c in t["catchable"]]}
                          for t in db["throwinfo"]],
            "throw_sites_by_region_64k": dict(sorted(collections.Counter(
                "0x%x" % (H(t["site"]) & ~0xFFFF) for t in throws).items())),
        },
        "seh": [{k: f.get(k) for k in ("kind", "form", "owner", "scopetable", "scopes")} for f in db["seh"]],
        "except_handler4_local": db["except_handler4_local"],
        "setjmp": db["setjmp"],
        "longjmp": db["longjmp"],
        "fs0_writes": db["fs0_writes"],
        "checks": checks,
        "failures": {k: v[:50] for k, v in fails.items() if v},
    }
    # resume-point table for the translator (sg_resume_points, docs/design/runtime-abi.md 10.3): the landing owner comes
    # from the EH frame that resumes there, not from the manifest's block ownership (see V3)
    table = []
    for kind, va, owners in sorted(resumes, key=lambda r: r[1]):
        table.append({"resume": "0x%x" % va, "kind": kind, "landing_owner": sorted("0x%x" % o for o in owners),
                      "manifest_owner": sorted("0x%x" % o for o in fi.owners(va))})
    rp = {"about": "Resume points (catch continuations, __except bodies, setjmp returns) and the function whose "
                   "translated C must contain each as a landing label. Built by tools/eh/verify.py.",
          "exe": db["exe"], "count": len(table), "landing_functions": checks["V3_resume_points"]["landing_functions"],
          "resume_points": table}
    if base:
        out = os.path.join(WORK, "scratch", "eh", "base", "funcinfo-summary.json")
        rpout = os.path.join(WORK, "scratch", "eh", "base", "resume-points.json")
    else:
        out = os.path.join(WORK, "data", "eh", "funcinfo-summary.json")
        rpout = os.path.join(WORK, "data", "eh", "resume-points.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(rpout, "w") as f:
        json.dump(rp, f, indent=1)
    with open(out, "w") as f:
        json.dump(summary, f, indent=1)
    print("wrote", out)
    for k, v in checks.items():
        print(k, "PASS" if v["pass"] else "FAIL", {a: b for a, b in v.items() if a != "pass"})


if __name__ == "__main__":
    main()

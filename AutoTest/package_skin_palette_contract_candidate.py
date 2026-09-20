"""CreateNew local candidate package; never installs, launches or uploads."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
def identity(path):
    with path.open("rb") as f:digest=hashlib.file_digest(f,"sha256").hexdigest().upper()
    return dict(size=path.stat().st_size,sha256=digest)
def require(ok,why):
    if not ok:raise RuntimeError(why)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sha",required=True);p.add_argument("--size",type=int,required=True)
    p.add_argument("--internal-defaults-receipt",type=Path,
                   help="Require a successful no-recorder-env runtime receipt for this DLL; package plain-launch instructions")
    p.add_argument("--output",type=Path,required=True);a=p.parse_args()
    dll=ROOT/"build32/src/d3d9/d3d9.dll";pin=dict(size=a.size,sha256=a.sha.upper())
    require(identity(dll)==pin,"DLL identity drift")
    with dll.open("rb") as f:
        header=f.read(64);require(header[:2]==b"MZ","DOS header")
        f.seek(struct.unpack_from("<I",header,60)[0]);pe=f.read(26)
        require(pe[:4]==b"PE\0\0" and struct.unpack_from("<H",pe,4)[0]==0x14c
                and struct.unpack_from("<H",pe,24)[0]==0x10b,"PE32/i386 required")
    out=a.output.resolve();archive=out.parent/(out.name+".zip")
    require(not out.exists() and not archive.exists(),"CreateNew target already exists")
    require(shutil.disk_usage(out.parent).free>100*1024*1024,"package disk headroom")
    require(not subprocess.check_output(["git","diff","--check"],cwd=ROOT,stderr=subprocess.DEVNULL),"whitespace")
    internal_defaults=a.internal_defaults_receipt is not None
    options=json.loads((ROOT/"build32/meson-info/intro-buildoptions.json").read_text(encoding="utf-8"))
    require([x["value"] for x in options if x["name"]=="warvk_internal_frame_recorder"]==[internal_defaults],
            "build profile and internal-default receipt mode must agree")
    if internal_defaults:
        require([x["value"] for x in options if x["name"]=="warvk_skin_palette_contract_candidate"]==[True],
                "internal player package requires compiled skin-palette contract default")
    selected={"d3d9.dll":dll,"README.md":ROOT/("docs/INTERNAL_RECORDER_CANDIDATE_README.md" if internal_defaults else "docs/SKIN_PALETTE_CANDIDATE_README.md")}
    if internal_defaults:
        receipt_path=a.internal_defaults_receipt.resolve()
        receipt=json.loads(receipt_path.read_text(encoding="utf-8"))
        before=json.loads((receipt_path.parent/"preflight.json").read_text(encoding="utf-8"))
        require(receipt.get("ok") is True and receipt.get("buildDefaults") is True and
                receipt.get("runCount")==1 and receipt.get("globalInputUsed") is False and
                receipt.get("watcherProcessesStarted")==0 and receipt.get("frameControlCommandsSent")==0,
                "successful internal-default runtime receipt required")
        require(before["candidate"]["size"]==pin["size"] and before["candidate"]["sha256"]==pin["sha256"],"receipt candidate mismatch")
        selected["internal-defaults-receipt.json"]=receipt_path
        selected["internal-defaults-preflight.json"]=receipt_path.parent/"preflight.json"
        selected["internal-defaults-env.json"]=receipt_path.parent/"env.json"
        selected["source/war3_frame_recorder_build.h"]=ROOT/"build32/src/d3d9/war3_frame_recorder_build.h"
    scripts=("launch_skin_palette_candidate.ps1","launch_frame_history_player.ps1","frame_history_watch.py",
             "frame_evidence_control.py","shadow_pose_full_trace_control.py","analyze_frame_evidence.py",
             "analyze_frame_history.py","analyze_frame_inputs.py","analyze_skin_palette_selection.py",
             "export_frame_history_video.py","test_skin_palette_contract.cpp",
             "test_skin_palette_contract_static.py","test_analyze_skin_palette_selection.py")
    sources=("src/d3d9/d3d9_device.cpp","src/d3d9/d3d9_war3_scene.h",
             "src/d3d9/war3/model/war3_model_hook.cpp","src/d3d9/war3/model/war3_model_hook.h",
             "src/d3d9/war3/render/war3_current_draw_contract.cpp","src/d3d9/war3/render/war3_current_draw_contract.h",
             "src/d3d9/war3/render/war3_canonical_draw.cpp","src/d3d9/war3/render/war3_canonical_draw.h",
             "src/d3d9/war3/render/war3_skin_palette_selection.h","src/d3d9/war3/tools/war3_frame_inputs.cpp",
             "src/d3d9/war3/render/war3_stage11_snapshot_page_policy.h",
             "src/d3d9/war3/tools/war3_frame_recorder_session.h",
             "src/d3d9/war3/tools/war3_frame_recorder_config.h","src/d3d9/war3/tools/war3_frame_recorder_profile.h",
             "src/d3d9/war3/tools/war3_frame_inputs_core.h","src/d3d9/war3/tools/war3_frame_recorder_build.h.in",
             "AutoTest/test_frame_recorder_defaults.cpp","meson_options.txt","src/d3d9/meson.build",
             "src/d3d9/war3/tools/war3_frame_history.h","src/d3d9/war3/tools/war3_frame_history.cpp",
             "src/d3d9/war3/tools/war3_frame_history_core.h",
             "src/d3d9/war3/tools/war3_frame_evidence.h","src/d3d9/war3/tools/war3_frame_evidence.cpp",
             "src/d3d9/war3/tools/war3_frame_evidence_core.h","src/d3d9/war3/tools/war3_frame_evidence_control.h",
             "AutoTest/test_frame_recorder_session.cpp","AutoTest/test_frame_evidence_runtime.cpp",
             "AutoTest/test_stage11_snapshot_publication.cpp",
             "docs/research/2026-09-15-recorder-owner-and-snapshot-publication.md",
             "docs/research/2026-09-15-skin-palette-publication-contract.md",
             "docs/agent-history/2026-09-16-high-pressure-fissure-evidence-candidate.md",
             "docs/agent-history/DEVELOPMENT_CHANGELOG.md")
    for name in scripts:selected["AutoTest/"+name]=ROOT/"AutoTest"/name
    for name in sources:selected["source/"+name]=ROOT/name
    out.mkdir();files={}
    for relative,src in selected.items():
        original=identity(src);dst=out/relative;dst.parent.mkdir(parents=True,exist_ok=True)
        with src.open("rb") as f,dst.open("xb") as g:shutil.copyfileobj(f,g)
        require(identity(src)==original and identity(dst)==original,"copy mismatch")
        files[relative]=original
    manifest=dict(schema=1,productAccepted=False,gameplayValidated=False,rootCauseReady=False,
                  internalRecorderDefault=internal_defaults,skinPaletteContractDefault=internal_defaults,
                  highPressureEvidenceProfile=internal_defaults,requiresRecorderLauncher=not internal_defaults,
                  defaultLauncherGate=1,legacyComparisonGate=0,
                  gitHead=subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(),
                  files=files)
    with (out/"candidate-manifest.json").open("x",encoding="utf-8") as f:json.dump(manifest,f,ensure_ascii=False,indent=2)
    with zipfile.ZipFile(archive,"x",compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for file in sorted(out.rglob("*")):
            if file.is_file():z.write(file,file.relative_to(out).as_posix())
    with zipfile.ZipFile(archive) as z:
        require(z.testzip() is None,"ZIP CRC")
        for rel,expected in files.items():
            data=z.read(rel)
            require(len(data)==expected["size"] and hashlib.sha256(data).hexdigest().upper()==expected["sha256"],"ZIP member hash")
    require(identity(dll)==pin,"source changed during packaging")
    print(json.dumps(dict(package=str(out),zip=str(archive),identity=identity(archive),dll=pin,files=len(files)),ensure_ascii=False))
if __name__=="__main__":main()

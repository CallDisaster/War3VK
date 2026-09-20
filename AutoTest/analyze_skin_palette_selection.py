"""Additional provenance validation. Never certifies visual repair or native slot ownership."""
import argparse
import json
from collections import Counter
from pathlib import Path
from analyze_frame_evidence import load, require, u64, uint, analyze
from analyze_frame_inputs import analyze_inputs

FIELDS = set("schema source space domain runtimeModel part meshPayload ownerEpoch publicationTicket captureSerial hash slotAllocationGeneration slot actualGroupCount frameTag".split())
def validate_selection(s, part, used):
    require(type(s) is dict and set(s)==FIELDS, "skin selection fields")
    require(s["schema"]==1 and type(s["schema"]) is int, "skin selection schema")
    for name in ("source","space","domain","slot","actualGroupCount","frameTag"):
        uint(s[name],2**32-1)
    for name in FIELDS-{"schema","source","space","domain","slot","actualGroupCount","frameTag"}:
        u64(s[name])
    require(s["source"]<=7 and s["space"]<=2 and s["domain"]<=2, "skin enums")
    require(u64(s["slotAllocationGeneration"])==0, "unsupported native allocation witness")
    if not used:
        require(s["source"]==0, "non-consumed semantic selection must be cleared")
        return "not-consumed"
    require(s["source"] in (1,3) and s["space"]==1 and s["domain"]==1, "unproved consumed palette")
    require(u64(s["part"])==u64(part) and u64(part)>0, "part mismatch")
    require(u64(s["hash"])>0 and 0<s["actualGroupCount"]<=256 and s["frameTag"]>0, "palette extent or freshness")
    if s["source"]==1:
        require(u64(s["captureSerial"])>0 and u64(s["meshPayload"])>0, "writer witness absent")
        return "captured-writer"
    require(all(u64(s[k])>0 for k in ("runtimeModel","ownerEpoch","publicationTicket")), "producer publication absent")
    require(s["slot"]<0x3a98 and s["actualGroupCount"]<=64, "owned snapshot bound")
    return "owned-part"

def summarize(inputs, cpu):
    counts=Counter(); rows=[]
    for batch in inputs["batches"]:
        for draw in batch["draws"]:
            # Only the semantic producer carries this source contract. Other
            # producers and a successful native snapshot override must say unused.
            used=(u64(draw["provenance"][0])==1 and draw["words"][15]!=0
                  and not (u64(draw["provenance"][2]) & (1<<16)))
            kind=validate_selection(draw.get("skinPaletteSelection"),draw["part"],used)
            counts[kind]+=1
            if used:
                rows.append(dict(frame=batch["frame"],volume=batch["volume"],draw=draw["index"],
                                 handle=draw["words"][1],selection=draw["skinPaletteSelection"]))
    reasons=Counter(); decisions=0
    for event in cpu["events"]:
        if event.get("label")!="skin-selection/v1":continue
        require(event["kind"]==12, "skin decision kind")
        reason=u64(event["data"][11]);require(reason<=8,"canonical rejection enum")
        decisions+=1
        if reason:reasons[str(reason)]+=1
    require(decisions>0,"no strict selection decisions recorded")
    return dict(schema=1,sourceContractValid=True,visualRepairProven=False,rootCauseReady=False,
                decisionEvents=decisions,sourceCounts=dict(counts),rejectionsByCanonicalReason=dict(reasons),
                draws=rows,limitations=["CPU publication epoch/ticket are NOT native slot allocation ownership.",
                    "Safe omission is not proof of repaired geometry; inspect rejection counts and visible shadow coverage.",
                    "Input reconstruction and pixel association remain separate checks."])

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("manifest","binary","cpu","output"):p.add_argument("--"+name,type=Path,required=True)
    a=p.parse_args();inputs=load(a.manifest);cpu=load(a.cpu)
    analyze(cpu)
    numeric=analyze_inputs(inputs,a.binary.read_bytes(),cpu)
    result=summarize(inputs,cpu)
    result["reconstructedDraws"]=numeric["reconstructedDraws"]
    result["captureDrops"]=numeric["captureDrops"]
    with a.output.open("x",encoding="utf-8") as f:json.dump(result,f,ensure_ascii=False,indent=2)
    print(json.dumps({k:v for k,v in result.items() if k!="draws"},ensure_ascii=False))
if __name__=="__main__":main()

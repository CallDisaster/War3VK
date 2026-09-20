import copy
import unittest
from analyze_skin_palette_selection import validate_selection, summarize

def fixture():
    return dict(schema=1,source=3,space=1,domain=1,runtimeModel="1",part="2",meshPayload="0",
                ownerEpoch="3",publicationTicket="4",captureSerial="0",hash="5",
                slotAllocationGeneration="0",slot=6,actualGroupCount=9,frameTag=8)
class SelectionReader(unittest.TestCase):
    def test_owned(self):self.assertEqual(validate_selection(fixture(),"2",True),"owned-part")
    def test_captured(self):
        s=fixture();s.update(source=1,captureSerial="12",meshPayload="13")
        self.assertEqual(validate_selection(s,"2",True),"captured-writer")
    def test_bad_fields(self):
        s=fixture();s["extra"]=1
        with self.assertRaises(ValueError):validate_selection(s,"2",True)
    def test_fail_closed_mutations(self):
        for key,value in (("source",0),("source",2),("source",4),("space",2),("domain",2),
                          ("runtimeModel","0"),("ownerEpoch","0"),("publicationTicket","0"),
                          ("part","7"),("hash","0"),("actualGroupCount",0),("actualGroupCount",65),
                          ("frameTag",0),("slot",0x3a98),("slotAllocationGeneration","1"),("hash",5)):
            with self.subTest(key=key,value=value):
                s=fixture();s[key]=value
                with self.assertRaises(ValueError):validate_selection(s,"2",True)
    def test_unused_must_not_masquerade(self):
        with self.assertRaises(ValueError):validate_selection(fixture(),"2",False)
    def test_rejections_not_visual_proof(self):
        event=dict(label="skin-selection/v1",kind=12,data=["0"]*11+["4"])
        out=summarize(dict(batches=[]),dict(events=[event]))
        self.assertEqual(out["rejectionsByCanonicalReason"],{"4":1})
        self.assertFalse(out["visualRepairProven"])
    def test_missing_decisions(self):
        with self.assertRaises(ValueError):summarize(dict(batches=[]),dict(events=[]))
    def test_inconsistent_decision(self):
        with self.assertRaises(ValueError):
            summarize(dict(batches=[]),dict(events=[dict(label="skin-selection/v1",kind=9,data=["0"]*11+["4"])]))
if __name__=="__main__":unittest.main()

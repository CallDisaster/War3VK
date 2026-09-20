"""Test measurement mechanics, never claim graphics acceptance from image deltas."""
import unittest
import numpy as np
from analyze_player_fissure_pairs_20260914 import components

class PixelEvidenceTests(unittest.TestCase):
    def test_identical(self):
        delta=np.zeros((64,64),np.float32)
        self.assertEqual(components(delta>32,delta),[])

    def test_added_dark_rectangle(self):
        delta=np.zeros((64,64),np.float32)
        delta[11:31,20:40]=-180
        row=components(delta < -32,delta)[0]
        self.assertEqual(row['bbox'],[20,11,40,31])
        self.assertEqual(row['area'],400)
        self.assertEqual(row['meanSignedGrayDelta'],-180)
        self.assertEqual(components(delta>32,delta),[])

    def test_removed_dark_rectangle(self):
        delta=np.zeros((64,64),np.float32)
        delta[11:31,20:40]=60
        row=components(delta>32,delta)[0]
        self.assertEqual(row['area'],400)
        self.assertEqual(row['meanSignedGrayDelta'],60)
        self.assertEqual(components(delta < -32,delta),[])

    def test_isolated_noise_is_not_area(self):
        delta=np.zeros((64,64),np.float32)
        delta[::4,::4]=255
        self.assertEqual(components(delta>32,delta),[])

    def test_strict_threshold(self):
        delta=np.full((20,20),32,np.float32)
        self.assertEqual(components(delta>32,delta),[])

if __name__=='__main__':
    unittest.main()

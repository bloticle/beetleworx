"""Golden pixels catch Wii-specific channel, alpha and block-layout mistakes."""
import unittest
from em1_texture_upgrade import decode_gx
class WiiPixelTests(unittest.TestCase):
    def test_cmpr_does_not_use_pc_bc1_interpolation(self):
        # Red > blue: selection2 is5/8 red+3/8 blue, not PC's2/3 blend.
        block=bytes.fromhex("f800001faaaaaaaa")*4
        self.assertEqual(decode_gx(block,8,8,"CMPR")[:4],bytes([159,0,95,255]))
    def test_cmpr_transparent_texels_keep_color(self):
        block=bytes.fromhex("001ff800ffffffff")*4
        self.assertEqual(decode_gx(block,8,8,"CMPR")[:4],bytes([127,0,128,0]))
    def test_subblocks_and_tiles(self):
        red=bytes.fromhex("f800000000000000");blue=bytes.fromhex("001f000000000000")
        data=decode_gx(red+blue+blue+red+blue*4,16,8,"CMPR")
        self.assertEqual(data[:4],bytes([255,0,0,255]))
        self.assertEqual(data[4*4:5*4],bytes([0,0,255,255]))
        self.assertEqual(data[8*4:9*4],bytes([0,0,255,255]))
        self.assertEqual(data[(4*16+4)*4:(4*16+5)*4],bytes([255,0,0,255]))
    def test_channel_formats(self):
        cases=[("I4",bytes([0xA3])*32,[170]*4),("I8",bytes([0x42])*32,[66]*4),
               ("IA4",bytes([0xA3])*32,[51,51,51,170]),("IA8",bytes([0x12,0x34])*16,[52,52,52,18]),
               ("RGB565",bytes.fromhex("07e0")*16,[0,255,0,255]),
               ("RGB5A3",bytes.fromhex("7123")*16,[17,34,51,255]),
               ("RGBA8",bytes([0x12,0x34])*16+bytes([0x56,0x78])*16,[52,86,120,18])]
        for mode,raw,want in cases:
            with self.subTest(mode=mode):self.assertEqual(decode_gx(raw,1,1,mode),bytes(want))
    def test_palette_and_bounds(self):
        self.assertEqual(decode_gx(bytes(32),1,1,"C8",[bytes([1,2,3,4])]),bytes([1,2,3,4]))
        with self.assertRaises(ValueError):decode_gx(bytes(31),1,1,"CMPR")
        with self.assertRaises(ValueError):decode_gx(bytes(32),0,1,"CMPR")
if __name__=="__main__":unittest.main()

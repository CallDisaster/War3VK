import struct
import unittest
from unittest.mock import patch
from analyze_native_oom_dump import parse_copy


class NativeDumpTest(unittest.TestCase):
    def test_private_stream_only_changes_memory_copy(self):
        raw = bytearray(48)
        raw[:4] = b'MDMP'
        struct.pack_into('<II', raw, 8, 1, 32)
        struct.pack_into('<III', raw, 32, 4096, 4, 44)
        raw[44:48] = b'test'
        original = bytes(raw)
        with patch('analyze_native_oom_dump.MinidumpFile.parse_bytes', return_value='parsed') as parser:
            result, streams, ignored = parse_copy(raw)
        self.assertEqual(raw, original)
        self.assertEqual(result, 'parsed')
        self.assertEqual(ignored, streams)
        changed = parser.call_args.args[0]
        self.assertEqual(changed[44:], b'test')
        self.assertEqual(struct.unpack_from('<I', changed, 32)[0], 0)

    def test_bad_header(self):
        for raw in (b'', b'BAD!' + bytes(40), b'MDMP' + bytes(8)):
            with self.assertRaises(ValueError): parse_copy(raw)

    def test_bad_directory(self):
        for count, offset in ((257,32),(1,4),(1,44),(0,1000)):
            raw = bytearray(48); raw[:4] = b'MDMP'
            struct.pack_into('<II',raw,8,count,offset)
            with self.assertRaises(ValueError): parse_copy(raw)

    def test_truncated_stream(self):
        raw = bytearray(48); raw[:4] = b'MDMP'
        struct.pack_into('<II',raw,8,1,32)
        struct.pack_into('<III',raw,32,4096,100,44)
        with self.assertRaises(ValueError): parse_copy(raw)


if __name__ == '__main__': unittest.main()

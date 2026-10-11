#!/usr/bin/env python3
"""Behavioral checks using Pillow as an independent pixel decoder.
Run: python3 tests.py ./jpegtool
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
from PIL import Image


def segments(path):
    """Read marker payloads, skipping stuffed bytes and restart markers in scans."""
    data = Path(path).read_bytes()
    if data[:2] != b'\xff\xd8':
        raise ValueError('not a JPEG')
    i = 2
    result = []
    while i < len(data):
        assert data[i] == 255
        while data[i] == 255:
            i += 1
        marker = data[i]
        i += 1
        if marker == 0xd9:
            return result
        if marker == 0x01 or 0xd0 <= marker <= 0xd7:
            continue
        length = int.from_bytes(data[i:i+2], 'big')
        assert length >= 2 and i + length <= len(data)
        result.append((marker, data[i+2:i+length]))
        i += length
        if marker == 0xda:
            while i < len(data):
                if data[i] != 255:
                    i += 1
                    continue
                j = i + 1
                while data[j] == 255:
                    j += 1
                if data[j] == 0 or 0xd0 <= data[j] <= 0xd7:
                    i = j + 1
                else:
                    break
    raise ValueError('missing EOI')


def check_huffman(path):
    for marker, payload in segments(path):
        if marker != 0xc4:
            continue
        pos = 0
        while pos < len(payload):
            counts = payload[pos+1:pos+17]
            symbols = sum(counts)
            assert len(counts) == 16 and 0 < symbols <= 256
            code = 0
            for length, count in enumerate(counts, 1):
                if count:
                    assert code + count - 1 < (1 << length) - 1, 'all-ones or invalid code'
                code = (code + count) << 1
            pos += 17 + symbols
        assert pos == len(payload)


def metadata(path):
    return [(m, p) for m, p in segments(path) if 0xe0 <= m <= 0xef or m == 0xfe]


def pixels(path):
    with Image.open(path) as im:
        return np.asarray(im).copy()


def run(tool, args, ok=True):
    result = subprocess.run([tool, *map(str, args)], text=True, capture_output=True)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result


def main():
    tool = str(Path(sys.argv[1] if len(sys.argv) > 1 else './jpegtool').resolve())
    rng = np.random.default_rng(2468)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        cases = 0
        # Odd image dimensions exercise interleaved MCU edge padding.
        for mode, shape in [('RGB', (37, 53, 3)), ('L', (17, 29)), ('CMYK', (19, 35, 4))]:
            data = rng.integers(0, 256, shape, dtype=np.uint8)
            im = Image.fromarray(data, mode=mode)
            for progressive in (False, True):
                for restart in (0, 3):
                    if mode == 'L':
                        sampling = (0,)
                    else:
                        sampling = (0, 1, 2)
                    for sub in sampling:
                        before, after = root/'before.jpg', root/'after.jpg'
                        im.save(before, quality=88, subsampling=sub, progressive=progressive,
                                restart_marker_blocks=restart, comment=b'JPEG-LAB comment',
                                exif=b'Exif\x00\x00minimal-test', icc_profile=b'profile-test-data')
                        run(tool, ['optimize', before, after])
                        run(tool, ['verify', before, after])
                        assert np.array_equal(pixels(before), pixels(after))
                        assert metadata(before) == metadata(after)
                        assert Image.open(before).quantization == Image.open(after).quantization
                        assert Image.open(after).info.get('progressive', False) == progressive
                        check_huffman(after)
                        if restart:
                            assert (0xdd, restart.to_bytes(2, 'big')) in segments(after)
                        cases += 1
        before, after = root/'before.jpg', root/'after.jpg'
        im = Image.fromarray(rng.integers(0, 256, (61, 79, 3), dtype=np.uint8))
        im.save(before, quality=92, subsampling=2)
        run(tool, ['standard', before, after])
        run(tool, ['verify', before, after])
        assert np.array_equal(pixels(before), pixels(after))
        assert Image.open(before).quantization == Image.open(after).quantization
        assert metadata(before) == metadata(after)
        run(tool, ['optimize', after, root/'reoptimized.jpg'])
        assert (root/'reoptimized.jpg').stat().st_size <= after.stat().st_size
        for factor in (1, 1.5, 2, 4):
            run(tool, ['requant', before, after, 'scale', factor])
            q1, q2 = Image.open(before).quantization, Image.open(after).quantization
            for t in q1:
                assert q2[t] == [min(255, max(1, int(x*factor+0.5))) for x in q1[t]]
            assert pixels(after).shape == pixels(before).shape
            assert metadata(before) == metadata(after)
            check_huffman(after)
            run(tool, ['verify', before, after], ok=factor == 1)
        run(tool, ['requant', before, after, 'quality', 65])
        reference = root/'reference.jpg'
        im.save(reference, quality=65)
        assert Image.open(after).quantization == Image.open(reference).quantization
        run(tool, ['requant', before, after, 'mpeg4', 1])
        assert Image.open(after).quantization[0][0] == 8
        assert Image.open(after).quantization[0][-1] == 45
        custom = root/'tables.txt'
        custom.write_text(' '.join(['16']*64 + ['32']*64))
        run(tool, ['requant', before, after, 'table', custom])
        assert Image.open(after).quantization == {0: [16]*64, 1: [32]*64}

        # Independently check a constant grayscale block's reconstructed DC.
        # q=1 => DC=-64 for sample value 120. New DC step 128 gives a
        # negative half tie => DC=-1 => decoded value 112, not 128.
        tie = root/'tie.jpg'
        Image.new('L', (8, 8), 120).save(tie, quality=100)
        custom.write_text(' '.join(['128'] + ['1']*63))
        run(tool, ['requant', tie, after, 'table', custom])
        assert np.all(pixels(after) == 112)

        digest = before.read_bytes()
        run(tool, ['optimize', before, before], ok=False)
        assert before.read_bytes() == digest
        truncated = root/'truncated.jpg'
        truncated.write_bytes(digest[:-20])
        keep = after.read_bytes()
        run(tool, ['optimize', truncated, after], ok=False)
        assert after.read_bytes() == keep
        for invalid in ('nan', 'inf', '0', '-1', '1xyz'):
            run(tool, ['requant', before, after, 'scale', invalid], ok=False)
            assert after.read_bytes() == keep
        run(tool, ['requant', before, after, 'quality', '65.5'], ok=False)
        # Repeated optimization of the generated baseline should stabilize.
        once, twice = root/'once.jpg', root/'twice.jpg'
        run(tool, ['optimize', before, once])
        run(tool, ['optimize', once, twice])
        assert once.read_bytes() == twice.read_bytes()
        custom.write_text(' '.join(['256']*64))
        run(tool, ['requant', before, after, 'table', custom], ok=False)
        assert not list(root.glob('*.tmp.*'))
        print(f'PASS: {cases} lossless cases; requantization, negative ties, metadata, DHT, restart and error-path checks')


if __name__ == '__main__':
    main()

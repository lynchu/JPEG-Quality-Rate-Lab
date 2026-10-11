# JPEG Huffman optimization and DCT requantization

`main.c` is a complete C11 command-line program. It uses the public libjpeg API
for JPEG parsing, coefficient storage, Huffman construction and entropy coding.
The quantizer selection and signed integer requantization are implemented here.

## Build

On Ubuntu/Debian:

```bash
sudo apt install build-essential libjpeg-dev
make
```

Equivalent direct build:

```bash
cc -O3 -std=c11 -Wall -Wextra -Wpedantic main.c -ljpeg -lm -o jpegtool
```

This program targets Linux/POSIX (temporary files, atomic rename, monotonic
clock). The provided binary was built on Linux x86-64 against libjpeg-turbo
2.1.5 with the libjpeg v8 ABI; rebuilding on your computer is recommended.
No vendored JPEG headers are needed when the development package is installed.

## Part 1: lossless rate reduction

```bash
./jpegtool inspect camera.jpg
./jpegtool optimize camera.jpg camera_optimized.jpg
./jpegtool verify camera.jpg camera_optimized.jpg
```

The tool preserves quantization tables, sampling and every real DCT coefficient.
It gathers statistics of the symbols actually emitted by the output encoder,
builds JPEG-compatible Huffman tables, emits DHT, and re-encodes the entropy
stream. This is not a byte-level DHT patch. The report gives input/output bytes,
percentage reduction, whole-file bits per pixel, and elapsed time.

`verify` checks geometry, sampling, color space, per-component quantization and
all real DCT coefficients. It does not compare APP/COM metadata or decoder
pixels; the Python tests check those independently.

The `standard` command creates a controlled comparison using default Huffman
tables, while preserving DQT and coefficients:

```bash
./jpegtool standard examples/photograph_original.jpg control.jpg
./jpegtool optimize control.jpg control_optimized.jpg
./jpegtool verify control.jpg control_optimized.jpg
```

Optimization does not promise a smaller file for every input. Existing tables
may already fit the image, and marker/scan overhead affects total size. The
program reports negative reduction if the result grows.

## Part 2: adjust quantization and rate

```bash
# Multiply the existing DQT steps by 2, keeping the original matrix shapes.
./jpegtool requant camera.jpg camera_smaller.jpg scale 2

# Use libjpeg's Annex-K-based quality setting, from 1 to 100.
./jpegtool requant camera.jpg camera_q75.jpg quality 75

# Adapt the MPEG-4 intra matrix as JPEG quantizer steps.
./jpegtool requant camera.jpg camera_mpeg4.jpg mpeg4 1

# Supply your own luminance/chrominance tables.
./jpegtool requant camera.jpg camera_custom.jpg table quant_tables/mpeg4_intra.txt
```

For each frequency coefficient:

```text
approximate_dequantized_coefficient = old_coefficient * old_quantizer
new_coefficient = round(approximate_dequantized_coefficient / new_quantizer)
```

Nearest-integer ties are rounded away from zero. All coefficient arithmetic uses
64-bit integers. New DQT steps are restricted to 1..255. The encoder always
recomputes Huffman statistics after requantization.

`scale` and `mpeg4` accept factors in 0.01..1000, with resulting steps clamped to
1..255. For `mpeg4`, the factor scales the provided MPEG-4 matrix rather than the
input DQT. It adapts the matrix shape only; JPEG quantization is retained, and
the output is a JPEG file. It does not implement MPEG-4 quantization or create
an MPEG-4 video.

Custom files contain exactly 64 or 128 whitespace-separated integers, each in
1..255. Their order is natural row-major 8x8, not zigzag. With 64 values the same
matrix is used for all components; with 128 values, the first matrix is for Y
and the second for Cb/Cr. Comments are not accepted in the table file.

A smaller quantization step cannot restore information lost in the input JPEG.
If refinement would exceed the program's conservative 8-bit DCT coefficient
limits, it rejects the operation instead of silently clipping coefficients.

## Input and metadata behavior

- Supports 8-bit Huffman-coded DCT JPEG with DQT values in 1..255.
- `optimize`, `inspect`, `verify`, and `scale` handle grayscale, RGB, YCbCr,
  CMYK and YCCK if the linked library supports the input.
- `quality`, `mpeg4`, and custom tables require grayscale or YCbCr input.
- Sequential output uses one interleaved scan; multi-scan sequential inputs may
  therefore change scan layout. The controlled measurements use single-scan
  baseline input.
- Progressive input remains progressive, using libjpeg's default scan script.
  Its size change can include scan-layout effects. `standard` requires
  sequential input.
- Restart interval, image dimensions, component sampling and APP/COM payloads
  are preserved. APP markers include EXIF and ICC data. Embedded thumbnails and
  metadata fields are copied verbatim, not recalculated.
- Strict JPEG warnings reject damaged/truncated files. Unsupported JPEG modes
  and incomplete standalone files are rejected by the library or header checks.
- Input/output must be distinct files. Output is written to a temporary file
  beside the destination, then renamed after success. An existing destination
  is replaced only after encoding and file close succeed.
- `inspect` reports the tables retained after all scans are read. Progressive
  files may redefine DHT across scans; this is not an exhaustive per-scan dump.

## Test and reproduce measurements

```bash
python3 -m pip install Pillow numpy matplotlib
make test
python3 benchmark.py examples/photograph_default.jpg --tool ./jpegtool --out rerun
```

The archive includes the sample input, output JPEGs, measurements CSV, comparison
figure, rate–distortion plot, and `REPORT.md`. PSNR is measured against the
decoded input JPEG, not the unavailable uncompressed source photograph.

For your own camera image:

```bash
python3 benchmark.py camera.jpg --tool ./jpegtool --out camera_results
```

Use an untouched JPEG copied directly from the camera. A file exported by an
editor or messaging app measures that software's JPEG encoding.

## Source map

| Function | Responsibility |
|---|---|
| `open_source` / `check_header` | Save metadata, read and validate JPEG headers |
| `transcode` | Read coefficients, configure output, optimize and re-encode |
| `select_quantizer` | Input scaling, quality preset, MPEG-4 shape, custom DQT |
| `requantize` | Apply signed integer coefficient rescaling |
| `inspect` | Print DQT and DHT information |
| `verify` | Check lossless coefficient preservation |
| `open_output` / `cleanup` | Safe temporary output and error cleanup |

References and experimental conclusions are in `REPORT.md`.

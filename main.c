#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <jpeglib.h>

/* JPEG coefficient-domain transcoder. libjpeg supplies the JPEG parser,
 * entropy coder and Annex-K-style statistics-based Huffman optimization.
 * This file implements CLI, quantizer selection, integer requantization,
 * marker preservation, inspection, and coefficient equality verification. */

typedef struct
{
    struct jpeg_error_mgr base;
    jmp_buf *trap;
} Error;
typedef struct
{
    struct jpeg_decompress_struct src, second;
    struct jpeg_compress_struct dst;
    Error se, de, ve;
    jmp_buf trap;
    FILE *input, *output, *other;
    int src_created, dst_created, second_created;
    char *temporary;
} Work;

/* Uses the MPEG-4 intra matrix’s shape as JPEG quantization steps. */
static const unsigned int mpeg4_intra[64] = {
    8, 17, 18, 19, 21, 23, 25, 27, 17, 18, 19, 21, 23, 25, 27, 28,
    20, 21, 22, 23, 24, 26, 28, 30, 21, 22, 23, 24, 26, 28, 30, 32,
    22, 23, 24, 26, 28, 30, 32, 35, 23, 24, 26, 28, 30, 32, 35, 38,
    25, 26, 28, 30, 32, 35, 38, 41, 27, 28, 30, 32, 35, 38, 41, 45};

/*
    Error handling and cleanup
*/
static void fail(Work *w, const char *message)
{
    fprintf(stderr, "error: %s\n", message);
    longjmp(w->trap, 1);
}

static void jpeg_failure(j_common_ptr c)
{
    /* j_common_ptr is a pointer type for the common fields shared by libjpeg’s
       compression and decompression objects. */
    Error *e = (Error *)c->err;
    (*c->err->output_message)(c);
    longjmp(*e->trap, 1);
}

static void strict_message(j_common_ptr c, int level)
{
    /* Reject truncated or damaged files instead of silently repairing them. */
    if (level < 0)
        jpeg_failure(c);
}

static void init_error(Error *e, jmp_buf *trap)
{
    jpeg_std_error(&e->base);
    e->base.error_exit = jpeg_failure;
    e->base.emit_message = strict_message;
    e->trap = trap;
}

static void cleanup(Work *w)
{
    if (w->dst_created)
        jpeg_destroy_compress(&w->dst);
    if (w->src_created)
        jpeg_destroy_decompress(&w->src);
    if (w->second_created)
        jpeg_destroy_decompress(&w->second);
    if (w->input)
        fclose(w->input);
    if (w->output)
        fclose(w->output);
    if (w->other)
        fclose(w->other);
    if (w->temporary)
    {
        unlink(w->temporary);
        free(w->temporary);
    }
    free(w);
}

/*
    Timing, file size, numeric parsing, and header checks
*/
static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int64_t filesize(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static double number(Work *w, const char *s, double lo, double hi)
{
    char *end;
    errno = 0;
    double x = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(x) || x < lo || x > hi)
        fail(w, "numeric argument is invalid or outside the allowed range");
    return x;
}

static void check_header(Work *w, j_decompress_ptr s)
{
    if (s->data_precision != 8 || s->arith_code)
        fail(w, "only 8-bit Huffman-coded DCT JPEG input is supported");

    for (int c = 0; c < s->num_components; ++c)
    {
        // Obtains the component’s quantization-table identifier.
        int t = s->comp_info[c].quant_tbl_no;
        if (t < 0 || t >= NUM_QUANT_TBLS || !s->quant_tbl_ptrs[t])
            fail(w, "missing or invalid quantization table");
        for (int k = 0; k < 64; ++k)
            if (s->quant_tbl_ptrs[t]->quantval[k] < 1 ||
                s->quant_tbl_ptrs[t]->quantval[k] > 255)
                fail(w, "this tool requires quantization steps in 1..255");
    }
}

/* Performs the standard setup for reading a JPEG. */
static void open_source(Work *w, const char *path)
{
    w->input = fopen(path, "rb");
    if (!w->input)
        fail(w, "cannot open input JPEG");

    init_error(&w->se, &w->trap);
    w->src.err = &w->se.base;

    w->src_created = 1;
    jpeg_create_decompress(&w->src);
    jpeg_stdio_src(&w->src, w->input);

    for (int n = 0; n < 16; ++n)
        jpeg_save_markers(&w->src, JPEG_APP0 + n, 0xFFFF); // JPEG application marker
    jpeg_save_markers(&w->src, JPEG_COM, 0xFFFF);          // comment
    jpeg_read_header(&w->src, TRUE);

    check_header(w, &w->src);
}

static void inspect(j_decompress_ptr s)
{
    // Image properties.
    printf("image=%ux%u components=%d coding=%s restart_mcus=%u\n",
           s->image_width, s->image_height, s->num_components,
           s->progressive_mode ? "progressive" : "sequential", s->restart_interval);

    // Component’s sampling factors and quantization-table assignment
    for (int c = 0; c < s->num_components; ++c)
    {
        jpeg_component_info *p = &s->comp_info[c];
        printf("component=%d sampling=%dx%d quant_table=%d\n",
               c, p->h_samp_factor, p->v_samp_factor, p->quant_tbl_no);
    }

    // Although DQT values are serialized in zigzag order in a JPEG file,
    // libjpeg presents them in natural order.
    for (int t = 0; t < NUM_QUANT_TBLS; ++t)
        if (s->quant_tbl_ptrs[t])
        {
            printf("DQT[%d] (natural row-major order):\n", t);
            for (int k = 0; k < 64; ++k)
                printf("%3u%c", s->quant_tbl_ptrs[t]->quantval[k], k % 8 == 7 ? '\n' : ' ');
        }

    // Huffman tables.
    for (int ac = 0; ac < 2; ++ac)
        for (int t = 0; t < NUM_HUFF_TBLS; ++t)
        {
            JHUFF_TBL *h = ac ? s->ac_huff_tbl_ptrs[t] : s->dc_huff_tbl_ptrs[t];
            if (!h)
                continue;

            printf("DHT[%s,%d] counts_by_length=", ac ? "AC" : "DC", t);
            int count = 0;
            for (int k = 1; k <= 16; ++k)
            {
                // bits[k] is number of symbols assigned a code of length k.
                printf("%s%u", k == 1 ? "" : ",", h->bits[k]);
                count += h->bits[k];
            }

            printf(" symbols=");
            for (int k = 0; k < count; ++k)
                printf("%s%02x", k ? "," : "", h->huffval[k]);
            puts("");
        }
}

static void load_tables(Work *w, const char *path, unsigned int values[128], int *count)
{
    w->other = fopen(path, "r");
    if (!w->other)
        fail(w, "cannot open custom quantization table file");

    char token[128];
    *count = 0;
    while (fscanf(w->other, "%127s", token) == 1)
    {
        if (*count == 128)
            fail(w, "table file must contain exactly 64 or 128 integers");
        double v = number(w, token, 1, 255);
        if (v != floor(v))
            fail(w, "quantization steps must be integers");
        values[(*count)++] = (unsigned int)v;
    }
    if (ferror(w->other))
        fail(w, "error reading table file");
    fclose(w->other);
    w->other = NULL;
    if (*count != 64 && *count != 128)
        fail(w, "table file must contain exactly 64 or 128 integers");
}

static unsigned int scaled_step(unsigned int old, double scale)
{
    double q = floor((double)old * scale + 0.5);
    return q < 1 ? 1 : q > 255 ? 255
                               : (unsigned int)q;
}

/* Choosing new tables. */
static void select_quantizer(Work *w, const char *kind, const char *argument)
{
    j_compress_ptr d = &w->dst;
    j_decompress_ptr s = &w->src;

    // SCALE mode: retain the encoder’s original frequency weighting
    if (!strcmp(kind, "scale"))
    {
        double scale = number(w, argument, 0.01, 1000);
        for (int t = 0; t < NUM_QUANT_TBLS; ++t)
            if (s->quant_tbl_ptrs[t])
            {
                for (int k = 0; k < 64; ++k)
                    d->quant_tbl_ptrs[t]->quantval[k] = scaled_step(s->quant_tbl_ptrs[t]->quantval[k], scale);
                d->quant_tbl_ptrs[t]->sent_table = FALSE;
            }
        return;
    }

    if (s->jpeg_color_space != JCS_GRAYSCALE && s->jpeg_color_space != JCS_YCbCr)
        fail(w, "quality, mpeg4 and custom table modes require grayscale or YCbCr input");

    /* New table 0 is luminance, table 1 is chrominance. */
    for (int c = 0; c < d->num_components; ++c)
        d->comp_info[c].quant_tbl_no = c ? 1 : 0;

    // QUALITY mode: Quality 50 uses the base matrices; quality 75 approximately halves their entries.
    // for q<50,  scale = floor(5000/q)
    //     q>=50, scale = 200 - 2q
    if (!strcmp(kind, "quality"))
    {
        double q = number(w, argument, 1, 100);
        if (q != floor(q))
            fail(w, "quality must be an integer in 1..100");
        jpeg_set_quality(d, (int)q, TRUE);
    }
    else
    {
        unsigned int custom[128];
        int count = 0;
        double scale = 1;
        // MPEG mode: scales the embedded mpeg4_intra[] matrix.
        if (!strcmp(kind, "mpeg4"))
            scale = number(w, argument, 0.01, 1000);
        else if (!strcmp(kind, "table")) // TABLE mode: loads the user-supplied values through load_tables().
            load_tables(w, argument, custom, &count);
        else
            fail(w, "quantizer must be scale, quality, mpeg4 or table");

        for (int t = 0; t < (d->num_components == 1 ? 1 : 2); ++t)
        {
            if (!d->quant_tbl_ptrs[t])
                d->quant_tbl_ptrs[t] = jpeg_alloc_quant_table((j_common_ptr)d);
            for (int k = 0; k < 64; ++k)
                d->quant_tbl_ptrs[t]->quantval[k] = (UINT16)(count ? custom[(count == 128 ? t * 64 : 0) + k] : scaled_step(mpeg4_intra[k], scale));
            d->quant_tbl_ptrs[t]->sent_table = FALSE;
        }
    }
    /* Suppress orphaned old quantization tables. */
    for (int t = d->num_components == 1 ? 1 : 2; t < NUM_QUANT_TBLS; ++t)
        d->quant_tbl_ptrs[t] = NULL;
}

static void requantize(Work *w, jvirt_barray_ptr *arrays)
{
    j_decompress_ptr s = &w->src;
    uint64_t changed = 0, total = 0, zeros_before = 0, zeros_after = 0;
    for (int c = 0; c < s->num_components; ++c)
    {
        jpeg_component_info *p = &s->comp_info[c];
        const UINT16 *oldq = s->quant_tbl_ptrs[p->quant_tbl_no]->quantval;
        const UINT16 *newq = w->dst.quant_tbl_ptrs[w->dst.comp_info[c].quant_tbl_no]->quantval;
        for (JDIMENSION y = 0; y < p->height_in_blocks; ++y)
        {
            // jvirt_barray_ptr is an array of handles to libjpeg-managed coefficient storage.
            JBLOCKARRAY row = (*s->mem->access_virt_barray)((j_common_ptr)s, arrays[c], y, 1, TRUE);
            for (JDIMENSION x = 0; x < p->width_in_blocks; ++x) // x selects a blokc in that row.
                for (int k = 0; k < 64; ++k)                    // k selects a coefficient within that block.
                {
                    int old = row[0][x][k];
                    int64_t v = (int64_t)old * oldq[k];
                    int64_t magnitude = v < 0 ? -v : v;
                    /* Nearest integer, with exact half ties rounded away from zero.
                       All arithmetic is integral, including negative coefficients. */
                    int64_t q = (magnitude + newq[k] / 2) / newq[k];
                    if (v < 0)
                        q = -q;

                    if (q < (k ? -1023 : -1024) || q > 1023)
                        fail(w, "requantization would exceed conservative 8-bit DCT coefficient limits; choose coarser steps");

                    changed += q != old;
                    ++total;
                    zeros_before += old == 0;
                    zeros_after += q == 0;
                    row[0][x][k] = (JCOEF)q;
                }
        }
    }
    printf("coefficients=%llu changed=%llu zero_before=%.4f zero_after=%.4f\n",
           (unsigned long long)total, (unsigned long long)changed,
           (double)zeros_before / (double)total, (double)zeros_after / (double)total);
}

static void open_output(Work *w, const char *input, const char *output)
{
    struct stat a, b;
    if (stat(input, &a) || (stat(output, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino))
        fail(w, "input and output must be different files");

    size_t len = strlen(output);
    if (len > SIZE_MAX - 16)
        fail(w, "output path is too long");

    w->temporary = malloc(len + 16);
    if (!w->temporary)
        fail(w, "out of memory");

    snprintf(w->temporary, len + 16, "%s.tmp.XXXXXX", output);
    int fd = mkstemp(w->temporary);
    if (fd < 0)
        fail(w, "cannot create temporary output beside destination");

    w->output = fdopen(fd, "wb");
    if (!w->output)
    {
        close(fd);
        fail(w, "cannot open output stream");
    }
}

static void transcode(Work *w, const char *input, const char *output,
                      const char *kind, const char *argument, int optimize)
{
    double started = seconds();
    open_source(w, input);
    if (!optimize && w->src.progressive_mode)
        fail(w, "standard-table control output requires sequential input");

    jvirt_barray_ptr *arrays = jpeg_read_coefficients(&w->src);
    init_error(&w->de, &w->trap);
    w->dst.err = &w->de.base;
    w->dst_created = 1;
    jpeg_create_compress(&w->dst);
    jpeg_copy_critical_parameters(&w->src, &w->dst);
    w->dst.restart_interval = w->src.restart_interval;

    /* Preserve baseline table grouping; let libjpeg choose progressive grouping. */
    // OPTIMIZE is for task1, Huffman optimization.
    if (optimize && !w->src.progressive_mode)
        for (int c = 0; c < w->src.num_components; ++c)
        {
            // Select which Huffman table a component uses.
            w->dst.comp_info[c].dc_tbl_no = w->src.comp_info[c].dc_tbl_no;
            w->dst.comp_info[c].ac_tbl_no = w->src.comp_info[c].ac_tbl_no;
        }

    // KIND is for task2, Requantization.
    if (kind)
    {
        select_quantizer(w, kind, argument);
        requantize(w, arrays);
    }

    if (w->src.progressive_mode)
    {
        jpeg_simple_progression(&w->dst);
        fprintf(stderr, "note: progressive output uses libjpeg's scan script; size changes can include scan-layout changes\n");
    }

    /* Two passes: count actual output DC categories and AC run/size symbols,
     * then build JPEG-valid Huffman tables and entropy-encode the coefficients.
     * libjpeg handles restart predictor resets and edge MCU dummy blocks. */
    w->dst.optimize_coding = optimize ? TRUE : FALSE;
    w->dst.arith_code = FALSE;
    w->dst.write_JFIF_header = FALSE;
    w->dst.write_Adobe_marker = FALSE;

    open_output(w, input, output);
    // Connects the compressor to the temporary file stream.
    jpeg_stdio_dest(&w->dst, w->output);
    // Initializes coefficient output and writes the initial file header.
    jpeg_write_coefficients(&w->dst, arrays);
    for (jpeg_saved_marker_ptr m = w->src.marker_list; m; m = m->next)
        jpeg_write_marker(&w->dst, m->marker, m->data, m->data_length);

    // Encode coefficient data.
    jpeg_finish_compress(&w->dst);
    jpeg_finish_decompress(&w->src);
    int close_status = fclose(w->output);
    w->output = NULL;
    if (close_status)
        fail(w, "cannot finish writing output");
    if (rename(w->temporary, output))
        fail(w, "cannot commit output file");

    int64_t before = filesize(input), after = filesize(output);
    printf("input_bytes=%lld output_bytes=%lld reduction_percent=%.4f bpp=%.6f elapsed_ms=%.3f\n",
           (long long)before, (long long)after, 100.0 * (1.0 - (double)after / (double)before),
           8.0 * (double)after / ((double)w->src.image_width * (double)w->src.image_height),
           (seconds() - started) * 1000.0);
}

static void verify(Work *w, const char *first, const char *second)
{
    open_source(w, first);
    jvirt_barray_ptr *a = jpeg_read_coefficients(&w->src);
    w->other = fopen(second, "rb");
    if (!w->other)
        fail(w, "cannot open second JPEG");

    init_error(&w->ve, &w->trap);
    w->second.err = &w->ve.base;
    w->second_created = 1;
    jpeg_create_decompress(&w->second);
    jpeg_stdio_src(&w->second, w->other);
    jpeg_read_header(&w->second, TRUE);
    check_header(w, &w->second);

    jvirt_barray_ptr *b = jpeg_read_coefficients(&w->second);
    j_decompress_ptr s = &w->src, d = &w->second;
    if (s->image_width != d->image_width || s->image_height != d->image_height ||
        s->num_components != d->num_components || s->jpeg_color_space != d->jpeg_color_space)
        fail(w, "image geometry or colorspace differs");

    uint64_t checked = 0;
    for (int c = 0; c < s->num_components; ++c)
    {
        jpeg_component_info *p = &s->comp_info[c], *q = &d->comp_info[c];
        if (p->component_id != q->component_id || p->h_samp_factor != q->h_samp_factor ||
            p->v_samp_factor != q->v_samp_factor || p->width_in_blocks != q->width_in_blocks ||
            p->height_in_blocks != q->height_in_blocks)
            fail(w, "component geometry differs");

        if (memcmp(s->quant_tbl_ptrs[p->quant_tbl_no]->quantval,
                   d->quant_tbl_ptrs[q->quant_tbl_no]->quantval, 64 * sizeof(UINT16)))
            fail(w, "quantization tables differ");

        for (JDIMENSION y = 0; y < p->height_in_blocks; ++y)
        {
            JBLOCKARRAY ar = (*s->mem->access_virt_barray)((j_common_ptr)s, a[c], y, 1, FALSE);
            JBLOCKARRAY br = (*d->mem->access_virt_barray)((j_common_ptr)d, b[c], y, 1, FALSE);
            if (memcmp(ar[0], br[0], p->width_in_blocks * sizeof(JBLOCK)))
                fail(w, "quantized DCT coefficients differ");
            checked += (uint64_t)p->width_in_blocks * 64;
        }
    }
    jpeg_finish_decompress(s);
    jpeg_finish_decompress(d);
    printf("PASS: sampling, quantization and all %llu DCT coefficients are identical\n", (unsigned long long)checked);
}

static void usage(void)
{
    fputs("Usage:\n"
          "  jpegtool inspect input.jpg\n"
          "  jpegtool optimize input.jpg output.jpg\n"
          "  jpegtool standard input.jpg output.jpg  (control: default Huffman tables)\n"
          "  jpegtool requant input.jpg output.jpg scale FACTOR\n"
          "  jpegtool requant input.jpg output.jpg quality INTEGER_1_TO_100\n"
          "  jpegtool requant input.jpg output.jpg mpeg4 FACTOR\n"
          "  jpegtool requant input.jpg output.jpg table TABLE.txt\n"
          "  jpegtool verify input.jpg optimized.jpg\n"
          "Table file: 64 or 128 integers (1..255), natural 8x8 row-major order.\n"
          "scale: scales input DQT; mpeg4: adapts the MPEG-4 intra matrix shape.\n"
          "optimize/requant compute adaptive Huffman tables; all retain APP/COM payloads.\n",
          stderr);
}

int main(int argc, char **argv)
{
    if (argc < 3 || (!strcmp(argv[1], "inspect") ? argc != 3 : !strcmp(argv[1], "requant")                                                              ? argc != 6
                                                           : !strcmp(argv[1], "optimize") || !strcmp(argv[1], "standard") || !strcmp(argv[1], "verify") ? argc != 4
                                                                                                                                                        : 1))
    {
        usage();
        return 2;
    }

    Work *w = calloc(1, sizeof(*w)); // allocates the operation state and initializes it to zero.
    if (!w)
    {
        fputs("out of memory\n", stderr);
        return 1;
    }
    if (setjmp(w->trap))
    {
        cleanup(w);
        return 1;
    }
    if (!strcmp(argv[1], "inspect"))
    {
        open_source(w, argv[2]);
        jpeg_read_coefficients(&w->src); /* consume scans to validate the file */
        inspect(&w->src);
        jpeg_finish_decompress(&w->src);
    }
    else if (!strcmp(argv[1], "verify"))
        verify(w, argv[2], argv[3]);
    else
        transcode(w, argv[2], argv[3], argc == 6 ? argv[4] : NULL,
                  argc == 6 ? argv[5] : NULL, strcmp(argv[1], "standard") != 0);
    cleanup(w);
    return 0;
}

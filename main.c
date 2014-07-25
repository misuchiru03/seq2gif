/* See LICENSE for licence details. */
#include "yaft.h"
#include "conf.h"
#include "util.h"
#include "pseudo.h"
#include "terminal.h"
#include "function.h"
#include "osc.h"
#include "dcs.h"
#include "parse.h"
#include "gifsave89.h"

/* ttyrec header */
#pragma pack(push,1)
typedef struct header {
    int32_t tv_sec;
    int32_t tv_usec;
    int32_t len;
} Header;
#pragma pack(pop)

enum {
    TERM_WIDTH      = 640,
    TERM_HEIGHT     = 384,
    INPUT_WAIT      = 0,
    INPUT_BUF       = 1,
    GIF_DELAY       = 10,
    OUTPUT_BUF      = 1024,
    NO_OUTPUT_LIMIT = 16,
};

enum cmap_bitfield {
    RED_SHIFT   = 5,
    GREEN_SHIFT = 2,
    BLUE_SHIFT  = 0,
    RED_MASK    = 3,
    GREEN_MASK  = 3,
    BLUE_MASK   = 2
};

struct tty_state tty = {
    .visible        = true,
    .redraw_flag    = false,
    .loop_flag      = true,
    .window_resized = false,
};

void pb_init(struct pseudobuffer *pb)
{
    pb->width  = TERM_WIDTH;
    pb->height = TERM_HEIGHT;
    pb->bytes_per_pixel = BYTES_PER_PIXEL;
    pb->line_length = pb->width * pb->bytes_per_pixel;
    pb->buf = ecalloc(pb->width * pb->height, pb->bytes_per_pixel);
}

void pb_die(struct pseudobuffer *pb)
{
    free(pb->buf);
}

void set_colormap(int colormap[COLORS * BYTES_PER_PIXEL + 1])
{
    int i, ci, r, g, b;
    uint8_t index;

    /* colormap: terminal 256color
    for (i = 0; i < COLORS; i++) {
        ci = i * BYTES_PER_PIXEL;

        r = (color_list[i] >> 16) & bit_mask[8];
        g = (color_list[i] >> 8)  & bit_mask[8];
        b = (color_list[i] >> 0)  & bit_mask[8];

        colormap[ci + 0] = r;
        colormap[ci + 1] = g;
        colormap[ci + 2] = b;
    }
    */

    /* colormap: red/green: 3bit blue: 2bit
    */
    for (i = 0; i < COLORS; i++) {
        index = (uint8_t) i;
        ci = i * BYTES_PER_PIXEL;

        r = (index >> RED_SHIFT)   & bit_mask[RED_MASK];
        g = (index >> GREEN_SHIFT) & bit_mask[GREEN_MASK];
        b = (index >> BLUE_SHIFT)  & bit_mask[BLUE_MASK];

        colormap[ci + 0] = r * bit_mask[BITS_PER_BYTE] / bit_mask[RED_MASK];
        colormap[ci + 1] = g * bit_mask[BITS_PER_BYTE] / bit_mask[GREEN_MASK];
        colormap[ci + 2] = b * bit_mask[BITS_PER_BYTE] / bit_mask[BLUE_MASK];
    }
    colormap[COLORS * BYTES_PER_PIXEL] = -1;
}

uint32_t pixel2index(uint32_t pixel)
{
    /* pixel is always 24bpp */
    uint32_t r, g, b;

    /* split r, g, b bits */
    r = (pixel >> 16) & bit_mask[8];
    g = (pixel >> 8)  & bit_mask[8];
    b = (pixel >> 0)  & bit_mask[8];

    /* colormap: terminal 256color
    if (r == g && r == b) { // 24 gray scale
        r = 24 * r / COLORS;
        return 232 + r;
    }                       // 6x6x6 color cube

    r = 6 * r / COLORS;
    g = 6 * g / COLORS;
    b = 6 * b / COLORS;

    return 16 + (r * 36) + (g * 6) + b;
    */

    /* colormap: red/green: 3bit blue: 2bit
    */
    // get MSB ..._MASK bits
    r = (r >> (8 - RED_MASK))   & bit_mask[RED_MASK];
    g = (g >> (8 - GREEN_MASK)) & bit_mask[GREEN_MASK];
    b = (b >> (8 - BLUE_MASK))  & bit_mask[BLUE_MASK];

    return (r << RED_SHIFT) | (g << GREEN_SHIFT) | (b << BLUE_SHIFT);
}

void apply_colormap(struct pseudobuffer *pb, unsigned char *img)
{
    int w, h;
    uint32_t pixel = 0;

    for (h = 0; h < pb->height; h++) {
        for (w = 0; w < pb->width; w++) {
            memcpy(&pixel, pb->buf + h * pb->line_length
                + w * pb->bytes_per_pixel, pb->bytes_per_pixel);
            *(img + h * pb->width + w) = pixel2index(pixel) & bit_mask[BITS_PER_BYTE];
        }
    }
}

size_t write_gif(unsigned char *gifimage, int size)
{
    size_t wsize = 0;

    wsize = fwrite(gifimage, sizeof(unsigned char), size, stdout);
    return wsize;
}

void sig_handler(int signo)
{
    if (signo == SIGINT) {
    }
}

void sig_set()
{
    struct sigaction sigact;

    memset(&sigact, 0, sizeof(struct sigaction));
    sigact.sa_handler = sig_handler;
    sigact.sa_flags   = SA_RESTART;
    sigaction(SIGINT, &sigact, NULL);
}

void sig_reset()
{
    struct sigaction sigact;

    memset(&sigact, 0, sizeof(struct sigaction));
    sigact.sa_handler = SIG_DFL;
    sigaction(SIGINT, &sigact, NULL);
}

int main(int argc, char *argv[])
{
    uint8_t obuf[OUTPUT_BUF];
    ssize_t nread;
    struct terminal term;
    struct pseudobuffer pb;
    Header rec;

    void *gsdata;
    unsigned char *gifimage = NULL;
    int gifsize, colormap[COLORS * BYTES_PER_PIXEL + 1];
    unsigned char *img;

    (void) argc;
    (void) argv;

    /* init */
    pb_init(&pb);
    term_init(&term, pb.width, pb.height);
    sig_set();

    /* init gif */
    img = (unsigned char *) ecalloc(pb.width * pb.height, 1);
    set_colormap(colormap);
    if (!(gsdata = newgif((void **) &gifimage, pb.width, pb.height, colormap, 0)))
        return EXIT_FAILURE;

    animategif(gsdata, /* repetitions */ 0, GIF_DELAY,
        /* transparent background */  -1, /* disposal */ 2);

    /* main loop */
    for(;;) {
        nread = read(STDIN_FILENO, &rec, sizeof(Header));
        if (nread != sizeof(Header) || rec.len <= 0) {
            break;
        }
        nread = read(STDIN_FILENO, obuf, rec.len);
        if (nread != rec.len) {
            break;
        }
        parse(&term, obuf, nread);
        refresh(&pb, &term);

        /* take screenshot */
        apply_colormap(&pb, img);
        controlgif(gsdata, -1,
                   (rec.tv_sec * 1000000 + rec.tv_usec) / 10000,
                   0, 0);
        putgif(gsdata, img);
    }

    /* output gif */
    gifsize = endgif(gsdata);
    if (gifsize > 0) {
        write_gif(gifimage, gifsize);
        free(gifimage);
    }
    free(img);

    /* normal exit */
    sig_reset();
    term_die(&term);
    pb_die(&pb);

    return EXIT_SUCCESS;
}


/*
    DeaDBeeF - The Ultimate Music Player
    Copyright (C) 2009-2013 Oleksiy Yakovenko <waker@users.sourceforge.net>
    based on apedec from FFMpeg Copyright (c) 2007 Benjamin Zores <ben@geexbox.org>
    based upon libdemac from Dave Chapman.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
   main changes compared to ffmpeg:
     demuxer and decoder joined into 1 module
     no mallocs/reallocs during decoding
     streaming through fixed ringbuffer (small mem footprint)
     24bit support merged from rockbox
*/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
//#include <alloca.h>
#include <assert.h>
#include <math.h>
#include <deadbeef/deadbeef.h>
#include <deadbeef/strdupa.h>

#ifdef TARGET_ANDROID
int posix_memalign (void **memptr, size_t alignment, size_t size) {
    *memptr = memalign (alignment, size);
    return *memptr ? 0 : -1;
}
#endif

#define ENABLE_DEBUG 0

//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

static DB_decoder_t plugin;
static DB_functions_t *deadbeef;

#define PACKET_BUFFER_SIZE 100000

#define min(x,y) ((x)<(y)?(x):(y))
#define max(x,y) ((x)>(y)?(x):(y))

static inline uint8_t bytestream_get_byte (const uint8_t **ptr) {
    uint8_t v = *(*ptr);
    (*ptr)++;
    return v;
}

static inline uint32_t bytestream_get_be32 (const uint8_t **ptr) {
    const uint8_t *tmp = *ptr;
    uint32_t x = tmp[3] | (tmp[2] << 8) | (tmp[1] << 16) | (tmp[0] << 24);
    (*ptr) += 4;
    return x;
}


#define BLOCKS_PER_LOOP     4608
#define MAX_CHANNELS        2
#define MAX_BYTESPERSAMPLE  3

#define APE_FRAMECODE_MONO_SILENCE    1
#define APE_FRAMECODE_LEFT_SILENCE    1 /* same as mono */
#define APE_FRAMECODE_RIGHT_SILENCE   2
#define APE_FRAMECODE_STEREO_SILENCE  3 /* combined */
#define APE_FRAMECODE_PSEUDO_STEREO   4

#define HISTORY_SIZE 512
#define PREDICTOR_ORDER 8
/** Total size of all predictor histories */
#define PREDICTOR_SIZE 50

#define YDELAYA (18 + PREDICTOR_ORDER*4)
#define YDELAYB (18 + PREDICTOR_ORDER*3)
#define XDELAYA (18 + PREDICTOR_ORDER*2)
#define XDELAYB (18 + PREDICTOR_ORDER)

#define YADAPTCOEFFSA 18
#define XADAPTCOEFFSA 14
#define YADAPTCOEFFSB 10
#define XADAPTCOEFFSB 5

/**
 * Possible compression levels
 * @{
 */
enum APECompressionLevel {
    COMPRESSION_LEVEL_FAST       = 1000,
    COMPRESSION_LEVEL_NORMAL     = 2000,
    COMPRESSION_LEVEL_HIGH       = 3000,
    COMPRESSION_LEVEL_EXTRA_HIGH = 4000,
    COMPRESSION_LEVEL_INSANE     = 5000
};
/** @} */

#define APE_FILTER_LEVELS 3

/** Filter orders depending on compression level */
static const uint16_t ape_filter_orders[5][APE_FILTER_LEVELS] = {
    {  0,   0,    0 },
    { 16,   0,    0 },
    { 64,   0,    0 },
    { 32, 256,    0 },
    { 16, 256, 1280 }
};

/** Filter fraction bits depending on compression level */
static const uint8_t ape_filter_fracbits[5][APE_FILTER_LEVELS] = {
    {  0,  0,  0 },
    { 11,  0,  0 },
    { 11,  0,  0 },
    { 10, 13,  0 },
    { 11, 13, 15 }
};


/** Filters applied to the decoded data */
typedef struct APEFilter {
    int16_t *coeffs;        ///< actual coefficients used in filtering
    int16_t *adaptcoeffs;   ///< adaptive filter coefficients used for correcting of actual filter coefficients
    int16_t *historybuffer; ///< filter memory
    int16_t *delay;         ///< filtered values

    int avg;
} APEFilter;

typedef struct APERice {
    uint32_t k;
    uint32_t ksum;
} APERice;

typedef struct APERangecoder {
    uint32_t low;           ///< low end of interval
    uint32_t range;         ///< length of interval
    uint32_t help;          ///< bytes_to_follow resp. intermediate value
    unsigned int buffer;    ///< buffer for input/output
} APERangecoder;

/** Filter histories */
typedef struct APEPredictor {
    int32_t *buf;

    int32_t lastA[2];

    int32_t filterA[2];
    int32_t filterB[2];

    int32_t coeffsA[2][4];  ///< adaption coefficients
    int32_t coeffsB[2][5];  ///< adaption coefficients
    int32_t historybuffer[HISTORY_SIZE + PREDICTOR_SIZE];
} APEPredictor;

/* The earliest and latest file formats supported by this library */
#define APE_MIN_VERSION 3950
#define APE_MAX_VERSION 3990

#define MAC_FORMAT_FLAG_8_BIT                 1 // is 8-bit [OBSOLETE]
#define MAC_FORMAT_FLAG_CRC                   2 // uses the new CRC32 error detection [OBSOLETE]
#define MAC_FORMAT_FLAG_HAS_PEAK_LEVEL        4 // uint32 nPeakLevel after the header [OBSOLETE]
#define MAC_FORMAT_FLAG_24_BIT                8 // is 24-bit [OBSOLETE]
#define MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS    16 // has the number of seek elements after the peak level
#define MAC_FORMAT_FLAG_CREATE_WAV_HEADER    32 // create the wave header on decompression (not stored)

#define MAC_SUBFRAME_SIZE 4608

#define APE_EXTRADATA_SIZE 6

typedef struct {
    int64_t pos;
    int nblocks;
    int size;
    int skip;
} APEFrame;

/** Decoder context */
typedef struct APEContext {
    /* Derived fields */
    uint32_t junklength;
    uint32_t firstframe;
    uint32_t totalsamples;
    int currentframe;
    APEFrame *frames;

    /* Info from Descriptor Block */
    char magic[4];
    int16_t fileversion;
    int16_t padding1;
    uint32_t descriptorlength;
    uint32_t headerlength;
    uint32_t seektablelength;
    uint32_t wavheaderlength;
    uint32_t audiodatalength;
    uint32_t audiodatalength_high;
    uint32_t wavtaillength;
    uint8_t md5[16];

    /* Info from Header Block */
    uint16_t compressiontype;
    uint16_t formatflags;
    uint32_t blocksperframe;
    uint32_t finalframeblocks;
    uint32_t totalframes;
    uint16_t bps;
    uint16_t channels;
    uint32_t samplerate;
    int samples;                             ///< samples left to decode in current frame

    /* Seektable */
    uint32_t *seektable;

    int fset;                                ///< which filter set to use (calculated from compression level)
    int flags;                               ///< global decoder flags

    uint32_t CRC;                            ///< frame CRC
    int frameflags;                          ///< frame flags
    int currentframeblocks;                  ///< samples (per channel) in current frame
    int blocksdecoded;                       ///< count of decoded samples in current frame
    APEPredictor predictor;                  ///< predictor used for final reconstruction

    int32_t decoded0[BLOCKS_PER_LOOP];       ///< decoded data for the first channel
    int32_t decoded1[BLOCKS_PER_LOOP];       ///< decoded data for the second channel

    int16_t* filterbuf[APE_FILTER_LEVELS];   ///< filter memory

    APERangecoder rc;                        ///< rangecoder used to decode actual values
    APERice riceX;                           ///< rice code parameters for the second channel
    APERice riceY;                           ///< rice code parameters for the first channel
    APEFilter filters[APE_FILTER_LEVELS][2]; ///< filters used for reconstruction

    uint8_t *data_end;                       ///< frame data end
    const uint8_t *ptr;                      ///< current position in frame data
    const uint8_t *last_ptr;

    uint8_t *packet_data; // must be PACKET_BUFFER_SIZE
    int packet_remaining; // number of bytes in packet_data
    int packet_sizeleft; // number of bytes left unread for current ape frame
    int samplestoskip;
    int64_t currentsample; // current sample from beginning of file

    uint8_t buffer[BLOCKS_PER_LOOP * 2 * 2 * 2];
    int remaining;

    int error;
    int skip_header;
    int filterbuf_size[APE_FILTER_LEVELS];
} APEContext;

typedef struct {
    DB_fileinfo_t info;
    int64_t startsample;
    int64_t endsample;
    APEContext ape_ctx;
    DB_FILE *fp;
} ape_info_t;


inline static int
read_uint16(DB_FILE *fp, uint16_t* x)
{
    unsigned char tmp[2];
    size_t n;

    n = deadbeef->fread(tmp, 1, 2, fp);

    if (n != 2)
        return -1;

    *x = tmp[0] | (tmp[1] << 8);

    return 0;
}


inline static int
read_uint32(DB_FILE *fp, uint32_t* x)
{
    unsigned char tmp[4];
    size_t n;

    n = deadbeef->fread(tmp, 1, 4, fp);

    if (n != 4)
        return -1;

    *x = tmp[0] | (tmp[1] << 8) | (tmp[2] << 16) | (tmp[3] << 24);

    return 0;
}

static void ape_dumpinfo(APEContext * ape_ctx)
{
#if ENABLE_DEBUG
    int i;

    fprintf (stderr, "Descriptor Block:\n\n");
    fprintf (stderr, "magic                = \"%c%c%c%c\"\n", ape_ctx->magic[0], ape_ctx->magic[1], ape_ctx->magic[2], ape_ctx->magic[3]);
    fprintf (stderr, "fileversion          = %d\n", ape_ctx->fileversion);
    fprintf (stderr, "descriptorlength     = %d\n", ape_ctx->descriptorlength);
    fprintf (stderr, "headerlength         = %d\n", ape_ctx->headerlength);
    fprintf (stderr, "seektablelength      = %d\n", ape_ctx->seektablelength);
    fprintf (stderr, "wavheaderlength      = %d\n", ape_ctx->wavheaderlength);
    fprintf (stderr, "audiodatalength      = %d\n", ape_ctx->audiodatalength);
    fprintf (stderr, "audiodatalength_high = %d\n", ape_ctx->audiodatalength_high);
    fprintf (stderr, "wavtaillength        = %d\n", ape_ctx->wavtaillength);
    fprintf (stderr, "md5                  = ");
    for (i = 0; i < 16; i++)
         fprintf (stderr, "%02x", ape_ctx->md5[i]);
    fprintf (stderr, "\n");

    fprintf (stderr, "\nHeader Block:\n\n");

    fprintf (stderr, "compressiontype      = %d\n", ape_ctx->compressiontype);
    fprintf (stderr, "formatflags          = %d\n", ape_ctx->formatflags);
    fprintf (stderr, "blocksperframe       = %d\n", ape_ctx->blocksperframe);
    fprintf (stderr, "finalframeblocks     = %d\n", ape_ctx->finalframeblocks);
    fprintf (stderr, "totalframes          = %d\n", ape_ctx->totalframes);
    fprintf (stderr, "bps                  = %d\n", ape_ctx->bps);
    fprintf (stderr, "channels             = %d\n", ape_ctx->channels);
    fprintf (stderr, "samplerate           = %d\n", ape_ctx->samplerate);

    fprintf (stderr, "\nSeektable\n\n");
    if ((ape_ctx->seektablelength / sizeof(uint32_t)) != ape_ctx->totalframes) {
        fprintf (stderr, "No seektable\n");
    } else {
        for (i = 0; i < ape_ctx->seektablelength / sizeof(uint32_t); i++) {
            if (i < ape_ctx->totalframes - 1) {
                fprintf (stderr, "%8d   %d (%d bytes)\n", i, ape_ctx->seektable[i], ape_ctx->seektable[i + 1] - ape_ctx->seektable[i]);
            } else {
                fprintf (stderr, "%8d   %d\n", i, ape_ctx->seektable[i]);
            }
        }
    }

    fprintf (stderr, "\nFrames\n\n");
    for (i = 0; i < ape_ctx->totalframes; i++)
        fprintf (stderr, "%8d   %8lld %8d (%d samples)\n", i, ape_ctx->frames[i].pos, ape_ctx->frames[i].size, ape_ctx->frames[i].nblocks);

    fprintf (stderr, "\nCalculated information:\n\n");
    fprintf (stderr, "junklength           = %d\n", ape_ctx->junklength);
    fprintf (stderr, "firstframe           = %d\n", ape_ctx->firstframe);
    fprintf (stderr, "totalsamples         = %d\n", ape_ctx->totalsamples);
#endif
}

static int
ape_read_header(DB_FILE *fp, APEContext *ape)
{
    int i;

    /* TODO: Skip any leading junk such as id3v2 tags */
    ape->junklength = 0;

    if (deadbeef->fread (ape->magic, 1, 4, fp) != 4) {
        return -1;
    }
    if (memcmp (ape->magic, "MAC ", 4))
        return -1;

    if (read_uint16 (fp, (uint16_t *)&ape->fileversion) < 0) {
        return -1;
    }

    if (ape->fileversion < APE_MIN_VERSION) {
        fprintf (stderr, "ape: Unsupported file version - %d.%02d\n", ape->fileversion / 1000, (ape->fileversion % 1000) / 10);
        return -1;
    }

    if (ape->fileversion >= 3980) {
        if (read_uint16 (fp, (uint16_t *)&ape->padding1) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->descriptorlength) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->headerlength) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->seektablelength) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->wavheaderlength) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->audiodatalength) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->audiodatalength_high) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->wavtaillength) < 0) {
            return -1;
        }
        if (deadbeef->fread (ape->md5, 1, 16, fp) != 16) {
            return -1;
        }

        /* Skip any unknown bytes at the end of the descriptor.
           This is for future compatibility */
        if (ape->descriptorlength > 52) {
            if (deadbeef->fseek (fp, ape->descriptorlength - 52, SEEK_CUR)) {
                return -1;
            }
        }

        /* Read header data */
        if (read_uint16 (fp, &ape->compressiontype) < 0) {
            return -1;
        }
        if (read_uint16 (fp, &ape->formatflags) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->blocksperframe) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->finalframeblocks) < 0) {
            return -1;
        }
        if (read_uint32 (fp, & ape->totalframes) < 0) {
            return -1;
        }
        if (read_uint16 (fp, &ape->bps) < 0) {
            return -1;
        }
        if (read_uint16 (fp, &ape->channels) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->samplerate) < 0) {
            return -1;
        }
    } else {
        ape->descriptorlength = 0;
        ape->headerlength = 32;

        if (read_uint16 (fp, &ape->compressiontype) < 0) {
            return -1;
        }
        if (read_uint16 (fp, &ape->formatflags) < 0) {
            return -1;
        }
        if (read_uint16 (fp, &ape->channels) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->samplerate) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->wavheaderlength) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->wavtaillength) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->totalframes) < 0) {
            return -1;
        }
        if (read_uint32 (fp, &ape->finalframeblocks) < 0) {
            return -1;
        }

        if (ape->formatflags & MAC_FORMAT_FLAG_HAS_PEAK_LEVEL) {
            if (deadbeef->fseek(fp, 4, SEEK_CUR)) { /* Skip the peak level */
                return -1;
            }
            ape->headerlength += 4;
        }

        if (ape->formatflags & MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS) {
            if (read_uint32 (fp, &ape->seektablelength) < 0) {
                return -1;
            };
            ape->headerlength += 4;
            ape->seektablelength *= sizeof(int32_t);
        } else
            ape->seektablelength = ape->totalframes * sizeof(int32_t);

        if (ape->formatflags & MAC_FORMAT_FLAG_8_BIT)
            ape->bps = 8;
        else if (ape->formatflags & MAC_FORMAT_FLAG_24_BIT)
            ape->bps = 24;
        else
            ape->bps = 16;

        if (ape->fileversion >= 3950)
            ape->blocksperframe = 73728 * 4;
        else if (ape->fileversion >= 3900 || (ape->fileversion >= 3800  && ape->compressiontype >= 4000))
            ape->blocksperframe = 73728;
        else
            ape->blocksperframe = 9216;

        /* Skip any stored wav header */
        if (!(ape->formatflags & MAC_FORMAT_FLAG_CREATE_WAV_HEADER)) {
            if (deadbeef->fseek (fp, ape->wavheaderlength, SEEK_CUR)) {
                return -1;
            }
        }
    }

    if(ape->totalframes > UINT_MAX / sizeof(APEFrame)){
        fprintf (stderr, "ape: Too many frames: %d\n", ape->totalframes);
        return -1;
    }

    if(ape->totalframes == 0) {
        fprintf (stderr, "ape: The count of frames is zero\n");
        return -1;
    }

    ape->frames = calloc(ape->totalframes, sizeof(APEFrame));
    if(!ape->frames)
        return -1;
    ape->firstframe   = ape->junklength + ape->descriptorlength + ape->headerlength + ape->seektablelength + ape->wavheaderlength;
    ape->currentframe = 0;


    ape->totalsamples = ape->finalframeblocks;
    if (ape->totalframes > 1)
        ape->totalsamples += ape->blocksperframe * (ape->totalframes - 1);

    if (ape->seektablelength > 0) {
        ape->seektable = calloc (1, ape->seektablelength);
        for (i = 0; i < ape->seektablelength / sizeof(uint32_t); i++) {
            if (read_uint32 (fp, &ape->seektable[i]) < 0) {
                return -1;
            }
        }
    }

    ape->frames[0].pos     = ape->firstframe;
    ape->frames[0].nblocks = ape->blocksperframe;
    ape->frames[0].skip    = 0;
    for (i = 1; i < ape->totalframes; i++) {
        if (ape->seektablelength > 0) {
            ape->frames[i].pos = ape->seektable[i];
        }
        ape->frames[i].nblocks  = ape->blocksperframe;
        ape->frames[i - 1].size = (int)(ape->frames[i].pos - ape->frames[i - 1].pos);
        ape->frames[i].skip     = (ape->frames[i].pos - ape->frames[0].pos) & 3;
    }
    ape->frames[ape->totalframes - 1].size    = ape->finalframeblocks * 4;
    ape->frames[ape->totalframes - 1].nblocks = ape->finalframeblocks;

    for (i = 0; i < ape->totalframes; i++) {
        if(ape->frames[i].skip){
            ape->frames[i].pos  -= ape->frames[i].skip;
            ape->frames[i].size += ape->frames[i].skip;
        }
        ape->frames[i].size = (ape->frames[i].size + 3) & ~3;
    }


    ape_dumpinfo(ape);

#if ENABLE_DEBUG
    fprintf (stderr, "ape: Decoding file - v%d.%02d, compression level %d\n", ape->fileversion / 1000, (ape->fileversion % 1000) / 10, ape->compressiontype);
#endif

    return 0;
}

#   define AV_WB32(p, d) do {                   \
        ((uint8_t*)(p))[3] = (d);               \
        ((uint8_t*)(p))[2] = (d)>>8;            \
        ((uint8_t*)(p))[1] = (d)>>16;           \
        ((uint8_t*)(p))[0] = (d)>>24;           \
    } while(0)

#define AV_WL32(p, v) AV_WB32(p, bswap_32(v))

static inline const uint32_t bswap_32(uint32_t x)
{
    x= ((x<<8)&0xFF00FF00) | ((x>>8)&0x00FF00FF);
    x= (x>>16) | (x<<16);
    return x;
}

static int ape_read_packet(DB_FILE *fp, APEContext *ape_ctx)
{
    int nblocks;
    APEContext *ape = ape_ctx;
    uint32_t extra_size = 8;

    if (ape->currentframe > ape->totalframes)
        return -1;
    trace ("seeking to packet %d (%lld + %d)\n", ape->currentframe, ape->frames[ape->currentframe].pos, ape_ctx->skip_header);
    if (deadbeef->fseek (fp, ape->frames[ape->currentframe].pos + ape_ctx->skip_header, SEEK_SET) != 0) {
        return -1;
    }

    /* Calculate how many blocks there are in this frame */
    if (ape->currentframe == (ape->totalframes - 1))
        nblocks = ape->finalframeblocks;
    else
        nblocks = ape->blocksperframe;

    AV_WL32(ape->packet_data    , nblocks);
    AV_WL32(ape->packet_data + 4, ape->frames[ape->currentframe].skip);
//    packet_sizeleft -= 8;

// update bitrate
    int bitrate = -1;
    if (nblocks != 0 && ape->frames[ape->currentframe].size != 0) {
        float sec = (float)nblocks / ape->samplerate;
        bitrate = ape->frames[ape->currentframe].size / sec * 8;
    }
    if (bitrate > 0) {
        deadbeef->streamer_set_bitrate (bitrate/1000);
    }

    int sz = PACKET_BUFFER_SIZE-8;
    sz = min (sz, ape->frames[ape->currentframe].size);
//    fprintf (stderr, "readsize: %d, packetsize: %d\n", sz, ape->frames[ape->currentframe].size);
    deadbeef->fread (ape->packet_data + extra_size, 1, sz, fp);
    ape->packet_sizeleft = ape->frames[ape->currentframe].size - sz + 8;
    ape->packet_remaining = sz+8;

    ape->currentframe++;

    return 0;
}

static void
ape_free_ctx (APEContext *ape_ctx) {
    int i;
    if (ape_ctx->packet_data) {
        free (ape_ctx->packet_data);
        ape_ctx->packet_data = NULL;
    }
    if (ape_ctx->frames) {
        free (ape_ctx->frames);
        ape_ctx->frames = NULL;
    }
    if (ape_ctx->seektable) {
        free (ape_ctx->seektable);
        ape_ctx->seektable = NULL;
    }
    for (i = 0; i < APE_FILTER_LEVELS; i++) {
        if (ape_ctx->filterbuf[i]) {
            free (ape_ctx->filterbuf[i]);
            ape_ctx->filterbuf[i] = NULL;
        }
    }
    memset (ape_ctx, 0, sizeof (APEContext));
}

static void
ffap_free (DB_fileinfo_t *_info)
{
    ape_info_t *info = (ape_info_t *)_info;
    ape_free_ctx (&info->ape_ctx);
    if (info->fp) {
        deadbeef->fclose (info->fp);
    }
    free (info);
}

static DB_fileinfo_t *
ffap_open (uint32_t hints) {
    ape_info_t *info = calloc (1, sizeof (ape_info_t));
    return &info->info;
}

static int
ffap_init (DB_fileinfo_t *_info, DB_playItem_t *it)
{
    ape_info_t *info = (ape_info_t*)_info;

    deadbeef->pl_lock ();
    const char *uri = strdupa (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();
    info->fp = deadbeef->fopen (uri);
    if (!info->fp) {
        return -1;
    }
    memset (&info->ape_ctx, 0, sizeof (info->ape_ctx));
    int skip = deadbeef->junk_get_leading_size (info->fp);
    if (skip > 0) {
        if (deadbeef->fseek (info->fp, skip, SEEK_SET)) {
            return -1;
        }
        info->ape_ctx.skip_header = skip;
    }
    if (ape_read_header (info->fp, &info->ape_ctx)) {
        return -1;
    }
    int i;

    if (info->ape_ctx.channels > 2) {
        fprintf (stderr, "ape: Only mono and stereo is supported\n");
        return -1;
    }

#if ENABLE_DEBUG
    fprintf (stderr, "ape: Compression Level: %d - Flags: %d\n", info->ape_ctx.compressiontype, info->ape_ctx.formatflags);
#endif
    if (info->ape_ctx.compressiontype % 1000 || info->ape_ctx.compressiontype > COMPRESSION_LEVEL_INSANE) {
        fprintf (stderr, "ape: Incorrect compression level %d\n", info->ape_ctx.compressiontype);
        return -1;
    }
    info->ape_ctx.fset = info->ape_ctx.compressiontype / 1000 - 1;
    for (i = 0; i < APE_FILTER_LEVELS; i++) {
        if (!ape_filter_orders[info->ape_ctx.fset][i])
            break;
        info->ape_ctx.filterbuf_size[i] = (ape_filter_orders[info->ape_ctx.fset][i] * 3 + HISTORY_SIZE) * 4;
        int err = posix_memalign ((void **)&info->ape_ctx.filterbuf[i], 16, info->ape_ctx.filterbuf_size[i]);
        if (err) {
            trace ("ffap: out of memory (posix_memalign)\n");
            return -1;
        }
    }

    _info->plugin = &plugin;
    _info->fmt.bps = info->ape_ctx.bps;
    _info->fmt.samplerate = info->ape_ctx.samplerate;
    _info->fmt.channels = info->ape_ctx.channels;
    _info->fmt.channelmask = _info->fmt.channels == 1 ? DDB_SPEAKER_FRONT_LEFT : (DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT);
    _info->readpos = 0;

    info->ape_ctx.packet_data = malloc (PACKET_BUFFER_SIZE);
    if (!info->ape_ctx.packet_data) {
        fprintf (stderr, "ape: failed to allocate memory for packet data\n");
        return -1;
    }

    int64_t endsample = deadbeef->pl_item_get_endsample (it);
    if (endsample > 0) {
        info->startsample = deadbeef->pl_item_get_startsample (it);
        info->endsample = endsample;
        plugin.seek_sample (_info, 0);
        //trace ("start: %d/%f, end: %d/%f\n", startsample, timestart, endsample, timeend);
    }
    else {
        info->startsample = 0;
        info->endsample = info->ape_ctx.totalsamples-1;
    }

    return 0;
}

/**
 * @defgroup rangecoder APE range decoder
 * @{
 */

#define CODE_BITS    32
#define TOP_VALUE    ((unsigned int)1 << (CODE_BITS-1))
#define SHIFT_BITS   (CODE_BITS - 9)
#define EXTRA_BITS   ((CODE_BITS-2) % 8 + 1)
#define BOTTOM_VALUE (TOP_VALUE >> 8)

/** Start the decoder */
static inline void range_start_decoding(APEContext * ctx)
{
    ctx->rc.buffer = bytestream_get_byte(&ctx->ptr);
    ctx->rc.low    = ctx->rc.buffer >> (8 - EXTRA_BITS);
    ctx->rc.range  = (uint32_t) 1 << EXTRA_BITS;
}

/** Perform normalization */
static inline void range_dec_normalize(APEContext * ctx)
{
    while (ctx->rc.range <= BOTTOM_VALUE) {
        ctx->rc.buffer <<= 8;
        if(ctx->ptr < ctx->data_end)
            ctx->rc.buffer += *ctx->ptr;
        ctx->ptr++;
        ctx->rc.low    = (ctx->rc.low << 8)    | ((ctx->rc.buffer >> 1) & 0xFF);
        ctx->rc.range  <<= 8;
    }
}

/**
 * Calculate culmulative frequency for next symbol. Does NO update!
 * @param ctx decoder context
 * @param tot_f is the total frequency or (code_value)1<<shift
 * @return the culmulative frequency
 */
static inline int range_decode_culfreq(APEContext * ctx, int tot_f)
{
    range_dec_normalize(ctx);
    ctx->rc.help = ctx->rc.range / tot_f;
    return ctx->rc.low / ctx->rc.help;
}

/**
 * Decode value with given size in bits
 * @param ctx decoder context
 * @param shift number of bits to decode
 */
static inline int range_decode_culshift(APEContext * ctx, int shift)
{
    range_dec_normalize(ctx);
    ctx->rc.help = ctx->rc.range >> shift;
    return ctx->rc.low / ctx->rc.help;
}


/**
 * Update decoding state
 * @param ctx decoder context
 * @param sy_f the interval length (frequency of the symbol)
 * @param lt_f the lower end (frequency sum of < symbols)
 */
static inline void range_decode_update(APEContext * ctx, int sy_f, int lt_f)
{
    ctx->rc.low  -= ctx->rc.help * lt_f;
    ctx->rc.range = ctx->rc.help * sy_f;
}

/** Decode n bits (n <= 16) without modelling */
static inline int range_decode_bits(APEContext * ctx, int n)
{
    int sym = range_decode_culshift(ctx, n);
    range_decode_update(ctx, 1, sym);
    return sym;
}


#define MODEL_ELEMENTS 64

/**
 * Fixed probabilities for symbols in Monkey Audio version 3.97
 */
static const uint16_t counts_3970[22] = {
        0, 14824, 28224, 39348, 47855, 53994, 58171, 60926,
    62682, 63786, 64463, 64878, 65126, 65276, 65365, 65419,
    65450, 65469, 65480, 65487, 65491, 65493,
};

/**
 * Probability ranges for symbols in Monkey Audio version 3.97
 */
static const uint16_t counts_diff_3970[21] = {
    14824, 13400, 11124, 8507, 6139, 4177, 2755, 1756,
    1104, 677, 415, 248, 150, 89, 54, 31,
    19, 11, 7, 4, 2,
};

/**
 * Fixed probabilities for symbols in Monkey Audio version 3.98
 */
static const uint16_t counts_3980[22] = {
        0, 19578, 36160, 48417, 56323, 60899, 63265, 64435,
    64971, 65232, 65351, 65416, 65447, 65466, 65476, 65482,
    65485, 65488, 65490, 65491, 65492, 65493,
};

/**
 * Probability ranges for symbols in Monkey Audio version 3.98
 */
static const uint16_t counts_diff_3980[21] = {
    19578, 16582, 12257, 7906, 4576, 2366, 1170, 536,
    261, 119, 65, 31, 19, 10, 6, 3,
    3, 2, 1, 1, 1,
};

/**
 * Decode symbol
 * @param ctx decoder context
 * @param counts probability range start position
 * @param counts_diff probability range widths
 */
static inline int range_get_symbol(APEContext * ctx,
                                   const uint16_t counts[],
                                   const uint16_t counts_diff[])
{
    int symbol, cf;

    cf = range_decode_culshift(ctx, 16);

    if(cf > 65492){
        symbol= cf - 65535 + 63;
        range_decode_update(ctx, 1, cf);
        if(unlikely (cf > 65535)) {
            ctx->error=1;
        }
        return symbol;
    }
    /* figure out the symbol inefficiently; a binary search would be much better */
    for (symbol = 0; counts[symbol + 1] <= cf; symbol++);

    range_decode_update(ctx, counts_diff[symbol], counts[symbol]);

    return symbol;
}
/** @} */ // group rangecoder

static inline void update_rice(APERice *rice, int x)
{
    int lim = rice->k ? (1 << (rice->k + 4)) : 0;
    rice->ksum += ((x + 1) / 2) - ((rice->ksum + 16) >> 5);

    if (rice->ksum < lim)
        rice->k--;
    else if (rice->ksum >= (1 << (rice->k + 5)))
        rice->k++;
}

static inline int ape_decode_value(APEContext * ctx, APERice *rice)
{
    int x, overflow;

    if (ctx->fileversion < 3990) {
        int tmpk;

        overflow = range_get_symbol(ctx, counts_3970, counts_diff_3970);

        if (overflow == (MODEL_ELEMENTS - 1)) {
            tmpk = range_decode_bits(ctx, 5);
            overflow = 0;
        } else
            tmpk = (rice->k < 1) ? 0 : rice->k - 1;

        if (tmpk <= 16)
            x = range_decode_bits(ctx, tmpk);
        else {
            x = range_decode_bits(ctx, 16);
            x |= (range_decode_bits(ctx, tmpk - 16) << 16);
        }
        x += overflow << tmpk;
    } else {
        int base, pivot;

        pivot = rice->ksum >> 5;
        if (pivot == 0)
            pivot = 1;

        overflow = range_get_symbol(ctx, counts_3980, counts_diff_3980);

        if (overflow == (MODEL_ELEMENTS - 1)) {
            overflow  = range_decode_bits(ctx, 16) << 16;
            overflow |= range_decode_bits(ctx, 16);
        }

        if (pivot >= 0x10000) {
            /* Codepath for 24-bit streams */
            int nbits, lo_bits, base_hi, base_lo;

            /* Count the number of bits in pivot */
            nbits = 17; /* We know there must be at least 17 bits */
            while ((pivot >> nbits) > 0) { nbits++; }

            /* base_lo is the low (nbits-16) bits of base
               base_hi is the high 16 bits of base
               */
            lo_bits = (nbits - 16);

            base_hi = range_decode_culfreq(ctx, (pivot >> lo_bits) + 1);
            range_decode_update(ctx, 1, base_hi);

            base_lo = range_decode_culshift(ctx, lo_bits);
            range_decode_update(ctx, 1, base_lo);

            base = (base_hi << lo_bits) + base_lo;
        }
        else {
            base = range_decode_culfreq(ctx, pivot);
            range_decode_update(ctx, 1, base);
        }

        x = base + overflow * pivot;
    }

    update_rice(rice, x);

    /* Convert to signed */
    if (x & 1)
        return (x >> 1) + 1;
    else
        return -(x >> 1);
}

static void entropy_decode(APEContext * ctx, int blockstodecode, int stereo)
{
    int32_t *decoded0 = ctx->decoded0;
    int32_t *decoded1 = ctx->decoded1;

    ctx->blocksdecoded = blockstodecode;

    if ((ctx->frameflags & APE_FRAMECODE_STEREO_SILENCE) == APE_FRAMECODE_STEREO_SILENCE) {
        /* We are pure silence, just memset the output buffer. */
        memset(decoded0, 0, blockstodecode * sizeof(int32_t));
        if (stereo) {
            memset(decoded1, 0, blockstodecode * sizeof(int32_t));
        }
    } else {
        while (likely (blockstodecode--)) {
            *decoded0++ = ape_decode_value(ctx, &ctx->riceY);
            if (stereo)
                *decoded1++ = ape_decode_value(ctx, &ctx->riceX);
        }
    }

    if (ctx->blocksdecoded == ctx->currentframeblocks)
        range_dec_normalize(ctx);   /* normalize to use up all bytes */
}

static void init_entropy_decoder(APEContext * ctx)
{
    /* Read the CRC */
    ctx->CRC = bytestream_get_be32(&ctx->ptr);

    /* Read the frame flags if they exist */
    ctx->frameflags = 0;
    if ((ctx->fileversion > 3820) && (ctx->CRC & 0x80000000)) {
        ctx->CRC &= ~0x80000000;

        ctx->frameflags = bytestream_get_be32(&ctx->ptr);
    }

    /* Keep a count of the blocks decoded in this frame */
    ctx->blocksdecoded = 0;

    /* Initialize the rice structs */
    ctx->riceX.k = 10;
    ctx->riceX.ksum = (1 << ctx->riceX.k) * 16;
    ctx->riceY.k = 10;
    ctx->riceY.ksum = (1 << ctx->riceY.k) * 16;

    /* The first 8 bits of input are ignored. */
    ctx->ptr++;

    range_start_decoding(ctx);
}

static const int32_t initial_coeffs[4] = {
    360, 317, -109, 98
};

static void init_predictor_decoder(APEContext * ctx)
{
    APEPredictor *p = &ctx->predictor;

    /* Zero the history buffers */
    memset(p->historybuffer, 0, PREDICTOR_SIZE * sizeof(int32_t));
    p->buf = p->historybuffer;

    /* Initialize and zero the coefficients */
    memcpy(p->coeffsA[0], initial_coeffs, sizeof(initial_coeffs));
    memcpy(p->coeffsA[1], initial_coeffs, sizeof(initial_coeffs));
    memset(p->coeffsB, 0, sizeof(p->coeffsB));

    p->filterA[0] = p->filterA[1] = 0;
    p->filterB[0] = p->filterB[1] = 0;
    p->lastA[0]   = p->lastA[1]   = 0;
}

/** Get inverse sign of integer (-1 for positive, 1 for negative and 0 for zero) */
static inline int APESIGN(int32_t x) {
    return (x < 0) - (x > 0);
}

static int predictor_update_filter(APEPredictor *p, const int decoded, const int filter, const int delayA, const int delayB, const int adaptA, const int adaptB)
{
    int32_t predictionA, predictionB;

    p->buf[delayA]     = p->lastA[filter];
    p->buf[adaptA]     = APESIGN(p->buf[delayA]);
    p->buf[delayA - 1] = p->buf[delayA] - p->buf[delayA - 1];
    p->buf[adaptA - 1] = APESIGN(p->buf[delayA - 1]);

    predictionA = p->buf[delayA    ] * p->coeffsA[filter][0] +
                  p->buf[delayA - 1] * p->coeffsA[filter][1] +
                  p->buf[delayA - 2] * p->coeffsA[filter][2] +
                  p->buf[delayA - 3] * p->coeffsA[filter][3];

    /*  Apply a scaled first-order filter compression */
    p->buf[delayB]     = p->filterA[filter ^ 1] - ((p->filterB[filter] * 31) >> 5);
    p->buf[adaptB]     = APESIGN(p->buf[delayB]);
    p->buf[delayB - 1] = p->buf[delayB] - p->buf[delayB - 1];
    p->buf[adaptB - 1] = APESIGN(p->buf[delayB - 1]);
    p->filterB[filter] = p->filterA[filter ^ 1];

    predictionB = p->buf[delayB    ] * p->coeffsB[filter][0] +
                  p->buf[delayB - 1] * p->coeffsB[filter][1] +
                  p->buf[delayB - 2] * p->coeffsB[filter][2] +
                  p->buf[delayB - 3] * p->coeffsB[filter][3] +
                  p->buf[delayB - 4] * p->coeffsB[filter][4];

    p->lastA[filter] = decoded + ((predictionA + (predictionB >> 1)) >> 10);
    p->filterA[filter] = p->lastA[filter] + ((p->filterA[filter] * 31) >> 5);

    if (!decoded) // no need updating filter coefficients
        return p->filterA[filter];

    if (decoded > 0) {
        p->coeffsA[filter][0] -= p->buf[adaptA    ];
        p->coeffsA[filter][1] -= p->buf[adaptA - 1];
        p->coeffsA[filter][2] -= p->buf[adaptA - 2];
        p->coeffsA[filter][3] -= p->buf[adaptA - 3];

        p->coeffsB[filter][0] -= p->buf[adaptB    ];
        p->coeffsB[filter][1] -= p->buf[adaptB - 1];
        p->coeffsB[filter][2] -= p->buf[adaptB - 2];
        p->coeffsB[filter][3] -= p->buf[adaptB - 3];
        p->coeffsB[filter][4] -= p->buf[adaptB - 4];
    } else {
        p->coeffsA[filter][0] += p->buf[adaptA    ];
        p->coeffsA[filter][1] += p->buf[adaptA - 1];
        p->coeffsA[filter][2] += p->buf[adaptA - 2];
        p->coeffsA[filter][3] += p->buf[adaptA - 3];

        p->coeffsB[filter][0] += p->buf[adaptB    ];
        p->coeffsB[filter][1] += p->buf[adaptB - 1];
        p->coeffsB[filter][2] += p->buf[adaptB - 2];
        p->coeffsB[filter][3] += p->buf[adaptB - 3];
        p->coeffsB[filter][4] += p->buf[adaptB - 4];
    }
    return p->filterA[filter];
}

static void predictor_decode_stereo(APEContext * ctx, int count)
{
    int32_t predictionA, predictionB;
    APEPredictor *p = &ctx->predictor;
    int32_t *decoded0 = ctx->decoded0;
    int32_t *decoded1 = ctx->decoded1;

    while (count--) {
        /* Predictor Y */
        predictionA = predictor_update_filter(p, *decoded0, 0, YDELAYA, YDELAYB, YADAPTCOEFFSA, YADAPTCOEFFSB);
        predictionB = predictor_update_filter(p, *decoded1, 1, XDELAYA, XDELAYB, XADAPTCOEFFSA, XADAPTCOEFFSB);
        *(decoded0++) = predictionA;
        *(decoded1++) = predictionB;

        /* Combined */
        p->buf++;

        /* Have we filled the history buffer? */
        if (p->buf == p->historybuffer + HISTORY_SIZE) {
            memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(int32_t));
            p->buf = p->historybuffer;
        }
    }
}

static void predictor_decode_mono(APEContext * ctx, int count)
{
    APEPredictor *p = &ctx->predictor;
    int32_t *decoded0 = ctx->decoded0;
    int32_t predictionA, currentA, A;

    currentA = p->lastA[0];

    while (count--) {
        A = *decoded0;

        p->buf[YDELAYA] = currentA;
        p->buf[YDELAYA - 1] = p->buf[YDELAYA] - p->buf[YDELAYA - 1];

        predictionA = p->buf[YDELAYA    ] * p->coeffsA[0][0] +
                      p->buf[YDELAYA - 1] * p->coeffsA[0][1] +
                      p->buf[YDELAYA - 2] * p->coeffsA[0][2] +
                      p->buf[YDELAYA - 3] * p->coeffsA[0][3];

        currentA = A + (predictionA >> 10);

        p->buf[YADAPTCOEFFSA]     = APESIGN(p->buf[YDELAYA    ]);
        p->buf[YADAPTCOEFFSA - 1] = APESIGN(p->buf[YDELAYA - 1]);

        if (A > 0) {
            p->coeffsA[0][0] -= p->buf[YADAPTCOEFFSA    ];
            p->coeffsA[0][1] -= p->buf[YADAPTCOEFFSA - 1];
            p->coeffsA[0][2] -= p->buf[YADAPTCOEFFSA - 2];
            p->coeffsA[0][3] -= p->buf[YADAPTCOEFFSA - 3];
        } else if (A < 0) {
            p->coeffsA[0][0] += p->buf[YADAPTCOEFFSA    ];
            p->coeffsA[0][1] += p->buf[YADAPTCOEFFSA - 1];
            p->coeffsA[0][2] += p->buf[YADAPTCOEFFSA - 2];
            p->coeffsA[0][3] += p->buf[YADAPTCOEFFSA - 3];
        }

        p->buf++;

        /* Have we filled the history buffer? */
        if (p->buf == p->historybuffer + HISTORY_SIZE) {
            memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(int32_t));
            p->buf = p->historybuffer;
        }

        p->filterA[0] = currentA + ((p->filterA[0] * 31) >> 5);
        *(decoded0++) = p->filterA[0];
    }

    p->lastA[0] = currentA;
}

static void do_init_filter(APEFilter *f, int16_t * buf, int order)
{
    f->coeffs = buf;
    f->historybuffer = buf + order;
    f->delay       = f->historybuffer + order * 2;
    f->adaptcoeffs = f->historybuffer + order;

    memset(f->historybuffer, 0, (order * 2) * sizeof(int16_t));
    memset(f->coeffs, 0, order * sizeof(int16_t));
    f->avg = 0;
}

static void init_filter(APEContext * ctx, APEFilter *f, int16_t * buf, int order)
{
    do_init_filter(&f[0], buf, order);
    do_init_filter(&f[1], buf + order * 3 + HISTORY_SIZE, order);
}

#ifdef HAVE_SSE2

#if ARCH_X86_64
#    define REG_a "rax"
#    define REG_b "rbx"
#    define REG_c "rcx"
#    define REG_d "rdx"
#    define REG_D "rdi"
#    define REG_S "rsi"
#    define PTR_SIZE "8"
#    define REG_SP "rsp"
#    define REG_BP "rbp"
#    define REGBP   rbp
#    define REGa    rax
#    define REGb    rbx
#    define REGc    rcx
#    define REGd    rdx
#    define REGSP   rsp
typedef int64_t x86_reg;
#elif ARCH_X86_32
#    define REG_a "eax"
#    define REG_b "ebx"
#    define REG_c "ecx"
#    define REG_d "edx"
#    define REG_D "edi"
#    define REG_S "esi"
#    define PTR_SIZE "4"
#    define REG_SP "esp"
#    define REG_BP "ebp"
#    define REGBP   ebp
#    define REGa    eax
#    define REGb    ebx
#    define REGc    ecx
#    define REGd    edx
#    define REGSP   esp
typedef int32_t x86_reg;
#else
#warning unknown arch, SIMD optimizations will be disabled
typedef int x86_reg;
#endif

typedef struct { uint64_t a, b; } xmm_reg;
#define DECLARE_ALIGNED(n,t,v)      t v __attribute__ ((aligned (n)))
#define DECLARE_ALIGNED_16(t, v) DECLARE_ALIGNED(16, t, v)
#endif

static int32_t scalarproduct_and_madd_int16_c(int16_t *v1, const int16_t *v2, const int16_t *v3, int order, int mul)
{
    int res = 0;
    while (order--) {
        res   += *v1 * *v2++;
        *v1++ += mul * *v3++;
    }
    return res;
}

static int32_t
(*scalarproduct_and_madd_int16)(int16_t *v1, const int16_t *v2, const int16_t *v3, int order, int mul);

static inline int16_t clip_int16(int a)
{
    if ((a+32768) & ~65535) return (a>>31) ^ 32767;
        else                    return a;
}

static void bswap_buf(uint32_t *dst, const uint32_t *src, int w){
    int i;

    for(i=0; i+8<=w; i+=8){
        dst[i+0]= bswap_32(src[i+0]);
        dst[i+1]= bswap_32(src[i+1]);
        dst[i+2]= bswap_32(src[i+2]);
        dst[i+3]= bswap_32(src[i+3]);
        dst[i+4]= bswap_32(src[i+4]);
        dst[i+5]= bswap_32(src[i+5]);
        dst[i+6]= bswap_32(src[i+6]);
        dst[i+7]= bswap_32(src[i+7]);
    }
    for(;i<w; i++){
        dst[i+0]= bswap_32(src[i+0]);
    }
}


static inline void do_apply_filter(APEContext * ctx, int version, APEFilter *f, int32_t *data, int count, int order, int fracbits)
{
    int res;
    int absres;

    while (count--) {
        res = scalarproduct_and_madd_int16(f->coeffs, f->delay - order, f->adaptcoeffs - order, order, APESIGN(*data));
        res = (res + (1 << (fracbits - 1))) >> fracbits;
        res += *data;

        *data++ = res;

        /* Update the output history */
        *f->delay++ = clip_int16(res);

        if (version < 3980) {
            /* Version ??? to < 3.98 files (untested) */
            f->adaptcoeffs[0]  = (res == 0) ? 0 : ((res >> 28) & 8) - 4;
            f->adaptcoeffs[-4] >>= 1;
            f->adaptcoeffs[-8] >>= 1;
        } else {
            /* Version 3.98 and later files */

            /* Update the adaption coefficients */
            absres = (res < 0 ? -res : res);

            if (absres > (f->avg * 3))
                *f->adaptcoeffs = ((res >> 25) & 64) - 32;
            else if (absres > (f->avg * 4) / 3)
                *f->adaptcoeffs = ((res >> 26) & 32) - 16;
            else if (absres > 0)
                *f->adaptcoeffs = ((res >> 27) & 16) - 8;
            else
                *f->adaptcoeffs = 0;

            f->avg += (absres - f->avg) / 16;

            f->adaptcoeffs[-1] >>= 1;
            f->adaptcoeffs[-2] >>= 1;
            f->adaptcoeffs[-8] >>= 1;
        }

        f->adaptcoeffs++;

        /* Have we filled the history buffer? */
        if (f->delay == f->historybuffer + HISTORY_SIZE + (order * 2)) {
            memmove(f->historybuffer, f->delay - (order * 2),
                    (order * 2) * sizeof(int16_t));
            f->delay = f->historybuffer + order * 2;
            f->adaptcoeffs = f->historybuffer + order;
        }
    }
}

static void apply_filter(APEContext * ctx, APEFilter *f,
                         int32_t * data0, int32_t * data1,
                         int count, int order, int fracbits)
{
    do_apply_filter(ctx, ctx->fileversion, &f[0], data0, count, order, fracbits);
    if (data1)
        do_apply_filter(ctx, ctx->fileversion, &f[1], data1, count, order, fracbits);
}

static void ape_apply_filters(APEContext * ctx, int32_t * decoded0,
                              int32_t * decoded1, int count)
{
    int i;

    for (i = 0; i < APE_FILTER_LEVELS; i++) {
        if (!ape_filter_orders[ctx->fset][i])
            break;
        apply_filter(ctx, ctx->filters[i], decoded0, decoded1, count, ape_filter_orders[ctx->fset][i], ape_filter_fracbits[ctx->fset][i]);
    }
}

static void init_frame_decoder(APEContext * ctx)
{
    int i;
    init_entropy_decoder(ctx);
    init_predictor_decoder(ctx);

    for (i = 0; i < APE_FILTER_LEVELS; i++) {
        if (!ape_filter_orders[ctx->fset][i])
            break;
        init_filter(ctx, ctx->filters[i], ctx->filterbuf[i], ape_filter_orders[ctx->fset][i]);
    }
}

static void ape_unpack_mono(APEContext * ctx, int count)
{
    int32_t left;
    int32_t *decoded0 = ctx->decoded0;
    int32_t *decoded1 = ctx->decoded1;

    if (ctx->frameflags & APE_FRAMECODE_STEREO_SILENCE) {
        /* We are pure silence, so we're done. */
        //fprintf (stderr, "pure silence mono\n");
        return;
    }

    entropy_decode(ctx, count, 0);
    ape_apply_filters(ctx, decoded0, NULL, count);

    /* Now apply the predictor decoding */
    predictor_decode_mono(ctx, count);

    /* Pseudo-stereo - just copy left channel to right channel */
    if (ctx->channels == 2) {
        while (count--) {
            left = *decoded0;
            *(decoded1++) = *(decoded0++) = left;
        }
    }
}

static void ape_unpack_stereo(APEContext * ctx, int count)
{
    int32_t left, right;
    int32_t *decoded0 = ctx->decoded0;
    int32_t *decoded1 = ctx->decoded1;

    if ((ctx->frameflags & APE_FRAMECODE_STEREO_SILENCE) == APE_FRAMECODE_STEREO_SILENCE) {
        /* We are pure silence, so we're done. */
        //fprintf (stderr, "pure silence stereo\n");
        return;
    }

    entropy_decode(ctx, count, 1);
    ape_apply_filters(ctx, decoded0, decoded1, count);

    /* Now apply the predictor decoding */
    predictor_decode_stereo(ctx, count);

    /* Decorrelate and scale to output depth */
    while (count--) {
        left = *decoded1 - (*decoded0 / 2);
        right = left + *decoded0;

        *(decoded0++) = left;
        *(decoded1++) = right;
    }
}

static int
ape_decode_frame(DB_fileinfo_t *_info, void *data, int *data_size)
{
    ape_info_t *info = (ape_info_t*)_info;
    APEContext *s = &info->ape_ctx;
    char *samples = data;
    int nblocks;
    int i, n;
    int blockstodecode;
    long bytes_used;
    int samplesize = _info->fmt.bps/8 * s->channels;;

    /* should not happen but who knows */
    if (BLOCKS_PER_LOOP * samplesize > *data_size) {
        fprintf (stderr, "ape: Packet size is too big! (max is %d while you have %d)\n", *data_size, BLOCKS_PER_LOOP * samplesize);
        return -1;
    }

    if (s->packet_remaining < PACKET_BUFFER_SIZE) {
        if (s->samples == 0) {
            if (s->currentframe == s->totalframes) {
                return -1;
            }
            assert (!s->samples);
            trace ("start reading packet %d\n", s->currentframe);
            assert (s->samples == 0); // all samples from prev packet must have been read
            // start new packet
            if (ape_read_packet (info->fp, s) < 0) {
                fprintf (stderr, "ape: error reading packet\n");
                return -1;
            }
            bswap_buf((uint32_t*)(s->packet_data), (const uint32_t*)(s->packet_data), s->packet_remaining >> 2);

            s->ptr = s->last_ptr = s->packet_data;

            nblocks = s->samples = bytestream_get_be32(&s->ptr);

            trace ("s->samples=%d (1)\n", s->samples);
            n = bytestream_get_be32(&s->ptr);
            if(n < 0 || n > 3){
                trace ("ape: Incorrect offset passed\n");
                return -1;
            }
            s->ptr += n;

            s->currentframeblocks = nblocks;

            //buf += 4;
            if (s->samples <= 0) {
                *data_size = 0;
                bytes_used = s->packet_remaining;
                goto error;
            }

            memset(s->decoded0,  0, sizeof(s->decoded0));
            memset(s->decoded1,  0, sizeof(s->decoded1));

            /* Initialize the frame decoder */
            trace ("init_frame_decoder\n");
            init_frame_decoder(s);
        }
        else {
            int sz = PACKET_BUFFER_SIZE - s->packet_remaining;
            sz = min (sz, s->packet_sizeleft);
            sz = sz&~3;
            uint8_t *p = s->packet_data + s->packet_remaining;
            size_t r = deadbeef->fread (p, 1, sz, info->fp);
            bswap_buf((uint32_t*)p, (const uint32_t*)p, (int)r >> 2);
            s->packet_sizeleft -= r;
            s->packet_remaining += r;
            //fprintf (stderr, "read more %d bytes for current packet, sizeleft=%d, packet_remaining=%d\n", r, packet_sizeleft, packet_remaining);
        }
    }
    s->data_end = s->packet_data + s->packet_remaining;

    if (s->packet_remaining == 0 && !s->samples) {
        *data_size = 0;
        return 0;
    }
    if (!s->packet_remaining) {
        fprintf (stderr, "ape: packetbuf is empty!!\n");
        *data_size = 0;
        bytes_used = s->packet_remaining;
        goto error;
    }

    nblocks = s->samples;
    blockstodecode = min(BLOCKS_PER_LOOP, nblocks);

    s->error=0;

    if ((s->channels == 1) || (s->frameflags & APE_FRAMECODE_PSEUDO_STEREO))
        ape_unpack_mono(s, blockstodecode);
    else
        ape_unpack_stereo(s, blockstodecode);

    if(s->error || s->ptr >= s->data_end){
        s->samples=0;
        if (s->error) {
            fprintf (stderr, "ape: Error decoding frame, error=%d\n", s->error);
        }
        else {
            fprintf (stderr, "ape: Error decoding frame, ptr >= data_end\n");
        }
        return -1;
    }

    int skip = min (s->samplestoskip, blockstodecode);
    i = skip;

    if (_info->fmt.bps == 32) {
        for (; i < blockstodecode; i++) {
            *((int32_t*)samples) = s->decoded0[i];
            samples += 4;
            if(s->channels > 1) {
                *((int32_t*)samples) = s->decoded1[i];
                samples += 4;
            }
        }
    }
    else if (_info->fmt.bps == 24) {
        for (; i < blockstodecode; i++) {
            int32_t sample = s->decoded0[i];

            samples[0] = sample&0xff;
            samples[1] = (sample&0xff00)>>8;
            samples[2] = (sample&0xff0000)>>16;
            samples += 3;
            if(s->channels > 1) {
                sample = s->decoded1[i];
                samples[0] = sample&0xff;
                samples[1] = (sample&0xff00)>>8;
                samples[2] = (sample&0xff0000)>>16;
                samples += 3;
            }
        }
    }
    else if (_info->fmt.bps == 16) {
        for (; i < blockstodecode; i++) {
            *((int16_t*)samples) = (int16_t)s->decoded0[i];
            samples += 2;
            if(s->channels > 1) {
                *((int16_t*)samples) = (int16_t)s->decoded1[i];
                samples += 2;
            }
        }
    }
    else if (_info->fmt.bps == 8) {
        for (; i < blockstodecode; i++) {
            *samples = (int16_t)s->decoded0[i];
            samples++;
            if(s->channels > 1) {
                *samples = (int16_t)s->decoded1[i];
                samples++;
            }
        }
    }
    
    s->samplestoskip -= skip;
    s->samples -= blockstodecode;

    *data_size = (blockstodecode - skip) * samplesize;
//    ape_ctx.currentsample += blockstodecode - skip;
    bytes_used = s->samples ? s->ptr - s->last_ptr : s->packet_remaining;

    // shift everything
error:
    if (bytes_used < s->packet_remaining) {
        memmove (s->packet_data, s->packet_data+bytes_used, s->packet_remaining-bytes_used);
    }
    s->packet_remaining -= bytes_used;
    s->ptr -= bytes_used;
    s->last_ptr = s->ptr;

    return (int)bytes_used;
}

static DB_playItem_t *
ffap_insert (ddb_playlist_t *plt, DB_playItem_t *after, const char *fname) {
    APEContext ape_ctx;
    memset (&ape_ctx, 0, sizeof (ape_ctx));
    DB_FILE *fp = deadbeef->fopen (fname);
    if (!fp) {
        return NULL;
    }

    int64_t fsize = deadbeef->fgetlength (fp);

    int skip = deadbeef->junk_get_leading_size (fp);
    if (skip > 0) {
        if (deadbeef->fseek (fp, skip, SEEK_SET)) {
            goto error;
        }
    }
    if (ape_read_header (fp, &ape_ctx) < 0) {
        fprintf (stderr, "ape: failed to read ape header\n");
        goto error;
    }
    if (ape_ctx.fileversion < APE_MIN_VERSION) {
        fprintf(stderr, "ape: unsupported file version - %.2f\n", ape_ctx.fileversion/1000.0);
        goto error;
    }

    float duration = ape_ctx.totalsamples / (float)ape_ctx.samplerate;
    DB_playItem_t *it = NULL;
    it = deadbeef->pl_item_alloc_init (fname, plugin.plugin.id);
    deadbeef->pl_add_meta (it, ":FILETYPE", "APE");
    deadbeef->plt_set_item_duration (plt, it, duration);
 
    /*int v2err = */deadbeef->junk_id3v2_read (it, fp);
    int v1err = deadbeef->junk_id3v1_read (it, fp);
    if (v1err >= 0) {
        if (deadbeef->fseek (fp, -128, SEEK_END)) {
            goto error;
        }
    }
    else {
        if (deadbeef->fseek (fp, 0, SEEK_END)) {
            goto error;
        }
    }
    /*int apeerr = */deadbeef->junk_apev2_read (it, fp);

    deadbeef->fclose (fp);
    fp = NULL;

    char s[100];
    snprintf (s, sizeof (s), "%lld", (long long)fsize);
    deadbeef->pl_add_meta (it, ":FILE_SIZE", s);
    snprintf (s, sizeof (s), "%d", ape_ctx.bps);
    deadbeef->pl_add_meta (it, ":BPS", s);
    snprintf (s, sizeof (s), "%d", ape_ctx.channels);
    deadbeef->pl_add_meta (it, ":CHANNELS", s);
    snprintf (s, sizeof (s), "%d", ape_ctx.samplerate);
    deadbeef->pl_add_meta (it, ":SAMPLERATE", s);
    int br = (int)roundf(fsize / duration * 8 / 1000);
    snprintf (s, sizeof (s), "%d", br);
    deadbeef->pl_add_meta (it, ":BITRATE", s);

    DB_playItem_t *cue = deadbeef->plt_process_cue (plt, after, it, ape_ctx.totalsamples, ape_ctx.samplerate);
    if (cue) {
        deadbeef->pl_item_unref (it);
        ape_free_ctx (&ape_ctx);
        return cue;
    }

    deadbeef->pl_add_meta (it, "title", NULL);

    after = deadbeef->plt_insert_item (plt, after, it);
    deadbeef->pl_item_unref (it);

    ape_free_ctx (&ape_ctx);
    return after;

error:
    if (fp) {
        deadbeef->fclose (fp);
    }
    if (ape_ctx.packet_data) {
        ape_free_ctx (&ape_ctx);
    }
    return NULL;

}

static int
ffap_read (DB_fileinfo_t *_info, char *buffer, int size) {
    ape_info_t *info = (ape_info_t*)_info;

    int samplesize = _info->fmt.bps / 8 * info->ape_ctx.channels;

    if (info->ape_ctx.currentsample + size / samplesize > info->endsample) {
        size = (int)((info->endsample - info->ape_ctx.currentsample + 1) * samplesize);
        trace ("size truncated to %d bytes (%d samples), cursample=%d, info->endsample=%d, totalsamples=%d\n", size, size / samplesize, info->ape_ctx.currentsample, info->endsample, info->ape_ctx.totalsamples);
        if (size <= 0) {
            return 0;
        }
    }
    int inits = size;
    while (size > 0) {
        if (info->ape_ctx.remaining > 0) {
            int sz = min (size, info->ape_ctx.remaining);
            memcpy (buffer, info->ape_ctx.buffer, sz);
            buffer += sz;
            size -= sz;
            if (info->ape_ctx.remaining > sz) {
                memmove (info->ape_ctx.buffer, info->ape_ctx.buffer + sz, info->ape_ctx.remaining-sz);
            }
            info->ape_ctx.remaining -= sz;
            continue;
        }
        int s = BLOCKS_PER_LOOP * 2 * 2 * 2;
        assert (info->ape_ctx.remaining <= s/2);
        s -= info->ape_ctx.remaining;
        uint8_t *buf = info->ape_ctx.buffer + info->ape_ctx.remaining;
        int n = ape_decode_frame (_info, buf, &s);
        if (n == -1) {
            break;
        }
        info->ape_ctx.remaining += s;

        int sz = min (size, info->ape_ctx.remaining);
        memcpy (buffer, info->ape_ctx.buffer, sz);
        buffer += sz;
        size -= sz;
        if (info->ape_ctx.remaining > sz) {
            memmove (info->ape_ctx.buffer, info->ape_ctx.buffer + sz, info->ape_ctx.remaining-sz);
        }
        info->ape_ctx.remaining -= sz;
    }
    info->ape_ctx.currentsample += (inits - size) / samplesize;
    _info->readpos = (info->ape_ctx.currentsample-info->startsample) / (float)_info->fmt.samplerate;
    return inits - size;
}

static int
ffap_seek_sample (DB_fileinfo_t *_info, int sample) {
    ape_info_t *info = (ape_info_t*)_info;
    sample += info->startsample;
    trace ("seeking to %d/%d\n", sample, info->ape_ctx.totalsamples);
    uint32_t newsample = sample;
    if (newsample > info->ape_ctx.totalsamples) {
        trace ("eof\n");
        return -1;
    }
    int nframe = newsample / info->ape_ctx.blocksperframe;
    if (nframe >= info->ape_ctx.totalframes) {
        trace ("eof2\n");
        return -1;
    }
    info->ape_ctx.currentframe = nframe;
    info->ape_ctx.samplestoskip = newsample - nframe * info->ape_ctx.blocksperframe;
    trace ("seek to sample %d at blockstart\n", nframe * info->ape_ctx.blocksperframe);
    trace ("samples to skip: %d\n", info->ape_ctx.samplestoskip);

    // reset decoder
    info->ape_ctx.CRC = 0;
    info->ape_ctx.frameflags = 0;
    info->ape_ctx.currentframeblocks = 0;
    info->ape_ctx.blocksdecoded = 0;
    memset (&info->ape_ctx.predictor, 0, sizeof (info->ape_ctx.predictor));
    memset (info->ape_ctx.decoded0, 0, sizeof (info->ape_ctx.decoded0));
    memset (info->ape_ctx.decoded1, 0, sizeof (info->ape_ctx.decoded1));
    for (int i = 0; i < APE_FILTER_LEVELS; i++) {
        memset (info->ape_ctx.filterbuf[i], 0, info->ape_ctx.filterbuf_size[i]);
    }
    memset (&info->ape_ctx.rc, 0, sizeof (info->ape_ctx.rc));
    memset (&info->ape_ctx.riceX, 0, sizeof (info->ape_ctx.riceX));
    memset (&info->ape_ctx.riceY, 0, sizeof (info->ape_ctx.riceY));
    memset (info->ape_ctx.filters, 0, sizeof (info->ape_ctx.filters));
    memset (info->ape_ctx.packet_data, 0, PACKET_BUFFER_SIZE);
    info->ape_ctx.packet_sizeleft = 0;
    info->ape_ctx.data_end = NULL;
    info->ape_ctx.ptr = NULL;
    info->ape_ctx.last_ptr = NULL;
    info->ape_ctx.error = 0;
    memset (info->ape_ctx.buffer, 0, sizeof (info->ape_ctx.buffer));

    info->ape_ctx.remaining = 0;
    info->ape_ctx.packet_remaining = 0;
    info->ape_ctx.samples = 0;
    info->ape_ctx.currentsample = newsample;
    _info->readpos = (float)(newsample-info->startsample)/info->ape_ctx.samplerate;
    return 0;
}

static int
ffap_seek (DB_fileinfo_t *_info, float seconds) {
    return ffap_seek_sample (_info, seconds * _info->fmt.samplerate);
}


static int ffap_read_metadata (DB_playItem_t *it) {
    deadbeef->pl_lock ();
    const char *uri = strdupa (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();
    DB_FILE *fp = deadbeef->fopen (uri);
    if (!fp) {
        return -1;
    }
    deadbeef->pl_delete_all_meta (it);
    /*int apeerr = */deadbeef->junk_apev2_read (it, fp);
    /*int v2err = */deadbeef->junk_id3v2_read (it, fp);
    /*int v1err = */deadbeef->junk_id3v1_read (it, fp);
    deadbeef->pl_add_meta (it, "title", NULL);
    deadbeef->fclose (fp);
    return 0;
}

static int ffap_write_metadata (DB_playItem_t *it) {
    // get options
    int strip_id3v2 = deadbeef->conf_get_int ("ape.strip_id3v2", 0);
    int strip_id3v1 = 0;//deadbeef->conf_get_int ("ape.strip_id3v1", 0);
    int strip_apev2 = deadbeef->conf_get_int ("ape.strip_apev2", 0);
    int write_id3v2 = deadbeef->conf_get_int ("ape.write_id3v2", 0);
    int write_id3v1 = 0;//deadbeef->conf_get_int ("ape.write_id3v1", 0);
    int write_apev2 = deadbeef->conf_get_int ("ape.write_apev2", 1);

    uint32_t junk_flags = 0;
    if (strip_id3v2) {
        junk_flags |= JUNK_STRIP_ID3V2;
    }
    if (strip_id3v1) {
        junk_flags |= JUNK_STRIP_ID3V1;
    }
    if (strip_apev2) {
        junk_flags |= JUNK_STRIP_APEV2;
    }
    if (write_id3v2) {
        junk_flags |= JUNK_WRITE_ID3V2;
    }
    if (write_id3v1) {
        junk_flags |= JUNK_WRITE_ID3V1;
    }
    if (write_apev2) {
        junk_flags |= JUNK_WRITE_APEV2;
    }

    return deadbeef->junk_rewrite_tags (it, junk_flags, 4, NULL);
}

static const char *exts[] = { "ape", NULL };

// define plugin interface
static DB_decoder_t plugin = {
    DDB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_DECODER,
    .plugin.id = "ffap",
    .plugin.name = "Monkey's Audio (APE) decoder",
    .plugin.descr = "APE player based on code from libavc and rockbox",
    .plugin.copyright = 
        "Copyright (C) 2009-2013 Oleksiy Yakovenko <waker@users.sourceforge.net>\n"
        "\n"
        "based on apedec from FFMpeg Copyright (c) 2007 Benjamin Zores <ben@geexbox.org>\n"
        "based upon libdemac from Dave Chapman.\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .plugin.website = "http://deadbeef.sf.net",
    .open = ffap_open,
    .init = ffap_init,
    .free = ffap_free,
    .read = ffap_read,
    .seek = ffap_seek,
    .seek_sample = ffap_seek_sample,
    .insert = ffap_insert,
    .read_metadata = ffap_read_metadata,
    .write_metadata = ffap_write_metadata,
    .exts = exts,
};

#if HAVE_SSE2 && !ARCH_UNKNOWN

int32_t ff_scalarproduct_and_madd_int16_sse2(int16_t *v1, const int16_t *v2, const int16_t *v3, int order, int mul);

#define FF_MM_MMX      0x0001 ///< standard MMX
#define FF_MM_3DNOW    0x0004 ///< AMD 3DNOW
#define FF_MM_MMX2     0x0002 ///< SSE integer functions or AMD MMX ext
#define FF_MM_SSE      0x0008 ///< SSE functions
#define FF_MM_SSE2     0x0010 ///< PIV SSE2 functions
#define FF_MM_3DNOWEXT 0x0020 ///< AMD 3DNowExt
#define FF_MM_SSE3     0x0040 ///< Prescott SSE3 functions
#define FF_MM_SSSE3    0x0080 ///< Conroe SSSE3 functions
#define FF_MM_SSE4     0x0100 ///< Penryn SSE4.1 functions
#define FF_MM_SSE42    0x0200 ///< Nehalem SSE4.2 functions
#define FF_MM_IWMMXT   0x0100 ///< XScale IWMMXT
#define FF_MM_ALTIVEC  0x0001 ///< standard AltiVec

#ifdef __APPLE__
#define mm_support() FF_MM_SSE2
#else
/* ebx saving is necessary for PIC. gcc seems unable to see it alone */
#define cpuid(index,eax,ebx,ecx,edx)\
    __asm__ volatile\
        ("mov %%"REG_b", %%"REG_S"\n\t"\
         "cpuid\n\t"\
         "xchg %%"REG_b", %%"REG_S\
         : "=a" (eax), "=S" (ebx),\
           "=c" (ecx), "=d" (edx)\
         : "0" (index));

/* Function to test if multimedia instructions are supported...  */
int mm_support(void)
{
    int rval = 0;
    int eax, ebx, ecx, edx;
    int max_std_level, max_ext_level, std_caps=0, ext_caps=0;

#if ARCH_X86_32
    x86_reg a, c;
    __asm__ volatile (
        /* See if CPUID instruction is supported ... */
        /* ... Get copies of EFLAGS into eax and ecx */
        "pushfl\n\t"
        "pop %0\n\t"
        "mov %0, %1\n\t"

        /* ... Toggle the ID bit in one copy and store */
        /*     to the EFLAGS reg */
        "xor $0x200000, %0\n\t"
        "push %0\n\t"
        "popfl\n\t"

        /* ... Get the (hopefully modified) EFLAGS */
        "pushfl\n\t"
        "pop %0\n\t"
        : "=a" (a), "=c" (c)
        :
        : "cc"
        );

    if (a == c) {
        trace ("ffap: cpuid is not supported\n");
        return 0; /* CPUID not supported */
    }
#endif

    cpuid(0, max_std_level, ebx, ecx, edx);

    if(max_std_level >= 1){
        cpuid(1, eax, ebx, ecx, std_caps);
        if (std_caps & (1<<23))
            rval |= FF_MM_MMX;
        if (std_caps & (1<<25))
            rval |= FF_MM_MMX2
#ifdef HAVE_SSE2
                  | FF_MM_SSE;
        if (std_caps & (1<<26))
            rval |= FF_MM_SSE2;
        if (ecx & 1)
            rval |= FF_MM_SSE3;
        if (ecx & 0x00000200 )
            rval |= FF_MM_SSSE3;
        if (ecx & 0x00080000 )
            rval |= FF_MM_SSE4;
        if (ecx & 0x00100000 )
            rval |= FF_MM_SSE42;
#endif
                  ;
    }

    cpuid(0x80000000, max_ext_level, ebx, ecx, edx);

    if(max_ext_level >= 0x80000001){
        cpuid(0x80000001, eax, ebx, ecx, ext_caps);
        if (ext_caps & (1<<31))
            rval |= FF_MM_3DNOW;
        if (ext_caps & (1<<30))
            rval |= FF_MM_3DNOWEXT;
        if (ext_caps & (1<<23))
            rval |= FF_MM_MMX;
        if (ext_caps & (1<<22))
            rval |= FF_MM_MMX2;
    }

    return rval;
}
#endif
#endif

#if ARCH_ARM
int32_t EXTERN_ASMff_scalarproduct_int16_neon(int16_t *v1, int16_t *v2, int len,
                                    int shift);
int32_t EXTERN_ASMff_scalarproduct_and_madd_int16_neon(int16_t *v1, const int16_t *v2, const int16_t *v3, int order, int mul);

#endif

DB_plugin_t *
ffap_load (DB_functions_t *api) {
    // detect sse2
#if ARCH_ARM
        scalarproduct_and_madd_int16 = EXTERN_ASMff_scalarproduct_and_madd_int16_neon;
#elif HAVE_SSE2 && !ARCH_UNKNOWN
    trace ("ffap: was compiled with sse2 support\n");
    int mm_flags = mm_support ();
    if (mm_flags & FF_MM_SSE2) {
        trace ("ffap: sse2 support detected\n");
        scalarproduct_and_madd_int16 = ff_scalarproduct_and_madd_int16_sse2;
    }
    else {
        trace ("ffap: sse2 is not supported by CPU\n");
        scalarproduct_and_madd_int16 = scalarproduct_and_madd_int16_c;
    }
#else
//    trace ("ffap: sse2 support was not compiled in\n");
    scalarproduct_and_madd_int16 = scalarproduct_and_madd_int16_c;
#endif
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

#ifndef DCC_ENCODERS_H_
# define DCC_ENCODERS_H_

#include <setjmp.h>
#include "common/marshaller.h"
#include "common/quic.h"
#include "red_channel.h"
#include "red_parse_qxl.h"
#include "image-cache.h"
#include "glz-encoder.h"
#include "jpeg-encoder.h"
#include "zlib-encoder.h"

typedef struct RedCompressBuf RedCompressBuf;
typedef struct _GlzDrawableInstanceItem GlzDrawableInstanceItem;
typedef struct _RedGlzDrawable RedGlzDrawable;

void             dcc_encoders_init                           (DisplayChannelClient *dcc);
void             dcc_free_glz_drawable_instance              (DisplayChannelClient *dcc,
                                                              GlzDrawableInstanceItem *item);
void             dcc_free_glz_drawable                       (DisplayChannelClient *dcc,
                                                              RedGlzDrawable *drawable);
int              dcc_free_some_independent_glz_drawables     (DisplayChannelClient *dcc);
void             dcc_free_glz_drawables                      (DisplayChannelClient *dcc);
void             dcc_free_glz_drawables_to_free              (DisplayChannelClient* dcc);
void             dcc_freeze_glz                              (DisplayChannelClient *dcc);
void             dcc_release_glz                             (DisplayChannelClient *dcc);

void             marshaller_add_compressed                   (SpiceMarshaller *m,
                                                              RedCompressBuf *comp_buf,
                                                              size_t size);

RedCompressBuf*  compress_buf_new                            (void);
void             compress_buf_free                           (RedCompressBuf *buf);

struct RedCompressBuf {
    uint8_t buf[64 * 1024];
    RedCompressBuf *send_next;
};

typedef struct GlzSharedDictionary {
    RingItem base;
    GlzEncDictContext *dict;
    uint32_t refs;
    uint8_t id;
    pthread_rwlock_t encode_lock;
    int migrate_freeze;
    RedClient *client; // channel clients of the same client share the dict
} GlzSharedDictionary;

GlzSharedDictionary* dcc_get_glz_dictionary                  (DisplayChannelClient *dcc,
                                                              uint8_t id, int window_size);
GlzSharedDictionary* dcc_restore_glz_dictionary              (DisplayChannelClient *dcc,
                                                              uint8_t id,
                                                              GlzEncDictRestoreData *restore_data);

typedef struct  {
    DisplayChannelClient *dcc;
    RedCompressBuf *bufs_head;
    RedCompressBuf *bufs_tail;
    jmp_buf jmp_env;
    union {
        struct {
            SpiceChunks *chunks;
            int next;
            int stride;
            int reverse;
        } lines_data;
        struct {
            RedCompressBuf* next;
            int size_left;
        } compressed_data; // for encoding data that was already compressed by another method
    } u;
    char message_buf[512];
} EncoderData;

typedef struct {
    QuicUsrContext usr;
    EncoderData data;
} QuicData;

typedef struct {
    LzUsrContext usr;
    EncoderData data;
} LzData;

typedef struct {
    JpegEncoderUsrContext usr;
    EncoderData data;
} JpegData;

typedef struct {
    ZlibEncoderUsrContext usr;
    EncoderData data;
} ZlibData;

typedef struct {
    GlzEncoderUsrContext usr;
    EncoderData data;
} GlzData;

#define MAX_GLZ_DRAWABLE_INSTANCES 2

/* for each qxl drawable, there may be several instances of lz drawables */
/* TODO - reuse this stuff for the top level. I just added a second level of multiplicity
 * at the Drawable by keeping a ring, so:
 * Drawable -> (ring of) RedGlzDrawable -> (up to 2) GlzDrawableInstanceItem
 * and it should probably (but need to be sure...) be
 * Drawable -> ring of GlzDrawableInstanceItem.
 */
struct _GlzDrawableInstanceItem {
    RingItem glz_link;
    RingItem free_link;
    GlzEncDictImageContext *context;
    RedGlzDrawable         *glz_drawable;
};

struct _RedGlzDrawable {
    RingItem link;    // ordered by the time it was encoded
    RingItem drawable_link;
    RedDrawable *red_drawable;
    Drawable    *drawable;
    uint32_t     group_id;
    GlzDrawableInstanceItem instances_pool[MAX_GLZ_DRAWABLE_INSTANCES];
    Ring instances;
    uint8_t instances_count;
    DisplayChannelClient *dcc;
};

#define RED_RELEASE_BUNCH_SIZE 64

#endif /* DCC_ENCODERS_H_ */
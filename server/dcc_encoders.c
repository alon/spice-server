#include <glib.h>

#include "dcc_encoders.h"
#include "display_channel.h"

#define ZLIB_DEFAULT_COMPRESSION_LEVEL 3

static SPICE_GNUC_NORETURN SPICE_GNUC_PRINTF(2, 3) void
quic_usr_error(QuicUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_critical("%s", usr_data->message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static SPICE_GNUC_NORETURN SPICE_GNUC_PRINTF(2, 3) void
lz_usr_error(LzUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_critical("%s", usr_data->message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static SPICE_GNUC_PRINTF(2, 3) void
glz_usr_error(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);

    spice_critical("%s", usr_data->message_buf); // if global lz fails in the middle
                                        // the consequences are not predictable since the window
                                        // can turn to be unsynchronized between the server and
                                        // and the client
}

static SPICE_GNUC_PRINTF(2, 3) void
quic_usr_warn(QuicUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", usr_data->message_buf);
}

static SPICE_GNUC_PRINTF(2, 3) void
lz_usr_warn(LzUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", usr_data->message_buf);
}

static SPICE_GNUC_PRINTF(2, 3) void
glz_usr_warn(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(usr_data->message_buf, sizeof(usr_data->message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", usr_data->message_buf);
}

static void *quic_usr_malloc(QuicUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void *lz_usr_malloc(LzUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void *glz_usr_malloc(GlzEncoderUsrContext *usr, int size)
{
    return spice_malloc(size);
}

static void quic_usr_free(QuicUsrContext *usr, void *ptr)
{
    free(ptr);
}

static void lz_usr_free(LzUsrContext *usr, void *ptr)
{
    free(ptr);
}

static void glz_usr_free(GlzEncoderUsrContext *usr, void *ptr)
{
    free(ptr);
}

RedCompressBuf* compress_buf_new(void)
{
    RedCompressBuf *buf = g_slice_new(RedCompressBuf);

    buf->send_next = NULL;

    return buf;
}

void compress_buf_free(RedCompressBuf *buf)
{
    g_slice_free(RedCompressBuf, buf);
}

static int encoder_usr_more_space(EncoderData *enc_data, uint8_t **io_ptr)
{
    RedCompressBuf *buf;

    buf = compress_buf_new();
    enc_data->bufs_tail->send_next = buf;
    enc_data->bufs_tail = buf;
    buf->send_next = NULL;
    *io_ptr = buf->buf;
    return sizeof(buf->buf);
}

static int quic_usr_more_space(QuicUsrContext *usr, uint32_t **io_ptr, int rows_completed)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_space(usr_data, (uint8_t **)io_ptr) / sizeof(uint32_t);
}

static int lz_usr_more_space(LzUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static int glz_usr_more_space(GlzEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static int jpeg_usr_more_space(JpegEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static int zlib_usr_more_space(ZlibEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static inline int encoder_usr_more_lines(EncoderData *enc_data, uint8_t **lines)
{
    struct SpiceChunk *chunk;

    if (enc_data->u.lines_data.reverse) {
        if (!(enc_data->u.lines_data.next >= 0)) {
            return 0;
        }
    } else {
        if (!(enc_data->u.lines_data.next < enc_data->u.lines_data.chunks->num_chunks)) {
            return 0;
        }
    }

    chunk = &enc_data->u.lines_data.chunks->chunk[enc_data->u.lines_data.next];
    if (chunk->len % enc_data->u.lines_data.stride) {
        return 0;
    }

    if (enc_data->u.lines_data.reverse) {
        enc_data->u.lines_data.next--;
        *lines = chunk->data + chunk->len - enc_data->u.lines_data.stride;
    } else {
        enc_data->u.lines_data.next++;
        *lines = chunk->data;
    }

    return chunk->len / enc_data->u.lines_data.stride;
}

static int quic_usr_more_lines(QuicUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int lz_usr_more_lines(LzUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int glz_usr_more_lines(GlzEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int jpeg_usr_more_lines(JpegEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int zlib_usr_more_input(ZlibEncoderUsrContext *usr, uint8_t** input)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    int buf_size;

    if (!usr_data->u.compressed_data.next) {
        spice_assert(usr_data->u.compressed_data.size_left == 0);
        return 0;
    }

    *input = (uint8_t*)usr_data->u.compressed_data.next->buf;
    buf_size = MIN(sizeof(usr_data->u.compressed_data.next->buf),
                   usr_data->u.compressed_data.size_left);

    usr_data->u.compressed_data.next = usr_data->u.compressed_data.next->send_next;
    usr_data->u.compressed_data.size_left -= buf_size;
    return buf_size;
}

static void dcc_init_quic(DisplayChannelClient *dcc)
{
    dcc->quic_data.usr.error = quic_usr_error;
    dcc->quic_data.usr.warn = quic_usr_warn;
    dcc->quic_data.usr.info = quic_usr_warn;
    dcc->quic_data.usr.malloc = quic_usr_malloc;
    dcc->quic_data.usr.free = quic_usr_free;
    dcc->quic_data.usr.more_space = quic_usr_more_space;
    dcc->quic_data.usr.more_lines = quic_usr_more_lines;

    dcc->quic = quic_create(&dcc->quic_data.usr);
    spice_warn_if_fail(dcc->quic);
}

static void dcc_init_lz(DisplayChannelClient *dcc)
{
    dcc->lz_data.usr.error = lz_usr_error;
    dcc->lz_data.usr.warn = lz_usr_warn;
    dcc->lz_data.usr.info = lz_usr_warn;
    dcc->lz_data.usr.malloc = lz_usr_malloc;
    dcc->lz_data.usr.free = lz_usr_free;
    dcc->lz_data.usr.more_space = lz_usr_more_space;
    dcc->lz_data.usr.more_lines = lz_usr_more_lines;

    dcc->lz = lz_create(&dcc->lz_data.usr);
    spice_warn_if_fail(dcc->lz);
}

static void glz_usr_free_image(GlzEncoderUsrContext *usr, GlzUsrImageContext *image)
{
    GlzData *lz_data = (GlzData *)usr;
    GlzDrawableInstanceItem *glz_drawable_instance = (GlzDrawableInstanceItem *)image;
    DisplayChannelClient *drawable_cc = glz_drawable_instance->glz_drawable->dcc;
    DisplayChannelClient *this_cc = SPICE_CONTAINEROF(lz_data, DisplayChannelClient, glz_data);
    if (this_cc == drawable_cc) {
        dcc_free_glz_drawable_instance(drawable_cc, glz_drawable_instance);
    } else {
        /* The glz dictionary is shared between all DisplayChannelClient
         * instances that belong to the same client, and glz_usr_free_image
         * can be called by the dictionary code
         * (glz_dictionary_window_remove_head). Thus this function can be
         * called from any DisplayChannelClient thread, hence the need for
         * this check.
         */
        pthread_mutex_lock(&drawable_cc->glz_drawables_inst_to_free_lock);
        ring_add_before(&glz_drawable_instance->free_link,
                        &drawable_cc->glz_drawables_inst_to_free);
        pthread_mutex_unlock(&drawable_cc->glz_drawables_inst_to_free_lock);
    }
}

static void dcc_init_glz_data(DisplayChannelClient *dcc)
{
    dcc->glz_data.usr.error = glz_usr_error;
    dcc->glz_data.usr.warn = glz_usr_warn;
    dcc->glz_data.usr.info = glz_usr_warn;
    dcc->glz_data.usr.malloc = glz_usr_malloc;
    dcc->glz_data.usr.free = glz_usr_free;
    dcc->glz_data.usr.more_space = glz_usr_more_space;
    dcc->glz_data.usr.more_lines = glz_usr_more_lines;
    dcc->glz_data.usr.free_image = glz_usr_free_image;
}

static void dcc_init_jpeg(DisplayChannelClient *dcc)
{
    dcc->jpeg_data.usr.more_space = jpeg_usr_more_space;
    dcc->jpeg_data.usr.more_lines = jpeg_usr_more_lines;

    dcc->jpeg = jpeg_encoder_create(&dcc->jpeg_data.usr);
    spice_warn_if_fail(dcc->jpeg);
}

static void dcc_init_zlib(DisplayChannelClient *dcc)
{
    dcc->zlib_data.usr.more_space = zlib_usr_more_space;
    dcc->zlib_data.usr.more_input = zlib_usr_more_input;

    dcc->zlib = zlib_encoder_create(&dcc->zlib_data.usr, ZLIB_DEFAULT_COMPRESSION_LEVEL);
    spice_warn_if_fail(dcc->zlib);
}

void dcc_encoders_init(DisplayChannelClient *dcc)
{
    dcc_init_glz_data(dcc);
    dcc_init_quic(dcc);
    dcc_init_lz(dcc);
    dcc_init_jpeg(dcc);
    dcc_init_zlib(dcc);

    // todo: tune level according to bandwidth
    dcc->zlib_level = ZLIB_DEFAULT_COMPRESSION_LEVEL;
}

void marshaller_add_compressed(SpiceMarshaller *m,
                               RedCompressBuf *comp_buf, size_t size)
{
    size_t max = size;
    size_t now;
    do {
        spice_return_if_fail(comp_buf);
        now = MIN(sizeof(comp_buf->buf), max);
        max -= now;
        spice_marshaller_add_ref_full(m, (uint8_t*)comp_buf->buf, now,
                                      (spice_marshaller_item_free_func)compress_buf_free, NULL);
        comp_buf = comp_buf->send_next;
    } while (max);
}

/* Remove from the to_free list and the instances_list.
   When no instance is left - the RedGlzDrawable is released too. (and the qxl drawable too, if
   it is not used by Drawable).
   NOTE - 1) can be called only by the display channel that created the drawable
          2) it is assumed that the instance was already removed from the dictionary*/
void dcc_free_glz_drawable_instance(DisplayChannelClient *dcc,
                                    GlzDrawableInstanceItem *instance)
{
    DisplayChannel *display_channel = DCC_TO_DC(dcc);
    RedWorker *worker = display_channel->common.worker;
    RedGlzDrawable *glz_drawable;

    spice_return_if_fail(instance);
    spice_return_if_fail(instance->glz_drawable);

    glz_drawable = instance->glz_drawable;

    spice_return_if_fail(glz_drawable->dcc == dcc);
    spice_return_if_fail(glz_drawable->instances_count > 0);

    ring_remove(&instance->glz_link);
    glz_drawable->instances_count--;

    // when the remove callback is performed from the channel that the
    // drawable belongs to, the instance is not added to the 'to_free' list
    if (ring_item_is_linked(&instance->free_link)) {
        ring_remove(&instance->free_link);
    }

    if (ring_is_empty(&glz_drawable->instances)) {
        spice_return_if_fail(glz_drawable->instances_count == 0);

        Drawable *drawable = glz_drawable->drawable;

        if (drawable) {
            ring_remove(&glz_drawable->drawable_link);
        }
        red_drawable_unref(worker, glz_drawable->red_drawable,
                           glz_drawable->group_id);
        dcc->glz_drawable_count--;
        if (ring_item_is_linked(&glz_drawable->link)) {
            ring_remove(&glz_drawable->link);
        }
        free(glz_drawable);
    }
}

/*
 * Releases all the instances of the drawable from the dictionary and the display channel client.
 * The release of the last instance will also release the drawable itself and the qxl drawable
 * if possible.
 * NOTE - the caller should prevent encoding using the dictionary during this operation
 */
void dcc_free_glz_drawable(DisplayChannelClient *dcc, RedGlzDrawable *drawable)
{
    RingItem *head_instance = ring_get_head(&drawable->instances);
    int cont = (head_instance != NULL);

    while (cont) {
        if (drawable->instances_count == 1) {
            /* Last instance: dcc_free_glz_drawable_instance will free the drawable */
            cont = FALSE;
        }
        GlzDrawableInstanceItem *instance = SPICE_CONTAINEROF(head_instance,
                                                        GlzDrawableInstanceItem,
                                                        glz_link);
        if (!ring_item_is_linked(&instance->free_link)) {
            // the instance didn't get out from window yet
            glz_enc_dictionary_remove_image(dcc->glz_dict->dict,
                                            instance->context,
                                            &dcc->glz_data.usr);
        }
        dcc_free_glz_drawable_instance(dcc, instance);

        if (cont) {
            head_instance = ring_get_head(&drawable->instances);
        }
    }
}

/*
 * Remove from the global lz dictionary some glz_drawables that have no reference to
 * Drawable (their qxl drawables are released too).
 * NOTE - the caller should prevent encoding using the dictionary during the operation
 */
int dcc_free_some_independent_glz_drawables(DisplayChannelClient *dcc)
{
    RingItem *ring_link;
    int n = 0;

    if (!dcc) {
        return 0;
    }
    ring_link = ring_get_head(&dcc->glz_drawables);
    while ((n < RED_RELEASE_BUNCH_SIZE) && (ring_link != NULL)) {
        RedGlzDrawable *glz_drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        ring_link = ring_next(&dcc->glz_drawables, ring_link);
        if (!glz_drawable->drawable) {
            dcc_free_glz_drawable(dcc, glz_drawable);
            n++;
        }
    }
    return n;
}

void dcc_free_glz_drawables_to_free(DisplayChannelClient* dcc)
{
    RingItem *ring_link;

    if (!dcc->glz_dict) {
        return;
    }
    pthread_mutex_lock(&dcc->glz_drawables_inst_to_free_lock);
    while ((ring_link = ring_get_head(&dcc->glz_drawables_inst_to_free))) {
        GlzDrawableInstanceItem *drawable_instance = SPICE_CONTAINEROF(ring_link,
                                                                 GlzDrawableInstanceItem,
                                                                 free_link);
        dcc_free_glz_drawable_instance(dcc, drawable_instance);
    }
    pthread_mutex_unlock(&dcc->glz_drawables_inst_to_free_lock);
}

/* Clear all lz drawables - enforce their removal from the global dictionary.
   NOTE - prevents encoding using the dictionary during the operation*/
void dcc_free_glz_drawables(DisplayChannelClient *dcc)
{
    RingItem *ring_link;
    GlzSharedDictionary *glz_dict = dcc ? dcc->glz_dict : NULL;

    if (!glz_dict) {
        return;
    }

    // assure no display channel is during global lz encoding
    pthread_rwlock_wrlock(&glz_dict->encode_lock);
    while ((ring_link = ring_get_head(&dcc->glz_drawables))) {
        RedGlzDrawable *drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        // no need to lock the to_free list, since we assured no other thread is encoding and
        // thus not other thread access the to_free list of the channel
        dcc_free_glz_drawable(dcc, drawable);
    }
    pthread_rwlock_unlock(&glz_dict->encode_lock);
}

void dcc_freeze_glz(DisplayChannelClient *dcc)
{
    pthread_rwlock_wrlock(&dcc->glz_dict->encode_lock);
    dcc->glz_dict->migrate_freeze = TRUE;
    pthread_rwlock_unlock(&dcc->glz_dict->encode_lock);
}

static GlzSharedDictionary *glz_shared_dictionary_new(RedClient *client, uint8_t id,
                                                      GlzEncDictContext *dict)
{
    spice_return_val_if_fail(dict != NULL, NULL);

    GlzSharedDictionary *shared_dict = spice_new0(GlzSharedDictionary, 1);

    shared_dict->dict = dict;
    shared_dict->id = id;
    shared_dict->refs = 1;
    shared_dict->migrate_freeze = FALSE;
    shared_dict->client = client;
    ring_item_init(&shared_dict->base);
    pthread_rwlock_init(&shared_dict->encode_lock, NULL);

    return shared_dict;
}

static pthread_mutex_t glz_dictionary_list_lock = PTHREAD_MUTEX_INITIALIZER;
static Ring glz_dictionary_list = {&glz_dictionary_list, &glz_dictionary_list};

static GlzSharedDictionary *find_glz_dictionary(RedClient *client, uint8_t dict_id)
{
    RingItem *now;
    GlzSharedDictionary *ret = NULL;

    now = &glz_dictionary_list;
    while ((now = ring_next(&glz_dictionary_list, now))) {
        GlzSharedDictionary *dict = (GlzSharedDictionary *)now;
        if ((dict->client == client) && (dict->id == dict_id)) {
            ret = dict;
            break;
        }
    }

    return ret;
}

#define MAX_LZ_ENCODERS MAX_CACHE_CLIENTS

static GlzSharedDictionary *create_glz_dictionary(DisplayChannelClient *dcc,
                                                  uint8_t id, int window_size)
{
    spice_info("Lz Window %d Size=%d", id, window_size);

    GlzEncDictContext *glz_dict =
        glz_enc_dictionary_create(window_size, MAX_LZ_ENCODERS, &dcc->glz_data.usr);

    return glz_shared_dictionary_new(RED_CHANNEL_CLIENT(dcc)->client, id, glz_dict);
}

GlzSharedDictionary *dcc_get_glz_dictionary(DisplayChannelClient *dcc,
                                            uint8_t id, int window_size)
{
    GlzSharedDictionary *shared_dict;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = find_glz_dictionary(RED_CHANNEL_CLIENT(dcc)->client, id);
    if (shared_dict) {
        shared_dict->refs++;
    } else {
        shared_dict = create_glz_dictionary(dcc, id, window_size);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    }

    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

static GlzSharedDictionary *restore_glz_dictionary(DisplayChannelClient *dcc,
                                                   uint8_t id,
                                                   GlzEncDictRestoreData *restore_data)
{
    GlzEncDictContext *glz_dict =
        glz_enc_dictionary_restore(restore_data, &dcc->glz_data.usr);

    return glz_shared_dictionary_new(RED_CHANNEL_CLIENT(dcc)->client, id, glz_dict);
}

GlzSharedDictionary *dcc_restore_glz_dictionary(DisplayChannelClient *dcc,
                                                uint8_t id,
                                                GlzEncDictRestoreData *restore_data)
{
    GlzSharedDictionary *shared_dict = NULL;

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = find_glz_dictionary(RED_CHANNEL_CLIENT(dcc)->client, id);

    if (shared_dict) {
        shared_dict->refs++;
    } else {
        shared_dict = restore_glz_dictionary(dcc, id, restore_data);
        ring_add(&glz_dictionary_list, &shared_dict->base);
    }

    pthread_mutex_unlock(&glz_dictionary_list_lock);
    return shared_dict;
}

/* destroy encoder, and dictionary if no one uses it*/
void dcc_release_glz(DisplayChannelClient *dcc)
{
    GlzSharedDictionary *shared_dict;

    dcc_free_glz_drawables(dcc);

    glz_encoder_destroy(dcc->glz);
    dcc->glz = NULL;

    if (!(shared_dict = dcc->glz_dict)) {
        return;
    }

    dcc->glz_dict = NULL;
    pthread_mutex_lock(&glz_dictionary_list_lock);
    if (--shared_dict->refs) {
        pthread_mutex_unlock(&glz_dictionary_list_lock);
        return;
    }
    ring_remove(&shared_dict->base);
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    glz_enc_dictionary_destroy(shared_dict->dict, &dcc->glz_data.usr);
    free(shared_dict);
}

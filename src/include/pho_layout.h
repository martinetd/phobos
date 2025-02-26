/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2024 CEA/DAM.
 *
 *  This file is part of Phobos.
 *
 *  Phobos is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  Phobos is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with Phobos. If not, see <http://www.gnu.org/licenses/>.
 */
/**
 * \brief  Phobos Data Layout management.
 */
#ifndef _PHO_LAYOUT_H
#define _PHO_LAYOUT_H

#include <stdio.h>
#include <assert.h>
#include <stdbool.h>

#include "phobos_store.h"
#include "pho_dss.h"
#include "pho_srl_lrs.h"
#include "pho_io.h"

/**
 * Operation names for dynamic loading with dlsym()
 * This is the publicly exposed API that layout modules provide.
 *
 * See below for the corresponding prototypes and additional information.
 */
#define PLM_OP_INIT         "pho_layout_mod_register"

struct pho_io_descr;
struct layout_info;

struct pho_encoder;

/**
 * Operation provided by a layout module.
 *
 * See layout_encode and layout_decode for a more complete documentation.
 */
struct pho_layout_module_ops {
    /** Initialize a new encoder to put an object into phobos */
    int (*encode)(struct pho_encoder *enc);

    /** Initialize a new decoder to get an object from phobos */
    int (*decode)(struct pho_encoder *dec);

    /** Delete an object from phobos */
    int (*delete)(struct pho_encoder *dec);

    /** Retrieve one node name from which an object can be accessed */
    int (*locate)(struct dss_handle *dss, struct layout_info *layout,
                  const char *focus_host, char **hostname, int *nb_new_lock);

    /** Updates the information of the layout, object and extent based on the
     * medium's extent and the layout used.
     */
    int (*get_specific_attrs)(struct pho_io_descr *iod,
                              struct io_adapter_module *ioa,
                              struct extent *extent,
                              struct pho_attrs *layout_md);

    /** Updates the status of an object based on its extents */
    int (*reconstruct)(struct layout_info lyt, struct object_info *obj);
};

/**
 * Operations provided by a given encoder (or decoder, both are the same
 * structure with a different operation vector).
 *
 * The encoders communicate their needs to the LRS via requests (see pho_lrs.h)
 * and retrieve corresponding responses, allowing them to eventually perform the
 * required IOs.
 *
 * See layout_next_request, layout_receive_response and layout_destroy for a
 * more complete documentation.
 */
struct pho_enc_ops {
    /** Give a response and get requests from this encoder / decoder */
    int (*step)(struct pho_encoder *enc, pho_resp_t *resp,
                pho_req_t **reqs, size_t *n_reqs);

    /** Destroy this encoder / decoder */
    void (*destroy)(struct pho_encoder *enc);
};

/**
 * A layout module, implementing one way of encoding a file into a phobos
 * object (simple, raid1, compression, etc.).
 *
 * Each layout module fills this structure in its entry point (PLM_OP_INIT).
 */
struct layout_module {
    void *dl_handle;            /**< Handle to the layout plugin */
    struct module_desc desc;    /**< Description of this layout */
    const struct pho_layout_module_ops *ops; /**< Operations of this layout */
};

/**
 * The different types of the encoder
 */
enum encoder_type {
    PHO_ENC_ENCODER,
    PHO_ENC_DECODER,
    PHO_ENC_ERASER,
};


/** An encoder encoding or decoding one object on a set of media */
struct pho_encoder {
    void *priv_enc;                 /**< Layouts specific data */
    const struct pho_enc_ops *ops;  /**< Layouts specific operations */
    enum encoder_type type;         /**< Type of the encoder */
    bool done;                      /**< True if this encoder has no more work
                                      *  to do (check rc to know if an error
                                      *  happened)
                                      */
    struct pho_xfer_desc *xfer;     /**< Transfer descriptor (managed
                                      *  externally)
                                      */
    struct layout_info *layout;     /**< Layouts of the current transfer filled
                                      *  out when decoding
                                      */
    size_t io_block_size;           /**< Block size (in bytes) of the I/O buffer
                                      */
    pho_resp_t *last_resp;          /**< Last response from the LRS (use for
                                      *  a mput with no-split to keep the write
                                      *  resp)
                                      */
};

/**
 * Check is the encoder is of type encoder.
 */
static inline bool is_encoder(struct pho_encoder *enc)
{
    return enc->type == PHO_ENC_ENCODER;
}

/**
 * Check is the encoder is of type decoder.
 */
static inline bool is_decoder(struct pho_encoder *dec)
{
    return dec->type == PHO_ENC_DECODER;
}

/**
 * Check is the encoder is of type delete.
 */
static inline bool is_delete(struct pho_encoder *dec)
{
    return dec->type == PHO_ENC_ERASER;
}

static inline const char *encoder_type2str(struct pho_encoder *enc)
{
    switch (enc->type) {
    case PHO_ENC_ENCODER:
        return "encoder";
    case PHO_ENC_DECODER:
        return "decoder";
    case PHO_ENC_ERASER:
        return "eraser encoder";
    default:
        return "unknow";
    }
}

/**
 * @defgroup pho_layout_mod Public API for layout modules
 * @{
 */

/**
 * Not for direct call.
 * Entry point of layout modules.
 *
 * The function fills the module description (.desc) and operation (.ops) fields
 * for this specific layout module.
 *
 * Global initialization operations can be performed here if need be.
 */
int pho_layout_mod_register(struct layout_module *self);

/** @} end of pho_layout_mod group */

#define CHECK_ENC_OP(_enc, _func) do { \
    assert(_enc); \
    assert(_enc->ops); \
    assert(_enc->ops->_func); \
} while (0)

/**
 * Initialize a new encoder \a enc to put an object described by \a xfer in
 * phobos.
 *
 * @param[out]  enc     Encoder to be initialized
 * @param[in]   xfer    Transfer to make this encoder work on. A reference on it
 *                      will be kept as long as \a enc is in use and some of its
 *                      fields may be modified (notably xd_rc). A xfer may only
 *                      be handled by one encoder.
 *
 * @return 0 on success, -errno on error.
 */
int layout_encode(struct pho_encoder *enc, struct pho_xfer_desc *xfer);

/**
 * Initialize a new encoder \a dec to get an object described by \a xfer from
 * phobos.
 *
 * @param[out]  dec     Decoder to be initialized
 * @param[in]   xfer    Transfer to make this encoder work on. A reference on it
 *                      will be kept as long as \a enc is in use and some of its
 *                      fields may be modified (notably xd_rc).
 * @param[in]   layout  Layout of the object to retrieve. It is used in-place by
 *                      the decoder and must be freed separately by the caller
 *                      after the decoder is destroyed.
 * @return 0 on success, -errno on error.
 */
int layout_decode(struct pho_encoder *dec, struct pho_xfer_desc *xfer,
                  struct layout_info *layout);

/**
 * Initialize a new encoder \a dec to delete an object given by its ID \a oid.
 *
 * @param[out]  dec     Decoder to be initialized
 * @param[in]   xfer    Information of the object requested to be deleted
 * @param[in]   layout  Layout of the object to delete. It is used in-place by
 *                      the decoder and must be freed separately by the caller
 *                      after the encoder is destroyed
 * @return 0 on success, -errno on error
 */
int layout_delete(struct pho_encoder *dec, struct pho_xfer_desc *xfer,
                  struct layout_info *layout);

/**
 * Retrieve one node name from which an object can be accessed.
 *
 * @param[in]   dss         DSS handle
 * @param[in]   layout      Layout of the object to locate
 * @param[in]   focus_host  Hostname on which the caller would like to access
 *                          the object if there is no more convenient node (if
 *                          NULL, focus_host is set to local hostname)
 * @param[out]  hostname    Allocated and returned hostname of the node that
 *                          gives access to the object (NULL if an error
 *                          occurs)
 * @param[out]  nb_new_lock Number of new locks on media added for the returned
 *                          hostname
 *
 * @return                  0 on success or -errno on failure.
 *                          -ENODEV if there is no existing medium to retrieve
 *                          this layout
 *                          -EINVAL on invalid replica count or invalid layout
 *                          name
 *                          -EAGAIN if there is currently no convenient node to
 *                          retrieve this layout
 *                          -EADDRNOTAVAIL if we cannot get self hostname
 */
int layout_locate(struct dss_handle *dss, struct layout_info *layout,
                  const char *focus_host, char **hostname, int *nb_new_lock);

/**
 * Advance the layout operation of one step by providing a response from the LRS
 * (or NULL for the first call to this function) and collecting newly emitted
 * requests.
 *
 * @param[in]   enc     The encoder to advance.
 * @param[in]   resp    The response to pass to the encoder (or NULL for the
 *                      first call to this function).
 * @param[out]  reqs    Caller allocated array of newly emitted requests (to be
 *                      freed by the caller).
 * @param[out]  n_reqs  Number of emitted requests.
 *
 * @return 0 on success, -errno on error. -EINVAL is returned when the encoder
 * has already finished its work (the call to this function was unexpected).
 */
static inline int layout_step(struct pho_encoder *enc, pho_resp_t *resp,
                              pho_req_t **reqs, size_t *n_reqs)
{
    if (enc->done)
        return -EINVAL;

    CHECK_ENC_OP(enc, step);
    return enc->ops->step(enc, resp, reqs, n_reqs);
}

/**
 * Update extent and layout metadata without attributes retrieved from the
 * extent using the io adapter provided.
 *
 * @param[in]      iod         The I/O descriptor of the entry
 * @param[in]      ioa         I/O adapter to use to get metadata.
 * @param[out]     ext         The extent to update.
 * @param[in,out]  lyt         The layout to retrieve the info from, and the
 *                             attributes to update.
 *
 * @return 0 on success, -errno on failure.
 */
int layout_get_specific_attrs(struct pho_io_descr *iod,
                              struct io_adapter_module *ioa,
                              struct extent *extent,
                              struct layout_info *layout);

/**
 * Updates the status of the object according to its detected extents
 *
 * @param[in]   lyt     The layout containing the extents.
 * @param[out]  obj     The object to update.
 *
 * @return 0 on success, -errno on error.
 */
int layout_reconstruct(struct layout_info lyt, struct object_info *obj);

/**
 * Destroy this encoder or decoder and all associated resources.
 */
void layout_destroy(struct pho_encoder *enc);

#endif

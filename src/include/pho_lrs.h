/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright 2014-2015 CEA/DAM. All Rights Reserved.
 */
/**
 * \brief  Phobos Local Resource Scheduler (LRS)
 */
#ifndef _PHO_LRS_H
#define _PHO_LRS_H

#include "pho_types.h"


struct dss_handle;
struct device_descr;

#ifndef SWIG
enum lrs_operation {
    LRS_OP_NONE = 0,
    LRS_OP_READ,
    LRS_OP_WRITE,
    LRS_OP_FORMAT,
};

struct lrs_intent {
    struct dss_handle   *li_dss;
    struct dev_descr    *li_device;
    enum lrs_operation   li_operation;
    struct pho_ext_loc   li_location;
};

/**
 * Query to write a given amount of data with a given layout.
 * (future: several extents if the file is splitted, striped...)
 *
 * @param(in)  dss     Initialized DSS handle.
 * @param(in)  size    Size of the object to be written.
 * @param(in)  layout  The requested layout.
 * @param(out) intent  The intent descriptor to fill.
 * @return 0 on success, -1 * posix error code on failure
 */
int lrs_write_prepare(struct dss_handle *dss, size_t intent_size,
                      const struct layout_info *layout,
                      struct lrs_intent *intent);

/**
 * Query to read from a given set of media.
 * (future: several locations if the file is splitted, striped...
 *  Moreover, the object may have several locations and layouts if it is
 *  duplicated).
 *
 * @param(in)  dss     Initialized DSS handle.
 * @param(in)  layout  Data layout description.
 * @param(out) intent  The intent descriptor to fill.
 * @return 0 on success, -1 * posix error code on failure
 */
int lrs_read_prepare(struct dss_handle *dss, const struct layout_info *layout,
                     struct lrs_intent *intent);

/**
 * Declare the current operation (read/write) as finished and flush data.
 * @param(in) intent    the intent descriptor filled by lrs_intent_{read,write}.
 * @param(in) err_code  status of the copy (errno value).
 * @return 0 on success, -1 * posix error code on failure
 */
int lrs_done(struct lrs_intent *intent, int err_code);
#endif /* ^SWIG */

/**
 * Load and format a media to the given fs type.
 *
 * @param(in)   dss     Initialized DSS handle.
 * @param(in)   id      Media ID for the media to format.
 * @param(in)   fs      Filesystem type (only PHO_FS_LTFS for now).
 * @param(in)   unlock  Unlock tape if successfully formated.
 * @return 0 on success, negative error code on failure.
 */
int lrs_format(struct dss_handle *dss, const struct media_id *id,
               enum fs_type fs, bool unlock);
#endif

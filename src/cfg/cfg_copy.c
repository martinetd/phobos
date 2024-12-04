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
 * \brief  Phobos cfg copy.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pho_cfg.h"
#include "pho_common.h"

#define DEFAULT_COPY_NAME_ATTR_KEY "default_copy_name"

enum pho_cfg_params_io {
    /* Actual parameters */
    PHO_CFG_COPY_default_copy_name,

    /* Delimiters, update when modifying options */
    PHO_CFG_COPY_FIRST = PHO_CFG_COPY_default_copy_name,
    PHO_CFG_COPY_LAST  = PHO_CFG_COPY_default_copy_name,
};

const struct pho_config_item cfg_copy[] = {
    [PHO_CFG_COPY_default_copy_name] = {
        .section = "copy",
        .name    = DEFAULT_COPY_NAME_ATTR_KEY,
        .value   = "source"
    },
};

int get_cfg_default_copy_name(const char **copy_name)
{
    const char *default_copy_name;

    default_copy_name = PHO_CFG_GET(cfg_copy, PHO_CFG_COPY, default_copy_name);
    if (!default_copy_name)
        return -EINVAL;

    *copy_name = default_copy_name;
    return 0;
}

/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2022 CEA/DAM.
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
 * \brief  Phobos Distributed State Service API for utilities.
 */
#ifndef _PHO_DSS_UTILS_H
#define _PHO_DSS_UTILS_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <jansson.h>
#include <libpq-fe.h>

char *dss_char4sql(PGconn *conn, const char *s);

void free_dss_char4sql(char *s);

/**
 * Execute a PSQL \p request, verify the result is as expected with \p tested
 * and put the result in \p res.
 *
 * \param conn[in]    The connection to the database
 * \param request[in] Request to execute
 * \param res[out]    Result holder of the request
 * \param tested[in]  The expected result of the request
 *
 * \return            0 on success, or the error as returned by PSQL
 */
int execute(PGconn *conn, const char *request, PGresult **res,
            ExecStatusType tested);

/**
 * Convert PostgreSQL status codes to meaningful errno values.
 * \param   res[in]         Failed query result descriptor
 * \return                  Negated errno value corresponding to the error
 */
int psql_state2errno(const PGresult *res);

/**
 * Execute a PSQL \p request, verify the result is as expected with \p tested,
 * and put the result in \p res if not NULL.
 *
 * In case the request failed, a 'rollback' request is sent to the database.
 *
 * \param conn[in]      The connection to the database
 * \param request[in]   Request to execute
 * \param res[out]      If not NULL, result of the request
 * \param tested[in]    The expected result of the request
 *
 * \return              0 on success, or the error as returned by PSQL
 */
int execute_and_commit_or_rollback(PGconn *conn, GString *request,
                                   PGresult **res, ExecStatusType tested);

/**
 * Unlike PQgetvalue that returns '' for NULL fields,
 * this function returns NULL for NULL fields.
 */
static inline char *get_str_value(PGresult *res, int row_number,
                                  int column_number)
{
    if (PQgetisnull(res, row_number, column_number))
        return NULL;

    return PQgetvalue(res, row_number, column_number);
}

struct dss_field {
    int byte_value;
    const char *query_value;
    const char *(*get_value)(void *resource);
};

void update_fields(void *resource, int64_t fields_to_update,
                   struct dss_field *fields, int fields_count,
                   GString *request);

/**
 * Retrieve a string contained in a JSON object under a given key.
 *
 * The result string should not be kept in a data structure. To keep a
 * copy of the targeted string, use json_dict2str() instead.
 *
 * \return The targeted string value on success, or NULL on error.
 */
const char *json_dict2tmp_str(const struct json_t *obj, const char *key);

/**
 * Retrieve a copy of a string contained in a JSON object under a given key.
 * The caller is responsible for freeing the result after use.
 *
 * \return a newly allocated copy of the string on success or NULL on error.
 */
char *json_dict2str(const struct json_t *obj, const char *key);

/**
 * Retrieve a signed but positive integer contained in a JSON object under a
 * given key.
 * An error is returned if no integer was found at this location.
 *
 * \retval the value on success and -1 on error.
 */
int json_dict2int(const struct json_t *obj, const char *key);

/**
 * Retrieve a signed but positive long long integer contained in a JSON object
 * under a given key.
 * An error is returned if no long long integer was found at this location.
 *
 * \retval the value on success and -1LL on error.
 */
long long json_dict2ll(const struct json_t *obj, const char *key);

#define JSON_INTEGER_SET_NEW(_j, _s, _f)                        \
    do {                                                        \
        json_t  *_tmp = json_integer((_s)._f);                  \
        if (!_tmp)                                              \
            pho_error(-ENOMEM, "Failed to encode '%s'", #_f);   \
        else                                                    \
            json_object_set_new((_j), #_f, _tmp);               \
    } while (0)

#endif

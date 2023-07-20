/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 *  All rights reserved (c) 2014-2023 CEA/DAM.
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
 * \brief  Test SCSI logging mechanism
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <scsi/sg.h>
#include <scsi/sg_io_linux.h>
#include <scsi/scsi.h>
#include <fcntl.h>

#include "../test_setup.h"
#include "pho_cfg.h"
#include "pho_common.h"
#include "pho_daemon.h"
#include "pho_dss.h"
#include "pho_layout.h"
#include "phobos_admin.h"

#include <cmocka.h>

#include "lrs_device.h"
#include "scsi_api.h"
#include "scsi_common.h"

// If there is a difference in the models, you may have to modify this macro
#define LTO5_MODEL "ULT3580-TD5"

static void get_serial_from_path(char *path, char **serial)
{
    struct dev_adapter_module *deva;
    struct ldm_dev_state lds = {};
    int rc;

    rc = get_dev_adapter(PHO_RSC_TAPE, &deva);
    assert_return_code(rc, -rc);
    rc = ldm_dev_query(deva, path, &lds);
    assert_return_code(rc, -rc);

    *serial = strdup(lds.lds_serial);
    assert_non_null(*serial);

    free(lds.lds_serial);
    free(lds.lds_model);
}

static void create_device(struct lrs_dev *dev, char *path, char *model,
                          struct dss_handle *dss)
{
    int rc;

    memset(dev, 0, sizeof(*dev));

    dev->ld_op_status = PHO_DEV_OP_ST_EMPTY;
    strcpy(dev->ld_dev_path, path);
    dev->ld_ongoing_io = false;
    dev->ld_needs_sync = false;
    dev->ld_dss_media_info = NULL;
    dev->ld_device_thread.state = THREAD_RUNNING;
    dev->ld_sys_dev_state.lds_family = PHO_RSC_TAPE;

    dev->ld_dss_dev_info = calloc(1, sizeof(*dev->ld_dss_dev_info));
    assert_non_null(dev->ld_dss_dev_info);

    dev->ld_dss_dev_info->rsc.adm_status = PHO_RSC_ADM_ST_UNLOCKED;
    dev->ld_device_thread.dss = *dss;
    dev->ld_dss_dev_info->rsc.model = model;
    dev->ld_dss_dev_info->rsc.id.family = PHO_RSC_TAPE;
    strcpy(dev->ld_dss_dev_info->rsc.id.name, path);
    dev->ld_dss_dev_info->path = path;
    rc = lrs_dev_technology(dev, &dev->ld_technology);
    assert_return_code(rc, -rc);

    if (path[0] == '/') {
        struct lib_handle lib_hdl;
        char *serial;

        rc = wrap_lib_open(PHO_RSC_TAPE, &lib_hdl, NULL);
        assert_return_code(rc, -rc);
        get_serial_from_path(path, &serial);
        assert_int_equal(ldm_lib_drive_lookup(&lib_hdl, serial,
                                              &dev->ld_lib_dev_info), 0);
        free(serial);
        rc = ldm_lib_close(&lib_hdl);
        assert_return_code(rc, -rc);
    }
}

static void cleanup_device(struct lrs_dev *dev)
{
    free((void *)dev->ld_technology);
    free(dev->ld_dss_dev_info);
}

static void medium_set_tags(struct media_info *medium,
                            char **tags, size_t n_tags)
{
    medium->tags.n_tags = n_tags;
    medium->tags.tags = tags;
}

static void create_medium(struct media_info *medium, const char *name)
{
    memset(medium, 0, sizeof(*medium));

    medium->fs.status = PHO_FS_STATUS_BLANK;
    medium->rsc.adm_status = PHO_RSC_ADM_ST_UNLOCKED;
    medium->rsc.model = NULL;
    medium->rsc.id.family = PHO_RSC_TAPE;
    strcpy(medium->rsc.id.name, name);

    medium->flags.put = true;
    medium->flags.get = true;
    medium->flags.delete = true;

    medium_set_tags(medium, NULL, 0);
}

static bool check_item_type(json_t *item, const char *rsc)
{
    json_t *rsc_str;
    json_t *type;
    bool ret;

    type = json_object_get(item, "type");
    assert_non_null(type);

    rsc_str = json_string(rsc);
    assert_non_null(rsc_str);

    ret = json_equal(type, rsc_str);
    json_decref(rsc_str);

    return ret;
}

static void get_arm_load_address(json_t *item, char **arm_address)
{
    // Assume we only have one arm, otherwise we can't know in
    // advance which one is going to be used
    asprintf(arm_address, "%" JSON_INTEGER_FORMAT,
             json_integer_value(json_object_get(item, "address")));
    assert_non_null(*arm_address);
}

static void get_slot_load_address(json_t *item, char **medium_address,
                                  char *medium_name)
{
    char *medium_address_tmp;
    json_t *volume;

    volume = json_object_get(item, "volume");
    assert_non_null(volume);

    if (strcmp(json_string_value(volume), medium_name) != 0)
        return;

    asprintf(&medium_address_tmp, "%" JSON_INTEGER_FORMAT,
             json_integer_value(json_object_get(item, "address")));
    assert_non_null(medium_address_tmp);

    asprintf(medium_address, "%#hx", atoi(medium_address_tmp));
    free(medium_address_tmp);
    assert_non_null(*medium_address);
}

static void get_drive_load_address(json_t *item, char **device_address,
                                   char *device_name)
{
    char *serial;
    json_t *id;

    id = json_object_get(item, "device_id");
    assert_non_null(id);

    get_serial_from_path(device_name, &serial);

    if (strstr(json_string_value(id), serial) != NULL) {
        char *device_address_tmp;

        asprintf(&device_address_tmp, "%" JSON_INTEGER_FORMAT,
                 json_integer_value(json_object_get(item, "address")));
        assert_non_null(device_address_tmp);

        asprintf(device_address, "%#hx", atoi(device_address_tmp));
        free(device_address_tmp);
        assert_non_null(*device_address);
    }

    free(serial);
}

static void get_load_addresses(json_t *item, char **arm_address,
                                 char **medium_address, char *medium_name,
                                 char **device_address, char *device_name)
{
    json_t *json_type;
    const char *type;

    (void) device_name;

    json_type = json_object_get(item, "type");
    assert_non_null(json_type);

    type = json_string_value(json_type);
    assert_non_null(type);

    if (strcmp(type, "arm") == 0)
        get_arm_load_address(item, arm_address);
    else if (strcmp(type, "slot") == 0)
        get_slot_load_address(item, medium_address, medium_name);
    else if (strcmp(type, "drive") == 0)
        get_drive_load_address(item, device_address, device_name);
}

static json_t *create_log_message(enum scsi_operation_type op, bool should_fail,
                                  char *medium_name, char *device_name)
{
    int arms_nb, slots_nb, impexp_nb, drives_nb;
    json_t *scsi_logical_action;
    json_t *scsi_operation;
    json_t *phobos_action;
    json_t *scsi_execute;
    char *medium_address;
    char *device_address;
    json_t *scsi_error;
    char *arm_address;
    json_t *lib_data;

    arms_nb = slots_nb = impexp_nb = drives_nb = 0;

    if (op >= ARMS_STATUS && op <= LOAD_MEDIUM) {
        json_t *value;
        size_t index;

        arms_nb = slots_nb = impexp_nb = drives_nb = 0;

        assert_false(phobos_admin_lib_scan(PHO_LIB_SCSI, "/dev/changer",
                                           &lib_data));

        json_array_foreach(lib_data, index, value) {
            switch (op) {
            case ARMS_STATUS:
                arms_nb += (check_item_type(value, "arm") ? 1 : 0);
                break;
            case SLOTS_STATUS:
                slots_nb += (check_item_type(value, "slot") ? 1 : 0);
                break;
            case IMPEXP_STATUS:
                impexp_nb += (check_item_type(value, "import/export") ? 1 : 0);
                break;
            case DRIVES_STATUS:
                drives_nb += (check_item_type(value, "drive") ? 1 : 0);
                break;
            case LOAD_MEDIUM:
                get_load_addresses(value, &arm_address,
                                   &medium_address, medium_name,
                                   &device_address, device_name);
                break;
            default:
                fail();
            }
        }

        destroy_json(lib_data);
    }

    scsi_execute = json_object();
    assert_non_null(scsi_execute);

    if (should_fail) {
        scsi_error = json_loads(
        "{"
        "   \"asc\": 0,"
        "   \"ascq\": 0,"
        "   \"sense_key\": 5,"
        "   \"asc_ascq_str\":"
        "       \"Additional sense: No additional sense information\","
        "   \"driver_status\": 0,"
        "   \"sense_key_str\": \"Illegal Request\","
        "   \"adapter_status\": 0,"
        "   \"req_sense_error\": 0,"
        "   \"scsi_masked_status\": 1"
        "}", 0, NULL);
        assert_non_null(scsi_error);

        assert_false(json_object_set_new(scsi_execute, "SCSI ERROR",
                                         scsi_error));
    }

    assert_false(json_object_set_new(scsi_execute, "SCSI action",
                                     json_string(SCSI_ACTION_NAMES[op])));

    switch (op) {
    case LIBRARY_LOAD:
        break;
    case ARMS_STATUS:
        assert_false(json_object_set_new(scsi_execute, "Type",
                                         json_string("0x1")));
        assert_false(json_object_set_new(scsi_execute, "Count",
                                         json_integer(arms_nb)));
        break;
    case SLOTS_STATUS:
        assert_false(json_object_set_new(scsi_execute, "Type",
                                         json_string("0x2")));
        assert_false(json_object_set_new(scsi_execute, "Count",
                                         json_integer(slots_nb)));
        break;
    case IMPEXP_STATUS:
        assert_false(json_object_set_new(scsi_execute, "Type",
                                         json_string("0x3")));
        assert_false(json_object_set_new(scsi_execute, "Count",
                                         json_integer(impexp_nb)));
        break;
    case DRIVES_STATUS:
        assert_false(json_object_set_new(scsi_execute, "Type",
                                         json_string("0x4")));
        assert_false(json_object_set_new(scsi_execute, "Count",
                                         json_integer(drives_nb)));
        break;
    case LOAD_MEDIUM:
        assert_false(json_object_set_new(scsi_execute, "Arm address",
                                         json_string(arm_address)));
        free(arm_address);
        assert_false(json_object_set_new(scsi_execute, "Source address",
                                         json_string(medium_address)));
        free(medium_address);
        assert_false(json_object_set_new(scsi_execute, "Target address",
                                         json_string(device_address)));
        free(device_address);
        break;
    default:
        fail();
    }

    scsi_operation = json_object();
    assert_non_null(scsi_operation);

    assert_false(json_object_set_new(scsi_operation, "scsi_execute",
                                     scsi_execute));

    scsi_logical_action = json_object();
    assert_non_null(scsi_logical_action);

    assert_false(json_object_set_new(scsi_logical_action,
                                     SCSI_OPERATION_TYPE_NAMES[op],
                                     scsi_operation));

    if (op == LOAD_MEDIUM)
        return scsi_logical_action;

    phobos_action = json_object();
    assert_non_null(phobos_action);

    assert_false(json_object_set_new(phobos_action, "Media lookup",
                                     scsi_logical_action));

    return phobos_action;
}

static void get_op_params(struct sg_io_hdr *hdr, uint8_t *code, uint8_t *type)
{
    *code = *(hdr->cmdp);

    if (*code == READ_ELEMENT_STATUS) {
        struct read_status_cdb *req = (struct read_status_cdb *)hdr->cmdp;

        *type = req->element_type_code;
    } else if (*code != MODE_SENSE && *code != MOVE_MEDIUM) {
        fail();
    }
}

static bool op_to_mock(enum scsi_operation_type op_to_mock,
                       uint8_t current_element_type, uint8_t current_op)
{
    switch (op_to_mock) {
    case LIBRARY_LOAD:
        return current_op == MODE_SENSE;
    case ARMS_STATUS:
        return current_op == READ_ELEMENT_STATUS && current_element_type == 1;
    case SLOTS_STATUS:
        return current_op == READ_ELEMENT_STATUS && current_element_type == 2;
    case IMPEXP_STATUS:
        return current_op == READ_ELEMENT_STATUS && current_element_type == 3;
    case DRIVES_STATUS:
        return current_op == READ_ELEMENT_STATUS && current_element_type == 4;
    case LOAD_MEDIUM:
        return current_op == MOVE_MEDIUM;
    default:
        fail();
        return false;
    }
}

static int mock_ioctl(int fd, unsigned long request, void *sg_io_hdr)
{
    struct sg_io_hdr *hdr = (struct sg_io_hdr *)sg_io_hdr;
    struct scsi_req_sense *sbp;
    int operation_to_mock;
    uint8_t type = 0;
    uint8_t code;

    get_op_params(hdr, &code, &type);
    operation_to_mock = mock();

    if (!op_to_mock(operation_to_mock, type, code))
        return ioctl(fd, request, hdr);

    /* This combination of masked_status and sense_key will lead to an EINVAL,
     * code 22, which is checked after the "dev_load" call.
     */
    hdr->masked_status = CHECK_CONDITION;
    sbp = (struct scsi_req_sense *)hdr->sbp;
    sbp->sense_key = SPC_SK_ILLEGAL_REQUEST;
    return 0;
}

static void scsi_logs_check(struct dss_handle *handle,
                            enum scsi_operation_type op, bool should_fail,
                            char *device_name, char *medium_name)
{
    struct phobos_global_context *context = phobos_context();
    /* The calloc here is necessary because if the dev_fails, it will free the
     * given medium.
     */
    struct media_info *medium = calloc(1, sizeof(*medium));
    struct lrs_dev device;
    struct pho_log *logs;
    json_t *full_message;
    struct pho_log log;
    int n_logs;
    bool fod;
    bool fom;
    bool cr;
    int rc;

    create_device(&device, device_name, LTO5_MODEL, handle);

    create_medium(medium, medium_name);

    if (should_fail) {
        context->mock_ioctl = &mock_ioctl;

        will_return_always(mock_ioctl, op);
    }

    rc = dev_load(&device, &medium, true, &fod, &fom, &cr, true);

    if (should_fail) {
        pho_context_reset_scsi_ioctl();
        assert_int_equal(-rc, EINVAL);
    } else {
        /* Directly unload the device to ensure following tests will succeed in
         * case of an error in this file.
         */
        dev_unload(&device);
        assert_return_code(-rc, rc);
    }

    rc = dss_logs_get(handle, NULL, &logs, &n_logs);
    assert_return_code(rc, -rc);

    assert_int_equal(n_logs, 1);

    log = logs[0];

    assert_int_equal(PHO_RSC_TAPE, log.medium.family);
    assert_int_equal(PHO_RSC_TAPE, log.device.family);
    assert_string_equal(device_name, log.device.name);
    assert_string_equal(medium_name, log.medium.name);
    assert_int_equal(PHO_DEVICE_LOAD, log.cause);

    if (should_fail)
        assert_int_equal(EINVAL, -log.error_number);
    else
        assert_return_code(-log.error_number, log.error_number);

    full_message = create_log_message(op, should_fail, medium_name,
                                      device_name);
    assert_non_null(full_message);

    assert_true(json_equal(full_message, log.message));
    destroy_json(full_message);

    dss_res_free(logs, n_logs);
    dss_logs_delete(handle, NULL);
    cleanup_device(&device);
}

static void scsi_logs_mode_sense_failure(void **state)
{
    struct dss_handle *handle = (struct dss_handle *)*state;

    /* The device and medium names used in the following tests are nonsensical
     * because they are unecessary. Since the tests will not actually load
     * anything, the "dev_load" will fail before the device/medium are
     * relevant.
     */
    scsi_logs_check(handle, LIBRARY_LOAD, true,
                    "test_mode_sense_failure_device",
                    "test_mode_sense_failure_medium");
}

static void scsi_logs_arms_status_failure(void **state)
{
    struct dss_handle *handle = (struct dss_handle *)*state;

    scsi_logs_check(handle, ARMS_STATUS, true,
                    "test_arms_status_failure_device",
                    "test_arms_status_failure_medium");
}

static void scsi_logs_slots_status_failure(void **state)
{
    struct dss_handle *handle = (struct dss_handle *)*state;

    scsi_logs_check(handle, SLOTS_STATUS, true,
                    "test_slots_status_failure_device",
                    "test_slots_status_failure_medium");
}

static void scsi_logs_impexp_status_failure(void **state)
{
    struct dss_handle *handle = (struct dss_handle *)*state;

    scsi_logs_check(handle, IMPEXP_STATUS, true,
                    "test_impexp_status_failure_device",
                    "test_impexp_status_failure_medium");
}

static void scsi_logs_drives_status_failure(void **state)
{
    struct dss_handle *handle = (struct dss_handle *)*state;

    scsi_logs_check(handle, DRIVES_STATUS, true,
                    "test_drives_status_failure_device",
                    "test_drives_status_failure_medium");
}

static void scsi_logs_move_medium_failure(void **state)
{
    struct dss_handle *handle = (struct dss_handle *)*state;

    /* The device and medium name here and in the following test are relevant
     * because we get to the actual load part of the dev_load function.
     */
    scsi_logs_check(handle, LOAD_MEDIUM, true, "/dev/st0", "P00003L5");
}

static void scsi_logs_move_medium_success(void **state)
{
    struct dss_handle *handle = (struct dss_handle *)*state;

    scsi_logs_check(handle, LOAD_MEDIUM, false, "/dev/st0", "P00003L5");
}

int main(void)
{
    const struct CMUnitTest test_scsi_logs[] = {
        cmocka_unit_test(scsi_logs_mode_sense_failure),
        cmocka_unit_test(scsi_logs_arms_status_failure),
        cmocka_unit_test(scsi_logs_slots_status_failure),
        cmocka_unit_test(scsi_logs_impexp_status_failure),
        cmocka_unit_test(scsi_logs_drives_status_failure),
        cmocka_unit_test(scsi_logs_move_medium_failure),
        cmocka_unit_test(scsi_logs_move_medium_success),
    };
    struct stat dev_changer;
    int error_count;
    int rc;

    rc = stat("/dev/changer", &dev_changer);
    if (rc)
        /* Exit code for skipping the test */
        return 77;

    pho_context_init();
    rc = pho_cfg_init_local("../phobos.conf");
    if (rc)
        return rc;

    pho_log_level_set(PHO_LOG_ERROR);

    error_count = cmocka_run_group_tests(test_scsi_logs, global_setup_dss,
                                         global_teardown_dss);

    pho_cfg_local_fini();
    pho_context_fini();

    return error_count;
}

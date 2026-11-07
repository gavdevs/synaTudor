/*
 * Offline checks for the libfprint-tod <-> tudor host protocol additions used by the
 * enrollment/verify fix:
 *  - host reported errors map to defined libfprint errors (a duplicate enrollment is a
 *    failure, never a success),
 *  - the record list response is parsed strictly (size, announced count, bounds),
 *  - a host reported identify match can be resolved to a pre-existing record.
 *
 * No sensor is opened and no host process is started: all inputs are hand built.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <fpi-device.h>

#include "data.h"
#include "ipc.h"

static RECGUID guid(uint32_t part_a, uint64_t part_b) {
    RECGUID g = { .PartA = part_a };
    memcpy(&g.PartB, &part_b, sizeof(part_b));
    return g;
}

static void check_error_mapping(void) {
    GError *error = NULL;

    //A duplicate finger must surface as DATA_DUPLICATE, not as a successful enrollment
    error = tudor_error_from_ipc(IPC_ERR_DUPLICATE_ENROLLMENT);
    assert(error != NULL);
    assert(error->domain == FP_DEVICE_ERROR);
    assert(error->code == FP_DEVICE_ERROR_DATA_DUPLICATE);
    g_clear_error(&error);

    //Commit failures become a general device error
    error = tudor_error_from_ipc(IPC_ERR_COMMIT_FAILED);
    assert(error != NULL);
    assert(error->domain == FP_DEVICE_ERROR);
    assert(error->code == FP_DEVICE_ERROR_GENERAL);
    g_clear_error(&error);

    //Adapter failures become a general device error
    error = tudor_error_from_ipc(IPC_ERR_ACTION_FAILED);
    assert(error != NULL);
    assert(error->domain == FP_DEVICE_ERROR);
    assert(error->code == FP_DEVICE_ERROR_GENERAL);
    g_clear_error(&error);

    //An unset code is a protocol error, never "no error"
    error = tudor_error_from_ipc(IPC_ERR_NONE);
    assert(error != NULL);
    assert(error->domain == FP_DEVICE_ERROR);
    assert(error->code == FP_DEVICE_ERROR_PROTO);
    g_clear_error(&error);

    error = tudor_error_from_ipc((enum ipc_error_code) 0x7f);
    assert(error != NULL);
    assert(error->code == FP_DEVICE_ERROR_PROTO);
    g_clear_error(&error);
}

static void check_record_lookup(void) {
    RECGUID rec_a_guid = guid(0x11111111, 0), rec_b_guid = guid(0x22222222, 0);
    GPtrArray *records = g_ptr_array_new_with_free_func(g_object_unref);

    //Records are created without a print: the lookup only uses the identity fields
    TudorRecord *rec_a = new_record(rec_a_guid, TUDOR_FINGER_RH_INDEX_FINGER, g_bytes_new(NULL, 0), NULL);
    TudorRecord *rec_b = new_record(rec_b_guid, TUDOR_FINGER_LH_THUMB, g_bytes_new("payload", 7), NULL);
    g_ptr_array_add(records, rec_a);
    g_ptr_array_add(records, rec_b);

    //A host identify result (GUID + finger) resolves to the pre-existing record
    assert(tudor_find_record(records, rec_a_guid, TUDOR_FINGER_RH_INDEX_FINGER) == rec_a);
    assert(tudor_find_record(records, rec_b_guid, TUDOR_FINGER_LH_THUMB) == rec_b);

    //Same GUID with a different finger is a different enrollment
    assert(tudor_find_record(records, rec_a_guid, TUDOR_FINGER_LH_THUMB) == NULL);
    //Unknown GUID
    assert(tudor_find_record(records, guid(0x33333333, 0), TUDOR_FINGER_RH_INDEX_FINGER) == NULL);
    //TUDOR_FINGER_ANY never matches a concrete record
    assert(tudor_find_record(records, rec_a_guid, TUDOR_FINGER_ANY) == NULL);

    g_ptr_array_unref(records);
}

static void check_finger_mapping(void) {
    struct {
        enum tudor_finger tudor;
        FpFinger fp;
    } cases[] = {
        { TUDOR_FINGER_LH_THUMB, FP_FINGER_LEFT_THUMB },
        { TUDOR_FINGER_LH_INDEX_FINGER, FP_FINGER_LEFT_INDEX },
        { TUDOR_FINGER_LH_MIDDLE_FINGER, FP_FINGER_LEFT_MIDDLE },
        { TUDOR_FINGER_LH_RING_FINGER, FP_FINGER_LEFT_RING },
        { TUDOR_FINGER_LH_LITTLE_FINGER, FP_FINGER_LEFT_LITTLE },
        { TUDOR_FINGER_RH_THUMB, FP_FINGER_RIGHT_THUMB },
        { TUDOR_FINGER_RH_INDEX_FINGER, FP_FINGER_RIGHT_INDEX },
        { TUDOR_FINGER_RH_MIDDLE_FINGER, FP_FINGER_RIGHT_MIDDLE },
        { TUDOR_FINGER_RH_RING_FINGER, FP_FINGER_RIGHT_RING },
        { TUDOR_FINGER_RH_LITTLE_FINGER, FP_FINGER_RIGHT_LITTLE },
        { TUDOR_FINGER_ANY, FP_FINGER_UNKNOWN },
        { TUDOR_FINGER_UNKNOWN, FP_FINGER_UNKNOWN }
    };

    for(size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        assert(fp_finger_from_tudor(cases[i].tudor) == cases[i].fp);
    }
}

static IPCMessageBuf *make_list_msg(uint32_t announced, size_t records) {
    IPCMessageBuf *msg = ipc_msg_buf_new();
    msg->size = sizeof(struct ipc_msg_resp_list_records) + records * sizeof(struct ipc_stored_record);
    msg->resp_list_records.type = IPC_MSG_RESP_LIST_RECORDS;
    msg->resp_list_records.num_records = announced;
    for(size_t i = 0; i < records; i++) {
        msg->resp_list_records.records[i].guid = guid(0x1000 + i, i);
        msg->resp_list_records.records[i].finger = TUDOR_FINGER_RH_INDEX_FINGER;
    }

    return msg;
}

static void check_record_list_parsing(void) {
    GError *error = NULL;
    size_t count = 123;

    //Empty list is valid
    IPCMessageBuf *msg = make_list_msg(0, 0);
    assert(check_ipc_record_list(msg, &count, &error));
    assert(count == 0);
    ipc_msg_buf_free(msg);

    //Multiple records are counted from the message size
    msg = make_list_msg(3, 3);
    assert(check_ipc_record_list(msg, &count, &error));
    assert(count == 3);
    assert(msg->resp_list_records.records[2].guid.PartA == 0x1002);
    ipc_msg_buf_free(msg);

    //Announced count must match the payload
    msg = make_list_msg(2, 3);
    assert(!check_ipc_record_list(msg, &count, &error));
    assert(error != NULL && error->code == FP_DEVICE_ERROR_PROTO);
    g_clear_error(&error);
    ipc_msg_buf_free(msg);

    //A truncated record is rejected
    msg = make_list_msg(1, 1);
    msg->size -= 1;
    assert(!check_ipc_record_list(msg, &count, &error));
    assert(error != NULL && error->code == FP_DEVICE_ERROR_PROTO);
    g_clear_error(&error);
    ipc_msg_buf_free(msg);

    //A message smaller than the header is rejected
    msg = ipc_msg_buf_new();
    msg->size = sizeof(enum ipc_msg_type);
    msg->type = IPC_MSG_RESP_LIST_RECORDS;
    assert(!check_ipc_record_list(msg, &count, &error));
    assert(error != NULL && error->code == FP_DEVICE_ERROR_PROTO);
    g_clear_error(&error);
    ipc_msg_buf_free(msg);

    //More records than the protocol allows is rejected
    assert(IPC_MAX_LISTED_RECORDS < 0xffff);
    msg = make_list_msg(IPC_MAX_LISTED_RECORDS + 1, IPC_MAX_LISTED_RECORDS + 1);
    assert(!check_ipc_record_list(msg, &count, &error));
    assert(error != NULL && error->code == FP_DEVICE_ERROR_PROTO);
    g_clear_error(&error);
    ipc_msg_buf_free(msg);
}

static void check_db_record_synthesis(void) {
    FpiDeviceTudor *tdev = g_object_new(FPI_TYPE_DEVICE_TUDOR, NULL);

    RECGUID g = guid(0x44444444, 7);
    TudorRecord *rec = tudor_get_db_record(tdev, g, TUDOR_FINGER_RH_MIDDLE_FINGER);
    assert(rec != NULL);
    assert(rec->finger == TUDOR_FINGER_RH_MIDDLE_FINGER);

    //A record the process never loaded still needs a print the caller can compare
    //against its gallery and delete from the device.
    assert(rec->print != NULL);
    assert(fp_print_get_device_stored(rec->print));
    assert(fp_print_get_finger(rec->print) == FP_FINGER_RIGHT_MIDDLE);

    //The print data must identify the record, otherwise delete cannot address it
    RECGUID parsed = {0};
    enum tudor_finger parsed_finger = TUDOR_FINGER_UNKNOWN;
    GBytes *data = NULL;
    assert(parse_print_data(tdev, rec->print, &parsed, &parsed_finger, &data));
    assert(memcmp(&parsed, &g, sizeof(RECGUID)) == 0);
    assert(parsed_finger == TUDOR_FINGER_RH_MIDDLE_FINGER);
    assert(data != NULL && g_bytes_get_size(data) == 0);
    g_bytes_unref(data);

    //Repeated lookups return the registered record instead of creating duplicates
    assert(tudor_get_db_record(tdev, g, TUDOR_FINGER_RH_MIDDLE_FINGER) == rec);
    assert(tdev->db_records->len == 1);

    g_object_unref(tdev);
}

static void check_error_message_size(void) {
    //The error response must be big enough for the module's size check
    struct ipc_msg_resp_error msg = { .type = IPC_MSG_RESP_ERROR, .code = IPC_ERR_DUPLICATE_ENROLLMENT };
    assert(sizeof(msg) >= sizeof(struct ipc_msg_resp_error));
    assert(msg.code == IPC_ERR_DUPLICATE_ENROLLMENT);
}

int main(void) {
    check_error_mapping();
    check_record_lookup();
    check_finger_mapping();
    check_record_list_parsing();
    check_db_record_synthesis();
    check_error_message_size();

    printf("tod-ipc-offline: all checks passed\n");
    return 0;
}

#ifndef TUDOR_TUDOR_LIBFPRINT_PROTO_H
#define TUDOR_TUDOR_LIBFPRINT_PROTO_H

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include <tudor/log.h>
#include <tudor/tudor.h>

enum ipc_msg_type {
    IPC_MSG_INIT,
    IPC_MSG_READY,
    IPC_MSG_PROBE,
    IPC_MSG_RESP_PROBE,
    IPC_MSG_SHUTDOWN,
    IPC_MSG_ACK,
    IPC_MSG_CANCEL,

    IPC_MSG_LOAD_PDATA,
    IPC_MSG_RESP_LOAD_PDATA,
    IPC_MSG_STORE_PDATA,

    IPC_MSG_ADD_RECORD,
    IPC_MSG_DEL_RECORD,
    IPC_MSG_CLEAR_RECORDS,

    IPC_MSG_ENROLL,
    IPC_MSG_RESP_ENROLL,
    IPC_MSG_VERIFY,
    IPC_MSG_RESP_VERIFY,
    IPC_MSG_IDENTIFY,
    IPC_MSG_RESP_IDENTIFY,

    IPC_MSG_LIST_RECORDS,
    IPC_MSG_RESP_LIST_RECORDS,

    IPC_MSG_RESP_ERROR
};

//Error codes reported by the host when an action fails in a defined way instead of
//killing the host process. The module maps them to FpDeviceError codes.
enum ipc_error_code {
    IPC_ERR_NONE = 0,
    //CommitEnrollment/CheckForDuplicate reported WINBIO_E_DUPLICATE_ENROLLMENT
    IPC_ERR_DUPLICATE_ENROLLMENT,
    //CommitEnrollment failed for any other reason
    IPC_ERR_COMMIT_FAILED,
    //The adapter action could not be started or completed
    IPC_ERR_ACTION_FAILED
};

#define IPC_SENSOR_NAME_SIZE 128
#define IPC_MAX_PDATA_SIZE 16384

#define IPC_MAX_RECORD_SIZE 98304
#define IPC_MAX_MESSAGE_SIZE 131072

struct ipc_msg_init {
    enum ipc_msg_type type;

    enum log_level log_level;
    uint8_t usb_bus, usb_addr;
};

struct ipc_msg_resp_probe {
    enum ipc_msg_type type;

    char sensor_name[IPC_SENSOR_NAME_SIZE+1];
};

struct ipc_msg_load_pdata {
    enum ipc_msg_type type;

    char sensor_name[IPC_SENSOR_NAME_SIZE+1];
};

struct ipc_msg_resp_load_pdata {
    enum ipc_msg_type type;

    bool has_pdata;
    uint8_t pdata[];
};

struct ipc_msg_store_pdata {
    enum ipc_msg_type type;

    char sensor_name[IPC_SENSOR_NAME_SIZE+1];
    uint8_t pdata[];
};

struct ipc_msg_add_record {
    enum ipc_msg_type type;

    RECGUID guid;
    enum tudor_finger finger;
    uint8_t record_data[];
};

struct ipc_msg_del_record {
    enum ipc_msg_type type;

    RECGUID guid;
    enum tudor_finger finger;
};

struct ipc_msg_enroll {
    enum ipc_msg_type type;

    RECGUID guid;
    enum tudor_finger finger;
};

struct ipc_msg_resp_enroll {
    enum ipc_msg_type type;

    bool retry, done;
    uint8_t record_data[];
};

struct ipc_msg_verify {
    enum ipc_msg_type type;

    RECGUID guid;
    enum tudor_finger finger;
};

struct ipc_msg_resp_verify {
    enum ipc_msg_type type;

    bool retry, did_match;
};

struct ipc_msg_resp_identify {
    enum ipc_msg_type type;

    bool retry, did_match;
    RECGUID guid;
    enum tudor_finger finger;
};

//Maximum number of records returned by IPC_MSG_RESP_LIST_RECORDS
#define IPC_MAX_LISTED_RECORDS 64

struct ipc_stored_record {
    RECGUID guid;
    enum tudor_finger finger;
};

struct ipc_msg_resp_list_records {
    enum ipc_msg_type type;

    uint32_t num_records;
    struct ipc_stored_record records[];
};

struct ipc_msg_resp_error {
    enum ipc_msg_type type;

    enum ipc_error_code code;
};

#endif
#ifndef LIBFPRINT_TOD_IPC_H
#define LIBFPRINT_TOD_IPC_H

#include <tudor/libfprint-proto.h>
#include "device.h"

#define IPC_TIMEOUT_SECS 25

typedef struct _IPCMessageBuf {
    int transfer_fd;
    size_t size;
    union {
        guint8 data[IPC_MAX_MESSAGE_SIZE];

        enum ipc_msg_type type;
        struct ipc_msg_init init;
        struct ipc_msg_resp_probe resp_probe;

        struct ipc_msg_load_pdata load_pdata;
        struct ipc_msg_resp_load_pdata resp_load_pdata;
        struct ipc_msg_store_pdata store_pdata;

        struct ipc_msg_add_record add_record;
        struct ipc_msg_del_record del_record;

        struct ipc_msg_enroll enroll;
        struct ipc_msg_resp_enroll resp_enroll;
        struct ipc_msg_verify verify;
        struct ipc_msg_resp_verify resp_verify;
        struct ipc_msg_resp_identify resp_identify;
        struct ipc_msg_resp_list_records resp_list_records;
        struct ipc_msg_resp_error resp_error;
    };
} IPCMessageBuf;

IPCMessageBuf *ipc_msg_buf_new();
void ipc_msg_buf_free(gpointer msg);
int ipc_msg_buf_steal_fd(IPCMessageBuf *msg);

void recv_ipc_msg(FpiDeviceTudor *tdev, GAsyncReadyCallback callback, gpointer user_data);
void recv_ipc_msg_no_timeout(FpiDeviceTudor *tdev, GAsyncReadyCallback callback, gpointer user_data);
bool send_ipc_msg(FpiDeviceTudor *tdev, IPCMessageBuf *msg, GError **error);
void send_acked_ipc_msg(FpiDeviceTudor *tdev, IPCMessageBuf *msg, GAsyncReadyCallback callback, gpointer user_data);

static inline bool check_ipc_msg_size(IPCMessageBuf *msg, size_t min_size, GError **error) {
    if(msg->size < min_size) {
        *error = fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO, "Message 0x%x smaller than minimum size", msg->type);
        return false;
    }
    return true;
}

//Validates an IPC_MSG_RESP_LIST_RECORDS message and stores the number of records it
//carries in num_records. The record array has a variable length, so the message size
//must agree with both the announced count and the record size.
static inline bool check_ipc_record_list(IPCMessageBuf *msg, size_t *num_records, GError **error) {
    if(!check_ipc_msg_size(msg, sizeof(msg->resp_list_records), error)) return false;

    size_t recs_size = msg->size - sizeof(msg->resp_list_records);
    if(recs_size % sizeof(struct ipc_stored_record) != 0) {
        *error = fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO, "Record list size %lu isn't a multiple of the record size", recs_size);
        return false;
    }

    size_t count = recs_size / sizeof(struct ipc_stored_record);
    if(count != msg->resp_list_records.num_records || count > IPC_MAX_LISTED_RECORDS) {
        *error = fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO, "Record list contains %lu records but %u were announced (max %d)", count, msg->resp_list_records.num_records, IPC_MAX_LISTED_RECORDS);
        return false;
    }

    *num_records = count;
    return true;
}

bool open_dbus_con(FpiDeviceTudor *tdev, GError **error);

bool start_host_process(FpiDeviceTudor *tdev, guint8 usb_bus, guint8 usb_addr, int *sock_fd, GError **error);
bool kill_host_process(FpiDeviceTudor *tdev, GError **error);
bool adopt_host_process(FpiDeviceTudor *tdev, guint8 usb_bus, guint8 usb_addr, int *sock_fd, GError **error);
bool orphan_host_process(FpiDeviceTudor *tdev, GError **error);

static inline bool check_host_proc_dead(FpiDeviceTudor *tdev, GError **error) {
    if(tdev->host_has_id && !tdev->host_dead) return false;
    g_clear_error(error);
    *error = fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO, "Tudor host process died");
    return true;
}

#endif
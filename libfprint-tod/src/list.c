#include "data.h"
#include "ipc.h"
#include "list.h"

static void list_recv_cb(GObject *src_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    FpiDeviceTudor *tdev = FPI_DEVICE_TUDOR(src_obj);
    unregister_cancel_handler(tdev);

    //Check for errors
    GError *error = NULL;
    IPCMessageBuf *msg = g_task_propagate_pointer(task, &error);
    if(!msg) {
        fpi_device_list_complete(FP_DEVICE(tdev), NULL, error);
        return;
    }

    //Handle the message
    switch(msg->type) {
        case IPC_MSG_ACK: {
            //Assume it's an ACK to a CANCEL message
            fpi_device_list_complete(FP_DEVICE(tdev), NULL, handle_cancel_ack(tdev));
        } break;
        case IPC_MSG_RESP_ERROR: {
            if(!check_ipc_msg_size(msg, sizeof(msg->resp_error), &error)) {
                fpi_device_list_complete(FP_DEVICE(tdev), NULL, error);
                break;
            }

            g_warning("Tudor host reported list failure code %d", msg->resp_error.code);
            fpi_device_list_complete(FP_DEVICE(tdev), NULL, tudor_error_from_ipc(msg->resp_error.code));
        } break;
        case IPC_MSG_RESP_LIST_RECORDS: {
            //Validate the record array
            size_t num_records;
            if(!check_ipc_record_list(msg, &num_records, &error)) {
                fpi_device_list_complete(FP_DEVICE(tdev), NULL, error);
                break;
            }

            //Build a print for each stored record. Sensor stored templates live on the sensor,
            //so the print only carries the identity: the (GUID, finger) pair is all the host
            //needs to verify/delete the record again.
            GPtrArray *prints = g_ptr_array_new_with_free_func(g_object_unref);
            for(size_t i = 0; i < num_records; i++) {
                struct ipc_stored_record *srec = &msg->resp_list_records.records[i];

                TudorRecord *rec = tudor_get_db_record(tdev, srec->guid, srec->finger);
                g_ptr_array_add(prints, g_object_ref(rec->print));
            }

            g_info("Tudor host listed %lu stored records", num_records);
            fpi_device_list_complete(FP_DEVICE(tdev), prints, NULL);
        } break;
        default: {
            fpi_device_list_complete(FP_DEVICE(tdev), NULL, fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO, "Unexpected message in list sequence: 0x%x", msg->type));
        } break;
    }

    ipc_msg_buf_free(msg);
}

void fpi_device_tudor_list(FpDevice *dev) {
    FpiDeviceTudor *tdev = FPI_DEVICE_TUDOR(dev);

    //Check if host process is dead
    GError *error = NULL;
    if(check_host_proc_dead(tdev, &error)) {
        fpi_device_list_complete(dev, NULL, error);
        return;
    }

    //Send the list request
    g_debug("Sending tudor list records IPC message...");
    tdev->send_msg->size = sizeof(enum ipc_msg_type);
    tdev->send_msg->type = IPC_MSG_LIST_RECORDS;
    if(!send_ipc_msg(tdev, tdev->send_msg, &error)) {
        fpi_device_list_complete(dev, NULL, error);
        return;
    }

    //Register cancellation handler
    register_cancel_handler(tdev);

    //Start response listener
    recv_ipc_msg_no_timeout(tdev, list_recv_cb, NULL);
}

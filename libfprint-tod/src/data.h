#ifndef LIBFPRINT_TOD_DATA_H
#define LIBFPRINT_TOD_DATA_H

#include <tudor/tudor.h>
#include <tudor/libfprint-proto.h>
#include "device.h"

struct _TudorRecord {
    GObject parent;

    RECGUID guid;
    enum tudor_finger finger;
    GBytes *data;
    FpPrint *print;
};
G_DECLARE_FINAL_TYPE(TudorRecord, tudor_record, TUDOR, RECORD, GObject);

bool load_pdata(FpiDeviceTudor *tdev, const gchar *sensor_name, GByteArray **pdata, GError **error);
bool store_pdata(FpiDeviceTudor *tdev, const gchar *sensor_name, GByteArray *pdata, GError **error);

TudorRecord *new_record(RECGUID guid, enum tudor_finger finger, GBytes *data, FpPrint *print);

//Maps a host reported IPC error to the matching libfprint device error.
GError *tudor_error_from_ipc(enum ipc_error_code code);

//Finds the record for a (GUID, finger) pair in a record array, or NULL.
TudorRecord *tudor_find_record(GPtrArray *records, RECGUID guid, enum tudor_finger finger);

//Returns the device record for a (GUID, finger) pair, creating and registering it
//(identity only, no template) when the host reports a record this process never saw.
//The returned record is owned by the device's record list.
TudorRecord *tudor_get_db_record(FpiDeviceTudor *tdev, RECGUID guid, enum tudor_finger finger);

//Translates a tudor finger enum into the libfprint one.
FpFinger fp_finger_from_tudor(enum tudor_finger finger);

void load_record(FpiDeviceTudor *tdev, TudorRecord *rec, GAsyncReadyCallback cb, gpointer user_data);
void insert_db_record(FpiDeviceTudor *tdev, TudorRecord *record);

bool parse_print_data(FpiDeviceTudor *tdev, FpPrint *print, RECGUID *guid, enum tudor_finger *finger, GBytes **data);
void set_print_record(FpiDeviceTudor *tdev, FpPrint *print, TudorRecord *record);

void fpi_device_tudor_delete(FpDevice *dev);
void fpi_device_clear_storage(FpDevice *dev);

#endif
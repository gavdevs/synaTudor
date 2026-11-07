#ifndef LIBTUDOR_TUDOR_TUDOR_H
#define LIBTUDOR_TUDOR_TUDOR_H

#include <stdbool.h>
#include <libusb.h>

typedef struct _HANDLE *HANDLE;

typedef struct {
    uint32_t PartA;
    uint16_t PartB, PartC;
    uint64_t PartD:16, PartE:48;
} RECGUID;

typedef struct _WINBIO_IDENTITY WINBIO_IDENTITY;
typedef struct _WINBIO_PIPELINE WINBIO_PIPELINE;

extern bool tudor_log_traces;
bool tudor_init();
bool tudor_shutdown();

typedef struct _async_res *tudor_async_res_t;
typedef void tudor_async_cb_fnc(tudor_async_res_t res, bool success, void *ctx);

void tudor_set_async_callback(tudor_async_res_t res, tudor_async_cb_fnc *cb, void *ctx);
void tudor_cancel_async(tudor_async_res_t res);
bool tudor_wait_async(tudor_async_res_t res);
void tudor_cleanup_async(tudor_async_res_t res);

struct tudor_pair_data {
    void *data;
    size_t data_size;
};
extern const struct tudor_pair_data *(*tudor_get_pdata_fnc)(const char *name);
extern void (*tudor_set_pdata_fnc)(const char *name, const struct tudor_pair_data *pdata);

struct tudor_device_state {
    bool pairing_in_process, unpairing_in_process;
    bool update_in_process;
    int init_fails, update_fails;
    uint32_t last_update_timestamp;
};

enum tudor_finger {
    TUDOR_FINGER_UNKNOWN = 0,
    TUDOR_FINGER_RH_THUMB = 1,
    TUDOR_FINGER_RH_INDEX_FINGER = 2,
    TUDOR_FINGER_RH_MIDDLE_FINGER = 3,
    TUDOR_FINGER_RH_RING_FINGER = 4,
    TUDOR_FINGER_RH_LITTLE_FINGER = 5,
    TUDOR_FINGER_LH_THUMB = 6,
    TUDOR_FINGER_LH_INDEX_FINGER = 7,
    TUDOR_FINGER_LH_MIDDLE_FINGER = 8,
    TUDOR_FINGER_LH_RING_FINGER = 9,
    TUDOR_FINGER_LH_LITTLE_FINGER = 10,
    TUDOR_FINGER_ANY = 0xff
};

struct tudor_record {
    struct tudor_record *prev, *next;
    struct tudor_record *res_next;

    WINBIO_IDENTITY *identity;

    RECGUID guid;
    enum tudor_finger finger;
    void *data;
    size_t data_size;

    //Additional WinBio storage record fields (used by match-on-sensor engines to
    //link a host identity to an enrollment that lives in the sensor database)
    void *payload;
    size_t payload_size;
    uint32_t *index_vector;
    size_t index_vector_count;
};

struct tudor_device {
    struct tudor_device_state state;

    //Driver data
    HANDLE reg_key;
    struct winwdf_device *wdf_device;
    struct winwdf_file *wdf_file;

    //WinBIO data
    WINBIO_PIPELINE *pipeline;
    HANDLE winbio_file;
    bool sensor_storage;   //enrollments live in the vendor storage adapter (sensor DB)
    bool storage_erased;   //the sensor database was erased; its session is gone

    //Enroll data
    bool enrolling;
    RECGUID enroll_guid;
    enum tudor_finger enroll_finger;

    //Database records
    pthread_mutex_t records_lock;
    struct tudor_record *records_head;
    struct tudor_record *result_records_head, *result_records_cursor;
};

bool tudor_open(struct tudor_device *device, libusb_device_handle *usb_dev, struct tudor_device_state *state);
bool tudor_close(struct tudor_device *device);

int tudor_wipe_records(struct tudor_device *device, RECGUID *guid, enum tudor_finger finger);
bool tudor_add_record(struct tudor_device *device, RECGUID guid, enum tudor_finger finger, const void *data, size_t data_size);

bool tudor_enroll_start(struct tudor_device *device, RECGUID guid, enum tudor_finger finger);
bool tudor_enroll_capture(struct tudor_device *device, bool *done, tudor_async_res_t *res);
bool tudor_enroll_commit(struct tudor_device *device, bool *is_duplicate);
bool tudor_enroll_discard(struct tudor_device *device);

bool tudor_verify(struct tudor_device *device, RECGUID guid, enum tudor_finger finger, bool *retry, bool *matches, tudor_async_res_t *res);
bool tudor_identify(struct tudor_device *device, bool *retry, bool *found_match, RECGUID *guid, enum tudor_finger *finger, tudor_async_res_t *res);

struct tudor_stored_record {
    RECGUID guid;
    enum tudor_finger finger;
    size_t template_size;
    size_t payload_size;
};

//Enumerate/deletes through the configured storage adapter (host list or sensor DB).
int tudor_storage_get_records(struct tudor_device *device, struct tudor_stored_record *records, int max_records);
//Counts enrollments matching a GUID/finger filter in the active storage backend.
//Returns -1 when enumeration fails, so callers can tell "none left" from "unknown".
int tudor_storage_count_matching_records(struct tudor_device *device, RECGUID *guid, enum tudor_finger finger);
bool tudor_storage_delete_records(struct tudor_device *device, RECGUID *guid, enum tudor_finger finger);

//Turns TUDOR_FINGER_ANY into a concrete enrolled finger for that identity, the way
//WinBioVerify does before it calls an engine adapter. Any other value is returned as is.
enum tudor_finger tudor_resolve_finger(struct tudor_device *device, RECGUID guid, enum tudor_finger finger);

//Fully zeroed WINBIO_IDENTITY for the given GUID (or a wildcard). The vendor adapters
//marshal the whole identity union to the driver, so a partially initialized identity
//would send stack bytes to the sensor.
WINBIO_IDENTITY tudor_guid_identity(RECGUID guid);
WINBIO_IDENTITY tudor_wildcard_identity(void);

//Erases the whole storage database (sensor backend only). This removes all
//enrollments, including any entry the record query API cannot enumerate. It is
//irreversible and pairing-dependent data may need to be re-created afterwards.
bool tudor_storage_erase_database(struct tudor_device *device);

#endif
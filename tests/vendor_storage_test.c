/*
 * Offline checks for the vendor (in-sensor) storage adapter integration.
 *
 * Loads the real vendor adapter DLL through libtudor and drives
 * WINBIO_STORAGE_INTERFACE with a synthetic pipeline. A test double stands in for
 * the WDF device file: it records the ioctls the adapter issues and completes them
 * as the driver would. No USB device is opened and no sensor database is touched.
 *
 * Two things are pinned here:
 *  1. the pipeline preconditions the vendor adapter requires before Attach
 *     (StorageContext == NULL, StorageHandle == INVALID_HANDLE_VALUE), and
 *  2. that record writes reach the device through IOCTL 0x442018 with the
 *     identity/subfactor/payload the caller supplied.
 */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tudor/tudor.h>
#include "tudor/internal.h"

#define IOCTL_STORAGE_ADD_RECORD 0x442018
#define IOCTL_STORAGE_QUERY 0x442030
#define IOCTL_STORAGE_ERASE_DATABASE 0x442028

struct ioctl_log {
    ULONG code;
    size_t in_size;
    uint8_t in[128];
};

static struct {
    int count;
    struct ioctl_log entries[16];
} ioctls;

//Reply the driver sends for 0x442030, captured verbatim from a real sensor
//(state/verify-fresh-00df.log:2624). The adapter parses it into one record.
static const uint8_t *canned_query_reply;
static size_t canned_query_reply_size;

static void fake_devctrl_reset(void) {
    memset(&ioctls, 0, sizeof(ioctls));
}

static NTSTATUS fake_devctrl(void *ctx, OVERLAPPED *ovlp, ULONG code, const void *in_buf, size_t in_size, void *out_buf, size_t out_size, void **op_ctx) {
    (void) ctx; (void) op_ctx;

    if(ioctls.count < (int) (sizeof(ioctls.entries) / sizeof(ioctls.entries[0]))) {
        struct ioctl_log *entry = &ioctls.entries[ioctls.count++];
        entry->code = code;
        entry->in_size = in_size;
        if(in_buf) memcpy(entry->in, in_buf, in_size < sizeof(entry->in) ? in_size : sizeof(entry->in));
    }

    //The real driver answers with a small status block; zeroes mean "success"
    if(out_buf && out_size) {
        memset(out_buf, 0, out_size);
        if(code == IOCTL_STORAGE_QUERY && canned_query_reply && canned_query_reply_size)
            memcpy(out_buf, canned_query_reply, canned_query_reply_size < out_size ? canned_query_reply_size : out_size);
    }
    winio_complete_overlapped(ovlp, STATUS_SUCCESS, out_size >= 8 ? 8 : out_size);
    return STATUS_SUCCESS;
}

static NTSTATUS fake_cancel(void *ctx, OVERLAPPED *ovlp, void *op_ctx) { (void) ctx; (void) ovlp; (void) op_ctx; return STATUS_SUCCESS; }
static void fake_cleanup(void *ctx, OVERLAPPED *ovlp, void *op_ctx) { (void) ctx; (void) ovlp; (void) op_ctx; }

static const GUID *vendor_db_guid(void) {
    //DatabaseId constant compiled into the vendor adapter (see device.c)
    return (const GUID*) ((const uint8_t*) tudor_adapter_dll->image.base_addr + 0x1e410);
}

static WINBIO_PIPELINE make_pipeline(struct tudor_device *device) {
    WINBIO_PIPELINE pipeline = {0};
    pipeline.SensorInterface = tudor_sensor_adapter;
    pipeline.EngineInterface = tudor_engine_adapter;
    pipeline.StorageInterface = tudor_vendor_storage_adapter;
    pipeline.SensorHandle = winio_create_file(device, true, NULL, NULL, fake_devctrl, fake_cancel, fake_cleanup, NULL);
    pipeline.EngineHandle = INVALID_HANDLE_VALUE;
    pipeline.StorageHandle = INVALID_HANDLE_VALUE;
    pipeline.StorageContext = NULL;
    assert(pipeline.SensorHandle);
    return pipeline;
}

static void check_attach_preconditions(struct tudor_device *device) {
    //A pipeline that already carries a storage context is rejected
    WINBIO_PIPELINE pipeline = make_pipeline(device);
    pipeline.StorageContext = device;
    HRESULT hres = tudor_vendor_storage_adapter->Attach(&pipeline);
    printf("[TEST] Attach with StorageContext set -> 0x%x\n", hres);
    assert(hres != ERROR_SUCCESS);
    assert(pipeline.StorageContext == device);

    //So is one whose StorageHandle isn't INVALID_HANDLE_VALUE
    pipeline = make_pipeline(device);
    pipeline.StorageHandle = pipeline.SensorHandle;
    hres = tudor_vendor_storage_adapter->Attach(&pipeline);
    printf("[TEST] Attach with StorageHandle set -> 0x%x\n", hres);
    assert(hres != ERROR_SUCCESS);
}

static void check_lifecycle(struct tudor_device *device) {
    WINBIO_PIPELINE pipeline = make_pipeline(device);
    fake_devctrl_reset();

    assert(tudor_vendor_storage_adapter->Attach(&pipeline) == ERROR_SUCCESS);
    assert(pipeline.StorageContext != NULL);
    assert(tudor_vendor_storage_adapter->PipelineInit(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Activate(&pipeline) == ERROR_SUCCESS);

    //The adapter validates every OpenDatabase argument before touching the context
    const char16_t *empty = u"";
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, NULL, empty, empty) == E_POINTER);
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, (GUID*) vendor_db_guid(), NULL, empty) == E_POINTER);
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, (GUID*) vendor_db_guid(), empty, NULL) == E_POINTER);
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, (GUID*) vendor_db_guid(), empty, empty) == ERROR_SUCCESS);

    //Record write: identity + subfactor + payload must reach the device. The identity
    //must be built through the helper: the adapter marshals the whole union to the
    //driver, so an identity with an uninitialized tail ships stack bytes into the
    //sensor record and into the tags derived from it.
    WINBIO_IDENTITY identity = tudor_guid_identity((RECGUID) {
        .PartA = 0x10203040, .PartB = 0x5060, .PartC = 0x7080, .PartD = 0x90a0, .PartE = 0xb0c0d0e0f001
    });
    UCHAR payload[4] = { 0xde, 0xad, 0xbe, 0xef };
    WINBIO_STORAGE_RECORD srec = {
        .Identity = &identity, .SubFactor = TUDOR_FINGER_RH_INDEX_FINGER,
        .TemplateBlob = NULL, .TemplateBlobSize = 0,
        .PayloadBlob = payload, .PayloadBlobSize = sizeof(payload)
    };
    assert(tudor_vendor_storage_adapter->AddRecord(&pipeline, &srec) == ERROR_SUCCESS);

    struct ioctl_log *add = NULL;
    for(int i = 0; i < ioctls.count; i++) {
        if(ioctls.entries[i].code == IOCTL_STORAGE_ADD_RECORD) add = &ioctls.entries[i];
    }
    assert(add);
    //Byte-exact record layout, verified against the vendor adapter and the driver logs:
    //  0x00 ULONG             identity Type (WINBIO_ID_TYPE_GUID)
    //  0x04 GUID              identity value
    //  0x14..0x4b             rest of the identity union - must be zero
    //  0x4c ULONG             subfactor
    //  0x50 SIZE_T            payload size
    //  0x58 ..                payload bytes
    assert(add->in_size == 0x58 + sizeof(payload) + 7);
    assert(add->in[0] == WINBIO_ID_TYPE_GUID);
    assert(memcmp(add->in + 4, &identity.TemplateGuid, sizeof(GUID)) == 0);
    for(size_t i = 0x14; i <= 0x4b; i++) assert(add->in[i] == 0);
    assert(add->in[0x4c] == TUDOR_FINGER_RH_INDEX_FINGER);
    assert(*(uint64_t*) (add->in + 0x50) == sizeof(payload));
    assert(memcmp(add->in + 0x58, payload, sizeof(payload)) == 0);

    //Invalid records are refused before any ioctl is issued
    int before = ioctls.count;
    WINBIO_IDENTITY wildcard = { .Type = WINBIO_ID_TYPE_WILDCARD, .Wildcard = 1 };
    WINBIO_STORAGE_RECORD bad = srec;
    bad.Identity = &wildcard;
    assert(tudor_vendor_storage_adapter->AddRecord(&pipeline, &bad) == E_INVALIDARG);
    bad = srec;
    bad.PayloadBlob = NULL;  //size without pointer
    assert(tudor_vendor_storage_adapter->AddRecord(&pipeline, &bad) == E_POINTER);
    assert(ioctls.count == before);

    //Queries against the empty test database report "no results", they do not invent records
    assert(tudor_vendor_storage_adapter->QueryBySubject(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER) == WINBIO_E_DATABASE_NO_RESULTS);
    SIZE_T num_records = 0;
    assert(tudor_vendor_storage_adapter->GetRecordCount(&pipeline, &num_records) == WINBIO_E_DATABASE_NO_RESULTS);
    assert(tudor_vendor_storage_adapter->FirstRecord(&pipeline) == WINBIO_E_DATABASE_NO_RESULTS);
    assert(tudor_vendor_storage_adapter->DeleteRecord(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER) == WINBIO_E_DATABASE_NO_SUCH_RECORD);

    //The CLI record enumerator must work through the vendor adapter too. The test
    //databases is empty, so it must report zero records rather than invent any.
    struct tudor_device enumerated = {0};
    assert(pthread_mutex_init(&enumerated.records_lock, NULL) == 0);
    enumerated.pipeline = &pipeline;
    enumerated.sensor_storage = true;
    struct tudor_stored_record listed[4];
    int num_listed = tudor_storage_get_records(&enumerated, listed, 4);
    printf("[TEST] storage record enumeration -> %d\n", num_listed);
    assert(num_listed == 0);
    pthread_mutex_destroy(&enumerated.records_lock);

    //EraseDatabase is the only database-wide clear the adapter exposes: it must
    //reach the driver's erase ioctl (and nothing else) in one call, and it tears
    //the open database session down, so record writes are refused afterwards.
    fake_devctrl_reset();
    assert(tudor_vendor_storage_adapter->EraseDatabase(&pipeline, (GUID*) vendor_db_guid(), empty, empty) == ERROR_SUCCESS);
    assert(ioctls.count == 1);
    assert(ioctls.entries[0].code == IOCTL_STORAGE_ERASE_DATABASE);
    assert(tudor_vendor_storage_adapter->AddRecord(&pipeline, &srec) == 0x8009800f);
    //The session can be reopened on the now empty database
    //The erase leaves no database session behind: reopening fails until the
    //driver is restarted, and the close path must skip CloseDatabase.
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, (GUID*) vendor_db_guid(), empty, empty) == 0x80098015);

    //Close order: database first (skipped after an erase), then the lifecycle
    assert(tudor_vendor_storage_adapter->CloseDatabase(&pipeline) != ERROR_SUCCESS);  //session already gone
    assert(tudor_vendor_storage_adapter->Deactivate(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->PipelineCleanup(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Detach(&pipeline) == ERROR_SUCCESS);
    assert(pipeline.StorageContext == NULL);

    winhandle_destroy(pipeline.SensorHandle);
}

static void check_query_reply_parsing(struct tudor_device *device) {
    //0x442030 reply captured verbatim from the sensor (state/verify-fresh-00df.log:2624,
    //the run that queried identity 1 / finger 2 and then verified with that exact key).
    //It carries one record - identity GUID 1, subfactor 2 - and zero sizes for both blobs,
    //which is exactly what the CLI prints (template=0 payload=0) and why the engine's
    //enrollment cache skips the record ("No payload data for an enrollment"), leaving
    //stiTudorGetFirstEnrollment with nothing to return (115).
    static const uint8_t reply[] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x2b, 0x7f, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x49, 0x4b, 0xec, 0xe0, 0x55, 0x00, 0x00, 0x59, 0xab, 0xf7, 0x95,
        0xfd, 0x7f, 0x00, 0x00, 0x00, 0xd7, 0x4a, 0xec, 0xe0, 0x55, 0x00, 0x00,
    };
    canned_query_reply = reply;
    canned_query_reply_size = sizeof(reply);

    WINBIO_PIPELINE pipeline = make_pipeline(device);
    fake_devctrl_reset();
    assert(tudor_vendor_storage_adapter->Attach(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->PipelineInit(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Activate(&pipeline) == ERROR_SUCCESS);
    const char16_t *empty = u"";
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, (GUID*) vendor_db_guid(), empty, empty) == ERROR_SUCCESS);

    WINBIO_IDENTITY identity = tudor_guid_identity((RECGUID) { .PartA = 1 });
    assert(tudor_vendor_storage_adapter->QueryBySubject(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->FirstRecord(&pipeline) == ERROR_SUCCESS);

    WINBIO_STORAGE_RECORD rec = {0};
    assert(tudor_vendor_storage_adapter->GetCurrentRecord(&pipeline, &rec) == ERROR_SUCCESS);
    assert(rec.Identity != NULL);
    assert(rec.Identity->Type == WINBIO_ID_TYPE_GUID);
    assert(rec.Identity->TemplateGuid.PartA == 1);
    assert(rec.SubFactor == TUDOR_FINGER_RH_INDEX_FINGER);
    assert(rec.TemplateBlobSize == 0 && rec.TemplateBlob == NULL);
    assert(rec.PayloadBlobSize == 0 && rec.PayloadBlob == NULL);
    assert(rec.IndexElementCount == 0);
    //The device stores the identity with a zeroed union tail; the write side must send
    //it that way too (tudor_guid_identity), or the payload tag derived from those bytes
    //will not resolve on readback.
    for(size_t i = 20; i < sizeof(WINBIO_IDENTITY); i++) assert(((uint8_t*) rec.Identity)[i] == 0);

    assert(tudor_vendor_storage_adapter->Deactivate(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->PipelineCleanup(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Detach(&pipeline) == ERROR_SUCCESS);
    canned_query_reply = NULL;
    canned_query_reply_size = 0;
}

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    setvbuf(stdout, NULL, _IONBF, 0);
    LOG_LEVEL = LOG_ERROR;

    assert(tudor_init());
    assert(tudor_vendor_storage_adapter);
    assert(tudor_vendor_storage_adapter->Version.MajorVersion >= 1);
    assert(tudor_vendor_storage_adapter->AddRecord);
    assert(tudor_vendor_storage_adapter->DeleteRecord);
    assert(tudor_vendor_storage_adapter->QueryBySubject);
    assert(tudor_vendor_storage_adapter->OpenDatabase);
    assert(tudor_vendor_storage_adapter->CloseDatabase);

    static struct tudor_device device;
    assert(pthread_mutex_init(&device.records_lock, NULL) == 0);

    check_attach_preconditions(&device);
    check_lifecycle(&device);
    check_query_reply_parsing(&device);

    pthread_mutex_destroy(&device.records_lock);
    assert(tudor_shutdown());
    puts("[TEST] vendor storage adapter checks passed");
    return 0;
}

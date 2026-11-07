/*
 * Offline check of the WinBio enroll-commit <-> storage-adapter contract.
 *
 * Loads the real vendor adapter DLL (embedded in libtudor) and drives
 * WINBIO_ENGINE_INTERFACE::CommitEnrollment with synthetic pipelines for both
 * storage backends libtudor supports:
 *
 *  - host adapter: proves the record the engine hands over is stored and readable
 *    (identity, subfactor, payload) instead of being rejected.
 *  - vendor adapter: proves the engine's commit reaches the sensor record write
 *    (IOCTL 0x442018) with the identity and the payload attached.
 *
 * No USB device is opened: the pipeline's sensor handle is a test double that
 * records ioctls and completes them, and the engine never asks for a pending
 * template blob (EngineContext+0x120), so it stays off the real hardware path.
 */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tudor/tudor.h>
#include "tudor/internal.h"

#define ENGINE_CONTEXT_SIZE 0x400
#define ENGINE_CTX_ENROLL_ACTIVE_OFF 0x5c
#define ENGINE_CTX_PENDING_TEMPLATE_OFF 0x120
#define ENGINE_CTX_LOCK_OFF 0x168
#define IOCTL_STORAGE_ADD_RECORD 0x442018

extern void *resolve_windows_api(const char *name);
typedef BOOL (__winfnc *init_cs_fnc)(void *sect, DWORD spin_count, DWORD flags);

struct ioctl_log {
    ULONG code;
    size_t in_size;
    uint8_t in[128];
};

static struct {
    int count;
    struct ioctl_log entries[16];
} ioctls;

static NTSTATUS fake_devctrl(void *ctx, OVERLAPPED *ovlp, ULONG code, const void *in_buf, size_t in_size, void *out_buf, size_t out_size, void **op_ctx) {
    (void) ctx; (void) op_ctx;

    if(ioctls.count < (int) (sizeof(ioctls.entries) / sizeof(ioctls.entries[0]))) {
        struct ioctl_log *entry = &ioctls.entries[ioctls.count++];
        entry->code = code;
        entry->in_size = in_size;
        if(in_buf) memcpy(entry->in, in_buf, in_size < sizeof(entry->in) ? in_size : sizeof(entry->in));
    }

    if(out_buf && out_size) memset(out_buf, 0, out_size);
    winio_complete_overlapped(ovlp, STATUS_SUCCESS, out_size >= 8 ? 8 : out_size);
    return STATUS_SUCCESS;
}

static NTSTATUS fake_cancel(void *ctx, OVERLAPPED *ovlp, void *op_ctx) { (void) ctx; (void) ovlp; (void) op_ctx; return STATUS_SUCCESS; }
static void fake_cleanup(void *ctx, OVERLAPPED *ovlp, void *op_ctx) { (void) ctx; (void) ovlp; (void) op_ctx; }

static uint8_t *make_engine_context(void) {
    uint8_t *ctx = calloc(1, ENGINE_CONTEXT_SIZE);
    assert(ctx);

    //The engine takes this lock around every pipeline operation
    init_cs_fnc init_cs = (init_cs_fnc) resolve_windows_api("InitializeCriticalSectionEx");
    assert(init_cs);
    assert(init_cs(ctx + ENGINE_CTX_LOCK_OFF, 0, 0));

    ctx[ENGINE_CTX_ENROLL_ACTIVE_OFF] = 1;
    //Leave the pending template blob NULL so the engine never calls DeviceIoControl
    //for a template commit and stays on the storage path we are testing
    assert(*(void**) (ctx + ENGINE_CTX_PENDING_TEMPLATE_OFF) == NULL);
    return ctx;
}

static WINBIO_PIPELINE make_pipeline(struct tudor_device *device, WINBIO_STORAGE_INTERFACE *storage, bool sensor_storage) {
    WINBIO_PIPELINE pipeline = {0};
    pipeline.SensorInterface = tudor_sensor_adapter;
    pipeline.EngineInterface = tudor_engine_adapter;
    pipeline.StorageInterface = storage;
    pipeline.SensorHandle = winio_create_file(device, true, NULL, NULL, fake_devctrl, fake_cancel, fake_cleanup, NULL);
    pipeline.EngineHandle = INVALID_HANDLE_VALUE;
    pipeline.StorageHandle = INVALID_HANDLE_VALUE;
    pipeline.EngineContext = make_engine_context();
    //The vendor adapter allocates its own context and requires NULL here
    pipeline.StorageContext = sensor_storage ? NULL : device;
    assert(pipeline.SensorHandle);
    return pipeline;
}

static void teardown_pipeline(WINBIO_PIPELINE *pipeline) {
    winhandle_destroy(pipeline->SensorHandle);
    free(pipeline->EngineContext);
}

static WINBIO_IDENTITY make_identity(void) {
    return (WINBIO_IDENTITY) {
        .Type = WINBIO_ID_TYPE_GUID,
        .TemplateGuid = { .PartA = 0x00000001, .PartB = 0x2222, .PartC = 0x3333, .PartD = 0x4444, .PartE = 0x555566667777 }
    };
}

static void free_host_records(struct tudor_device *device) {
    while(device->records_head) {
        struct tudor_record *rec = device->records_head;
        device->records_head = rec->next;
        free(rec->data);
        free(rec->payload);
        free(rec->index_vector);
        free(rec->identity);
        free(rec);
    }
    device->result_records_head = device->result_records_cursor = NULL;
}

static void check_commit_host_storage(UCHAR *payload, SIZE_T payload_size) {
    struct tudor_device device = {0};
    assert(pthread_mutex_init(&device.records_lock, NULL) == 0);

    WINBIO_PIPELINE pipeline = make_pipeline(&device, tudor_storage_adapter, false);
    assert(tudor_storage_adapter->Attach(&pipeline) == ERROR_SUCCESS);
    assert(tudor_storage_adapter->PipelineInit(&pipeline) == ERROR_SUCCESS);

    WINBIO_IDENTITY identity = make_identity();
    printf("[TEST] host storage: CommitEnrollment(payload_size=%lu)...\n", (unsigned long) payload_size);
    HRESULT commit_res = tudor_engine_adapter->CommitEnrollment(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER, payload, payload_size);
    printf("[TEST] host storage: CommitEnrollment -> 0x%x\n", commit_res);
    assert(commit_res == ERROR_SUCCESS);

    //The record the engine asked the storage adapter to persist
    assert(tudor_storage_adapter->QueryBySubject(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER) == ERROR_SUCCESS);
    assert(tudor_storage_adapter->FirstRecord(&pipeline) == ERROR_SUCCESS);

    SIZE_T num_records = 0;
    assert(tudor_storage_adapter->GetRecordCount(&pipeline, &num_records) == ERROR_SUCCESS);
    assert(num_records == 1);

    WINBIO_STORAGE_RECORD srec = {0};
    assert(tudor_storage_adapter->GetCurrentRecord(&pipeline, &srec) == ERROR_SUCCESS);
    printf("[TEST] host storage: subfactor=%u template=%lu payload=%lu\n", (unsigned) srec.SubFactor,
        (unsigned long) srec.TemplateBlobSize, (unsigned long) srec.PayloadBlobSize);
    assert(srec.SubFactor == TUDOR_FINGER_RH_INDEX_FINGER);
    assert(srec.PayloadBlobSize == payload_size);
    if(payload_size) assert(srec.PayloadBlob && memcmp(srec.PayloadBlob, payload, payload_size) == 0);

    teardown_pipeline(&pipeline);
    free_host_records(&device);
    pthread_mutex_destroy(&device.records_lock);
}

static void check_commit_vendor_storage(UCHAR *payload, SIZE_T payload_size) {
    struct tudor_device device = {0};
    assert(pthread_mutex_init(&device.records_lock, NULL) == 0);

    WINBIO_PIPELINE pipeline = make_pipeline(&device, tudor_vendor_storage_adapter, true);
    memset(&ioctls, 0, sizeof(ioctls));

    assert(tudor_vendor_storage_adapter->Attach(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->PipelineInit(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Activate(&pipeline) == ERROR_SUCCESS);
    const GUID *db_guid = (const GUID*) ((const uint8_t*) tudor_adapter_dll->image.base_addr + 0x1e410);
    const char16_t *empty = u"";
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, (GUID*) db_guid, empty, empty) == ERROR_SUCCESS);

    WINBIO_IDENTITY identity = make_identity();
    printf("[TEST] vendor storage: CommitEnrollment(payload_size=%lu)...\n", (unsigned long) payload_size);
    HRESULT commit_res = tudor_engine_adapter->CommitEnrollment(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER, payload, payload_size);
    printf("[TEST] vendor storage: CommitEnrollment -> 0x%x (ioctls=%d)\n", commit_res, ioctls.count);
    assert(commit_res == ERROR_SUCCESS);

    //The engine's storage write must reach the device as a record ioctl
    struct ioctl_log *add = NULL;
    for(int i = 0; i < ioctls.count; i++) {
        if(ioctls.entries[i].code == IOCTL_STORAGE_ADD_RECORD) add = &ioctls.entries[i];
    }
    assert(add);
    assert(add->in[0] == TUDOR_FINGER_RH_INDEX_FINGER);
    assert(memcmp(add->in + 4, &identity.TemplateGuid, sizeof(GUID)) == 0);
    bool payload_seen = payload_size == 0;
    for(size_t i = 0; !payload_seen && i + payload_size <= add->in_size && i + payload_size <= sizeof(add->in); i++) {
        if(memcmp(add->in + i, payload, payload_size) == 0) payload_seen = true;
    }
    assert(payload_seen);
    printf("[TEST] vendor storage: record ioctl present (in_size=%lu, payload in record=%s)\n",
        (unsigned long) add->in_size, payload_seen ? "yes" : "no");

    //The query path against the test double must not invent records
    assert(tudor_vendor_storage_adapter->QueryBySubject(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER) == WINBIO_E_DATABASE_NO_RESULTS);

    assert(tudor_vendor_storage_adapter->CloseDatabase(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Deactivate(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->PipelineCleanup(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Detach(&pipeline) == ERROR_SUCCESS);

    teardown_pipeline(&pipeline);
    pthread_mutex_destroy(&device.records_lock);
}

static void check_enrollment_hash_failure_is_reported(void) {
    struct tudor_device device = {0};
    assert(pthread_mutex_init(&device.records_lock, NULL) == 0);

    WINBIO_PIPELINE pipeline = make_pipeline(&device, tudor_vendor_storage_adapter, true);
    assert(tudor_vendor_storage_adapter->Attach(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->PipelineInit(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Activate(&pipeline) == ERROR_SUCCESS);
    const GUID *db_guid = (const GUID*) ((const uint8_t*) tudor_adapter_dll->image.base_addr + 0x1e410);
    const char16_t *empty = u"";
    assert(tudor_vendor_storage_adapter->OpenDatabase(&pipeline, (GUID*) db_guid, empty, empty) == ERROR_SUCCESS);

    //A pipeline without pending enrollment data must report a failure, not a bogus hash
    UCHAR *hash = (UCHAR*) 0x1;
    SIZE_T hash_size = 0xdead;
    HRESULT hres = tudor_engine_adapter->GetEnrollmentHash(&pipeline, &hash, &hash_size);
    printf("[TEST] GetEnrollmentHash without enrollment data -> 0x%x\n", hres);
    assert(hres != ERROR_SUCCESS);

    //...and committing without a payload still has to work (the fallback path)
    WINBIO_IDENTITY identity = make_identity();
    assert(tudor_engine_adapter->CommitEnrollment(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER, NULL, 0) == ERROR_SUCCESS);

    assert(tudor_vendor_storage_adapter->CloseDatabase(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Deactivate(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->PipelineCleanup(&pipeline) == ERROR_SUCCESS);
    assert(tudor_vendor_storage_adapter->Detach(&pipeline) == ERROR_SUCCESS);

    teardown_pipeline(&pipeline);
    pthread_mutex_destroy(&device.records_lock);
}

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    setvbuf(stdout, NULL, _IONBF, 0);
    LOG_LEVEL = LOG_ERROR;
    tudor_log_traces = false;

    assert(tudor_init());
    assert(tudor_engine_adapter);
    assert(tudor_storage_adapter);
    assert(tudor_vendor_storage_adapter);

    UCHAR payload[32];
    memset(payload, 0xa5, sizeof(payload));

    check_commit_host_storage(NULL, 0);
    check_commit_host_storage(payload, sizeof(payload));
    check_commit_vendor_storage(NULL, 0);
    check_commit_vendor_storage(payload, sizeof(payload));
    check_enrollment_hash_failure_is_reported();

    assert(tudor_shutdown());
    puts("[TEST] all commit/storage contract checks passed");
    return 0;
}

/*
 * Offline checks for the identity serialization and the WinBio storage record
 * contract implemented by libtudor's host storage adapter, plus the record
 * enumeration/deletion helpers the CLI relies on.
 *
 * No driver DLL is loaded here: the host adapter is plain C.
 */
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tudor/tudor.h>
#include "tudor/internal.h"

static struct tudor_device device;
static WINBIO_PIPELINE pipeline;

static WINBIO_IDENTITY make_guid_identity(void) {
    return (WINBIO_IDENTITY) {
        .Type = WINBIO_ID_TYPE_GUID,
        .TemplateGuid = {
            .PartA = 0xdeadbeef,
            .PartB = 0x1234,
            .PartC = 0xabcd,
            .PartD = 0x7788,
            .PartE = 0x112233445566
        }
    };
}

static WINBIO_STORAGE_RECORD make_record(WINBIO_IDENTITY *identity, UCHAR subfactor, void *template, SIZE_T template_size, void *payload, SIZE_T payload_size) {
    return (WINBIO_STORAGE_RECORD) {
        .Identity = identity,
        .SubFactor = subfactor,
        .IndexVector = NULL,
        .IndexElementCount = 0,
        .TemplateBlob = template,
        .TemplateBlobSize = template_size,
        .PayloadBlob = payload,
        .PayloadBlobSize = payload_size
    };
}

static void check_identity_layout(void) {
    //Values from winbio_types.h: the vendor engine only accepts GUID/SID/SECURE_ID
    //(2..4) for stored identities, so an off-by-one enum would change the meaning.
    assert(WINBIO_ID_TYPE_NULL == 0);
    assert(WINBIO_ID_TYPE_WILDCARD == 1);
    assert(WINBIO_ID_TYPE_GUID == 2);
    assert(WINBIO_ID_TYPE_SID == 3);
    assert(WINBIO_ID_TYPE_SECURE_ID == 4);

    //Wire layout the vendor adapters consume: ULONG Type followed by the value union
    assert(sizeof(ULONG) == 4);
    assert(sizeof(GUID) == 16);
    assert(offsetof(WINBIO_IDENTITY, Type) == 0);
    assert(offsetof(WINBIO_IDENTITY, TemplateGuid) == 4);
    assert(offsetof(WINBIO_IDENTITY, AccountSid) == 4);
    assert(offsetof(WINBIO_IDENTITY, AccountSid.Size) == 4);
    assert(offsetof(WINBIO_IDENTITY, AccountSid.Data) == 8);
    assert(sizeof(WINBIO_IDENTITY) == 4 + 4 + 68);

    //A GUID identity must survive a byte copy unchanged
    WINBIO_IDENTITY identity = make_guid_identity();
    uint8_t copy[sizeof(GUID)];
    memcpy(copy, &identity.TemplateGuid, sizeof(copy));
    assert(memcmp(copy, &identity.TemplateGuid, sizeof(copy)) == 0);
    assert(identity.TemplateGuid.PartA == 0xdeadbeef);
    assert(identity.TemplateGuid.PartB == 0x1234);
    assert(identity.TemplateGuid.PartC == 0xabcd);
    assert(identity.TemplateGuid.PartD == 0x7788);
    assert(identity.TemplateGuid.PartE == 0x112233445566);
}

static void check_record_roundtrip(void) {
    WINBIO_IDENTITY identity = make_guid_identity();
    UCHAR template_blob[16], payload_blob[32];
    for(size_t i = 0; i < sizeof(template_blob); i++) template_blob[i] = (UCHAR) (0x10 + i);
    for(size_t i = 0; i < sizeof(payload_blob); i++) payload_blob[i] = (UCHAR) (0xa0 + i);

    WINBIO_STORAGE_RECORD srec = make_record(&identity, TUDOR_FINGER_RH_INDEX_FINGER, template_blob, sizeof(template_blob), payload_blob, sizeof(payload_blob));
    assert(tudor_storage_adapter->AddRecord(&pipeline, &srec) == ERROR_SUCCESS);

    //A duplicate identity/subfactor pair must be refused, not silently overwritten
    WINBIO_STORAGE_RECORD dup = srec;
    assert(tudor_storage_adapter->AddRecord(&pipeline, &dup) == WINBIO_E_DUPLICATE_ENROLLMENT);

    //Query by the exact identity returns the stored record, including the payload
    assert(tudor_storage_adapter->QueryBySubject(&pipeline, &identity, TUDOR_FINGER_RH_INDEX_FINGER) == ERROR_SUCCESS);
    assert(tudor_storage_adapter->FirstRecord(&pipeline) == ERROR_SUCCESS);

    SIZE_T num_records = 0;
    assert(tudor_storage_adapter->GetRecordCount(&pipeline, &num_records) == ERROR_SUCCESS);
    assert(num_records == 1);

    WINBIO_STORAGE_RECORD out = {0};
    assert(tudor_storage_adapter->GetCurrentRecord(&pipeline, &out) == ERROR_SUCCESS);
    assert(out.Identity != NULL);
    assert(out.Identity->Type == WINBIO_ID_TYPE_GUID);
    assert(memcmp(&out.Identity->TemplateGuid, &identity.TemplateGuid, sizeof(GUID)) == 0);
    assert(out.SubFactor == TUDOR_FINGER_RH_INDEX_FINGER);
    assert(out.TemplateBlobSize == sizeof(template_blob));
    assert(out.TemplateBlob && memcmp(out.TemplateBlob, template_blob, sizeof(template_blob)) == 0);
    assert(out.PayloadBlobSize == sizeof(payload_blob));
    assert(out.PayloadBlob && memcmp(out.PayloadBlob, payload_blob, sizeof(payload_blob)) == 0);
    assert(out.IndexElementCount == 0);
    assert(tudor_storage_adapter->NextRecord(&pipeline) == WINBIO_E_DATABASE_NO_MORE_RECORDS);

    //A different GUID must not match
    WINBIO_IDENTITY other = identity;
    other.TemplateGuid.PartA++;
    assert(tudor_storage_adapter->QueryBySubject(&pipeline, &other, WINBIO_SUBTYPE_ANY) == WINBIO_E_DATABASE_NO_RESULTS);
}

static void check_enumeration_helpers(void) {
    struct tudor_stored_record records[4];

    int count = tudor_storage_get_records(&device, records, 4);
    assert(count == 1);
    assert(records[0].guid.PartA == 0xdeadbeef);
    assert(records[0].finger == TUDOR_FINGER_RH_INDEX_FINGER);
    assert(records[0].template_size == 16);
    assert(records[0].payload_size == 32);

    //Filtered deletion of a non-matching identity must fail cleanly
    WINBIO_IDENTITY other = make_guid_identity();
    other.TemplateGuid.PartA++;
    RECGUID other_guid = *(RECGUID*) &other.TemplateGuid;
    assert(tudor_storage_delete_records(&device, &other_guid, TUDOR_FINGER_ANY) == false);

    //Matching deletion must succeed and release the record
    WINBIO_IDENTITY identity = make_guid_identity();
    RECGUID guid = *(RECGUID*) &identity.TemplateGuid;
    assert(tudor_storage_delete_records(&device, &guid, TUDOR_FINGER_RH_INDEX_FINGER) == true);
    assert(device.records_head == NULL);
    assert(tudor_storage_adapter->QueryBySubject(&pipeline, &identity, WINBIO_SUBTYPE_ANY) == WINBIO_E_DATABASE_NO_RESULTS);
    assert(tudor_storage_get_records(&device, records, 4) == 0);
}

static void check_failure_paths(void) {
    WINBIO_IDENTITY identity = make_guid_identity();
    UCHAR blob[8] = {0};

    //Wrong identity types are not storable
    WINBIO_IDENTITY wildcard = { .Type = WINBIO_ID_TYPE_WILDCARD, .Wildcard = 1 };
    WINBIO_STORAGE_RECORD rec = make_record(&wildcard, 1, blob, sizeof(blob), NULL, 0);
    assert(tudor_storage_adapter->AddRecord(&pipeline, &rec) == E_INVALIDARG);

    WINBIO_IDENTITY secure = identity;
    secure.Type = WINBIO_ID_TYPE_SECURE_ID;
    rec = make_record(&secure, 1, blob, sizeof(blob), NULL, 0);
    assert(tudor_storage_adapter->AddRecord(&pipeline, &rec) == E_INVALIDARG);

    //Declared sizes without pointers are refused instead of dereferenced
    rec = make_record(&identity, 1, blob, sizeof(blob), NULL, sizeof(blob));
    assert(tudor_storage_adapter->AddRecord(&pipeline, &rec) == E_INVALIDARG);

    rec = make_record(&identity, 1, NULL, sizeof(blob), NULL, 0);
    assert(tudor_storage_adapter->AddRecord(&pipeline, &rec) == E_INVALIDARG);

    rec = make_record(&identity, 1, blob, sizeof(blob), NULL, 0);
    rec.IndexVector = NULL;
    rec.IndexElementCount = 2;
    assert(tudor_storage_adapter->AddRecord(&pipeline, &rec) == E_INVALIDARG);

    //Nothing was stored by the failed attempts
    assert(device.records_head == NULL);
    assert(tudor_storage_adapter->QueryBySubject(&pipeline, &identity, WINBIO_SUBTYPE_ANY) == WINBIO_E_DATABASE_NO_RESULTS);
    assert(tudor_storage_adapter->FirstRecord(&pipeline) == WINBIO_E_DATABASE_NO_RESULTS);
    assert(tudor_storage_adapter->GetCurrentRecord(&pipeline, &(WINBIO_STORAGE_RECORD){0}) == WINBIO_E_DATABASE_NO_RESULTS);
    assert(tudor_storage_adapter->NextRecord(&pipeline) == WINBIO_E_DATABASE_NO_RESULTS);

    //Deleting something that isn't there reports the documented error
    assert(tudor_storage_adapter->DeleteRecord(&pipeline, &identity, 1) == WINBIO_E_DATABASE_NO_SUCH_RECORD);
}

static void check_zero_size_template(void) {
    //Match-in-sensor engines commit identity-only records: no template blob at all
    WINBIO_IDENTITY identity = make_guid_identity();
    identity.TemplateGuid.PartA = 7;
    WINBIO_STORAGE_RECORD rec = make_record(&identity, TUDOR_FINGER_LH_THUMB, NULL, 0, NULL, 0);
    assert(tudor_storage_adapter->AddRecord(&pipeline, &rec) == ERROR_SUCCESS);

    assert(tudor_storage_adapter->QueryBySubject(&pipeline, &identity, TUDOR_FINGER_LH_THUMB) == ERROR_SUCCESS);
    assert(tudor_storage_adapter->FirstRecord(&pipeline) == ERROR_SUCCESS);
    WINBIO_STORAGE_RECORD out = {0};
    assert(tudor_storage_adapter->GetCurrentRecord(&pipeline, &out) == ERROR_SUCCESS);
    assert(out.TemplateBlob == NULL && out.TemplateBlobSize == 0);
    assert(out.PayloadBlob == NULL && out.PayloadBlobSize == 0);

    //Wildcard delete removes the record and frees it
    WINBIO_IDENTITY wildcard = { .Type = WINBIO_ID_TYPE_WILDCARD, .Wildcard = 1 };
    assert(tudor_storage_adapter->DeleteRecord(&pipeline, &wildcard, WINBIO_SUBTYPE_ANY) == ERROR_SUCCESS);
    assert(device.records_head == NULL);
}

static void check_any_subfactor_resolution(void) {
    //WinBioVerify resolves WINBIO_SUBTYPE_ANY inside the framework, before an engine
    //adapter sees it: the records for the identity are enumerated and the engine is
    //called with each concrete subfactor. Handing the wildcard to the vendor engine
    //makes its on-chip lookup match nothing, so every capture is rejected as a bad one.
    WINBIO_IDENTITY identity = make_guid_identity();
    UCHAR blob[8] = {0};
    WINBIO_STORAGE_RECORD rec = make_record(&identity, TUDOR_FINGER_LH_RING_FINGER, blob, sizeof(blob), NULL, 0);
    assert(tudor_storage_adapter->AddRecord(&pipeline, &rec) == ERROR_SUCCESS);

    RECGUID guid = *(RECGUID*) &identity.TemplateGuid;
    assert(tudor_resolve_finger(&device, guid, TUDOR_FINGER_ANY) == TUDOR_FINGER_LH_RING_FINGER);

    //A concrete subfactor is passed through untouched, enrolled or not
    assert(tudor_resolve_finger(&device, guid, TUDOR_FINGER_RH_INDEX_FINGER) == TUDOR_FINGER_RH_INDEX_FINGER);

    //An identity without records keeps the wildcard; the engine, not the loader, decides
    WINBIO_IDENTITY unknown = identity;
    unknown.TemplateGuid.PartA++;
    RECGUID unknown_guid = *(RECGUID*) &unknown.TemplateGuid;
    assert(tudor_resolve_finger(&device, unknown_guid, TUDOR_FINGER_ANY) == TUDOR_FINGER_ANY);

    WINBIO_IDENTITY wildcard = { .Type = WINBIO_ID_TYPE_WILDCARD, .Wildcard = 1 };
    assert(tudor_storage_adapter->DeleteRecord(&pipeline, &wildcard, WINBIO_SUBTYPE_ANY) == ERROR_SUCCESS);
    assert(device.records_head == NULL);
}

static void check_identity_helpers_zero_the_union(void) {
    //The vendor adapters marshal sizeof(WINBIO_IDENTITY) bytes to the driver, including
    //the SID/secure-id part of the union. Building the identity with a compound literal
    //leaves that part uninitialized, so stack addresses travel with every commit, query
    //and verify request and end up in the on-sensor record and its payload tags.
    RECGUID guid = {
        .PartA = 0x11223344, .PartB = 0x5566, .PartC = 0x7788,
        .PartD = 0x99aa, .PartE = 0xbbccddeeff00
    };
    WINBIO_IDENTITY ident;
    memset(&ident, 0xcd, sizeof(ident));
    ident = tudor_guid_identity(guid);
    assert(ident.Type == WINBIO_ID_TYPE_GUID);
    assert(memcmp(&ident.TemplateGuid, &guid, sizeof(GUID)) == 0);
    for(size_t i = 20; i < sizeof(ident); i++) assert(((const uint8_t*) &ident)[i] == 0);

    WINBIO_IDENTITY wildcard;
    memset(&wildcard, 0xcd, sizeof(wildcard));
    wildcard = tudor_wildcard_identity();
    assert(wildcard.Type == WINBIO_ID_TYPE_WILDCARD);
    for(size_t i = 4; i < sizeof(wildcard); i++) assert(((const uint8_t*) &wildcard)[i] == 0);
}

int main(int argc, char **argv) {
    (void) argc; (void) argv;

    LOG_LEVEL = LOG_ERROR;

    assert(pthread_mutex_init(&device.records_lock, NULL) == 0);
    memset(&pipeline, 0, sizeof(pipeline));
    pipeline.StorageInterface = tudor_storage_adapter;
    pipeline.StorageContext = &device;
    device.pipeline = &pipeline;

    assert(tudor_storage_adapter->Attach(&pipeline) == ERROR_SUCCESS);
    assert(tudor_storage_adapter->PipelineInit(&pipeline) == ERROR_SUCCESS);

    check_identity_layout();
    check_record_roundtrip();
    check_enumeration_helpers();
    check_failure_paths();
    check_zero_size_template();
    check_any_subfactor_resolution();
    check_identity_helpers_zero_the_union();

    assert(tudor_storage_adapter->PipelineCleanup(&pipeline) == ERROR_SUCCESS);
    assert(tudor_storage_adapter->Detach(&pipeline) == ERROR_SUCCESS);
    assert(device.records_head == NULL);
    pthread_mutex_destroy(&device.records_lock);

    puts("[TEST] identity/storage contract checks passed");
    return 0;
}

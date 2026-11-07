#include <unistd.h>
#include "internal.h"

//DatabaseId of the on-sensor enrollment database. This is the constant the vendor
//storage adapter uses for the secure DB2 store (the same value the WBF service passes
//from WINBIO_UNIT_SCHEMA); OpenDatabase requires it and both string arguments to be set.
static GUID sensor_db_id = DEFINE_GUID(ae144bee, 3f99, 4711, b16c, e0883a3d2f6c);

WINBIO_IDENTITY tudor_guid_identity(RECGUID guid) {
    //The vendor adapters marshal sizeof(WINBIO_IDENTITY) bytes to the driver, not just
    //the type and the GUID: the SID/secure-id part of the identity union travels with
    //every 0x442018/0x442030/0x442058 request. A compound literal only initializes the
    //GUID member, so the rest of the union keeps whatever was on the stack - addresses,
    //that is - and those bytes end up in the on-sensor record and in the tags the driver
    //derives from it. Zero the whole identity before it leaves the process.
    WINBIO_IDENTITY ident;
    memset(&ident, 0, sizeof(ident));
    ident.Type = WINBIO_ID_TYPE_GUID;
    memcpy(&ident.TemplateGuid, &guid, sizeof(GUID));
    return ident;
}

WINBIO_IDENTITY tudor_wildcard_identity(void) {
    //Same reasoning for the wildcard query: only Type is meaningful, everything else
    //must be zero rather than leftover stack bytes.
    WINBIO_IDENTITY ident;
    memset(&ident, 0, sizeof(ident));
    ident.Type = WINBIO_ID_TYPE_WILDCARD;
    ident.Null = 0;
    return ident;
}

static void req_cb(struct winwdf_request *req, NTSTATUS status, OVERLAPPED *ovlp) {
    //Get request info
    const void *out_buf = NULL;
    size_t out_buf_size, num_transfered = 0;
    if(status == STATUS_SUCCESS) {
        if(!winwdf_get_request_info(req, NULL, NULL, NULL, &out_buf, &out_buf_size, &num_transfered)) {
            log_error("Couldn't get request info!");
            abort();
        }
        if(num_transfered > out_buf_size) num_transfered = out_buf_size;
    }

    if(LOG_LEVEL <= LOG_VERBOSE) {
        cant_fail_ret(pthread_mutex_lock(&LOG_LOCK));
        printf("[DEVCTRL] <- status 0x%x out (size 0x%lx): ", status, num_transfered);
        if(status == STATUS_SUCCESS) for(size_t i = 0; i < num_transfered; i++) printf("%02x", ((uint8_t*) out_buf)[i]);
        puts("");
        cant_fail_ret(pthread_mutex_unlock(&LOG_LOCK));
    }

    //Complete the OVERLAPPED
    winio_complete_overlapped(ovlp, status, num_transfered);
}

static NTSTATUS tudor_devctrl(struct tudor_device *device, OVERLAPPED *ovlp, ULONG code, void *in_buf, size_t in_size, void *out_buf, size_t out_size, struct winwdf_request **req) {
    if(LOG_LEVEL <= LOG_VERBOSE) {
        cant_fail_ret(pthread_mutex_lock(&LOG_LOCK));
        printf("[DEVCTRL] -> in code 0x%x (size 0x%lx): ", code, in_size);
        for(size_t i = 0; i < in_size; i++) printf("%02x", ((uint8_t*) in_buf)[i]);
        puts("");
        cant_fail_ret(pthread_mutex_unlock(&LOG_LOCK));
    }

    //Start the request
    struct winmodule *mod = winmodule_get_cur();
    winmodule_set_cur(&tudor_driver_dll->module);
    NTSTATUS status = winwdf_devctrl_file(device->wdf_file, code, in_buf, in_size, out_buf, out_size, req);
    winmodule_set_cur(mod);

    //Add callback
    if(status == STATUS_SUCCESS) winwdf_add_request_callback(*req, (winwdf_request_cb_fnc*) req_cb, ovlp);

    return status;
}

static NTSTATUS tudor_cancel(struct tudor_device *device, OVERLAPPED *ovlp, struct winwdf_request *req) {
    winwdf_cancel_request(req);
    return STATUS_SUCCESS;
}

static void tudor_cleanup(struct tudor_device *device, OVERLAPPED *ovlp, struct winwdf_request *req) {
    winwdf_destroy_object((WDFOBJECT) req);
}

bool tudor_open(struct tudor_device *device, libusb_device_handle *usb_dev, struct tudor_device_state *state) {
    HRESULT hres;
    NTSTATUS status;

    device->state = state ? *state : (struct tudor_device_state) {0};
    device->enrolling = false;
    cant_fail_ret(pthread_mutex_init(&device->records_lock, NULL));
    device->records_head = NULL;
    device->result_records_head = device->result_records_cursor = NULL;

    //Reset the USB device
    int usb_err;
    if((usb_err = libusb_reset_device(usb_dev)) != 0) {
        log_error("libusb_reset_device failed: %d [%s]", usb_err, libusb_error_name(usb_err));
        return false;
    }

    //Open the device through the driver
    device->reg_key = winreg_open_key(device, "HKEY_LOCAL_MACHINE\\Tudor\\Device");
    if((status = winwdf_add_device(tudor_wdf_driver, device->reg_key, usb_dev, &device->wdf_device)) != 0) {
        log_error("Error adding WDF device: 0x%x!", status);
        return false;
    }
    if(!device->wdf_device) {
        log_error("Driver didn't create a WDF device!");
        return false;
    }
    winwdf_event_queue_flush();

    if((status = winwdf_open_device(device->wdf_device, &device->wdf_file)) != 0) {
        log_error("Error opening WDF file: 0x%x!", status);
        return false;
    }

    //This is dumb, but otherwise we run into race conditions
    cant_fail(usleep(3000000));

    //Initialize the pipeline
    winmodule_set_cur(&tudor_adapter_dll->module);

    //Pick the storage backend. The vendor adapter owns the sensor's enrollment
    //database (match-in-sensor devices); the host adapter is the legacy in-memory
    //list that upstream used for engines which store templates on the host.
    //TUDOR_HOST_STORAGE=1 keeps the legacy backend for debugging.
    const char *host_storage_env = getenv("TUDOR_HOST_STORAGE");
    bool force_host_storage = host_storage_env && *host_storage_env && strcmp(host_storage_env, "0") != 0;
    WINBIO_STORAGE_INTERFACE *storage_adapter = (!force_host_storage && tudor_vendor_storage_adapter) ? tudor_vendor_storage_adapter : tudor_storage_adapter;
    device->sensor_storage = storage_adapter == tudor_vendor_storage_adapter;

    log_debug("Initializing WINBIO pipeline...");
    device->pipeline = (WINBIO_PIPELINE*) malloc(sizeof(WINBIO_PIPELINE));
    if(!device->pipeline) { perror("Error allocating WINBIO pipeline"); abort(); }
    *device->pipeline = (WINBIO_PIPELINE) {0};
    device->pipeline->EngineInterface = tudor_engine_adapter;
    device->pipeline->SensorInterface = tudor_sensor_adapter;
    device->pipeline->StorageInterface = storage_adapter;
    device->pipeline->SensorHandle = device->winbio_file = winio_create_file(device, true, NULL, NULL, (winio_devctrl_fnc*) tudor_devctrl, (winio_cancel_fnc*) tudor_cancel, (winio_cleanup_fnc*) tudor_cleanup, NULL);
    device->pipeline->EngineHandle = INVALID_HANDLE_VALUE;
    device->pipeline->StorageHandle = INVALID_HANDLE_VALUE;
    //The vendor adapter allocates its own context in Attach and rejects a pipeline
    //that arrives with StorageContext set; the host adapter keeps our device there.
    device->pipeline->StorageContext = device->sensor_storage ? NULL : device;

    log_info("Storage backend: %s", device->sensor_storage ? "vendor (sensor database)" : "host (in-memory record list)");

    log_debug("Attaching interfaces to pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Attach, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Attach, device->pipeline);
    WINBIO_CALL_PIPELINE(storage_adapter->Attach, device->pipeline);

    log_debug("Initializing pipeline interfaces...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->PipelineInit, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->PipelineInit, device->pipeline);
    WINBIO_CALL_PIPELINE(storage_adapter->PipelineInit, device->pipeline);

    //Reset the sensor
    log_debug("Resetting sensor...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Reset, device->pipeline);

    //Activate the pipeline
    log_debug("Activating pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Activate, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Activate, device->pipeline);
    WINBIO_CALL_PIPELINE(storage_adapter->Activate, device->pipeline);

    //Open the enrollment database. For the vendor adapter this is the on-sensor
    //database identified by the WINBIO_UNIT_SCHEMA DatabaseId the WBF service
    //would pass; without it the adapter refuses every record operation.
    if(device->sensor_storage) {
        log_debug("Opening sensor enrollment database...");
        if((hres = storage_adapter->OpenDatabase(device->pipeline, &sensor_db_id, u"", u"")) != ERROR_SUCCESS) {
            log_error("Error opening the sensor enrollment database: 0x%x! (set TUDOR_HOST_STORAGE=1 to fall back to the host record list)", hres);
            return false;
        }
    }

    //Check the sensor status
    log_debug("Checking sensor status...");
    ULONG sensor_status = WINBIO_SENSOR_FAILURE;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->QueryStatus, device->pipeline, &sensor_status)
    if(sensor_status != WINBIO_SENSOR_READY) {
        log_error("Sensor didn't return ready status! [status 0x%x]", status);
        return false;
    }

    return true;
}

bool tudor_close(struct tudor_device *device) {
    HRESULT hres;

    winmodule_set_cur(&tudor_adapter_dll->module);

    WINBIO_STORAGE_INTERFACE *storage_adapter = device->pipeline->StorageInterface;

    //Close the enrollment database (vendor adapter only; the host adapter has none)
    if(device->sensor_storage && !device->storage_erased) {
        log_debug("Closing sensor enrollment database...");
        if((hres = storage_adapter->CloseDatabase(device->pipeline)) != ERROR_SUCCESS)
            log_warn("Error closing the sensor enrollment database: 0x%x", hres);
    } else if(device->storage_erased) log_debug("Storage database was erased; it has no session left to close");

    //Deactivate the pipeline
    log_debug("Deactivating pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Deactivate, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Deactivate, device->pipeline);
    WINBIO_CALL_PIPELINE(storage_adapter->Deactivate, device->pipeline);

    //Uninitialize the pipeline
    log_debug("Uninitializing pipeline interfaces...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->PipelineCleanup, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->PipelineCleanup, device->pipeline);
    WINBIO_CALL_PIPELINE(storage_adapter->PipelineCleanup, device->pipeline);

    log_debug("Detaching interfaces from pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Detach, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Detach, device->pipeline);
    WINBIO_CALL_PIPELINE(storage_adapter->Detach, device->pipeline);
    free(device->pipeline);

    winhandle_destroy(device->winbio_file);

    //Close the WDF file
    winmodule_set_cur(&tudor_driver_dll->module);
    winwdf_close_file(device->wdf_file);

    //Remove the device
    winmodule_set_cur(&tudor_driver_dll->module);
    winwdf_remove_device(device->wdf_device);
    winwdf_event_queue_flush();
    winhandle_destroy(device->reg_key);

    //Free records
    cant_fail_ret(pthread_mutex_lock(&device->records_lock));
    for(struct tudor_record *rec = device->records_head, *nrec = rec ? rec->next : NULL; rec; rec = nrec, nrec = rec ? rec->next : NULL) {
        free(rec->data);
        free(rec->payload);
        free(rec->index_vector);
        free(rec->identity);
        free(rec);
    }
    cant_fail_ret(pthread_mutex_unlock(&device->records_lock));
    cant_fail_ret(pthread_mutex_destroy(&device->records_lock));

    return true;
}

bool tudor_enroll_start(struct tudor_device *device, RECGUID guid, enum tudor_finger finger) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;

    if(device->enrolling) {
        log_error("Already enrolling a finger!");
        return false;
    }

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollBegin
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_storage_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->CreateEnrollment, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->SetEnrollmentParameters, device->pipeline, &(WINBIO_EXTENDED_ENROLLMENT_PARAMETERS) {
        .Size = sizeof(WINBIO_EXTENDED_ENROLLMENT_PARAMETERS),
        .SubFactor = (UCHAR) finger
    });

    device->enrolling = true;
    device->enroll_guid = guid;
    device->enroll_finger = finger;
    return true;
}

static void enroll_cb(OVERLAPPED *ovlp, NTSTATUS status, tudor_async_res_t res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;
    bool success = false, done = true;

    if(status != STATUS_SUCCESS) {
        log_error("Error starting capture: 0x%x!", status);
        goto exit;
    }

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollCapture
    ULONG reject_detail;
    if((hres = tudor_sensor_adapter->FinishCapture(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) done = false;
        log_error("Error finishing sensor capture: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if((hres = tudor_sensor_adapter->PushDataToEngine(res->dev->pipeline, WINBIO_PURPOSE_ENROLL, 0, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) done = false;
        log_error("Error pushing sensor data to engine: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    log_debug("Updating enrollment...");
    if((hres = tudor_engine_adapter->UpdateEnrollment(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS && hres != WINBIO_I_MORE_DATA) {
        if(hres == WINBIO_E_BAD_CAPTURE) done = false;
        log_error("Error updating enrollment: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    success = true;
    done = hres != WINBIO_I_MORE_DATA;

    exit:;
    *res->args.enroll.done = done;
    async_complete_op(res, success);
}

bool tudor_enroll_capture(struct tudor_device *device, bool *done, tudor_async_res_t *res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    *res = NULL;
    HRESULT hres;

    *done = true;
    if(!device->enrolling) {
        log_error("Not currently enrolling a finger!");
        return false;
    }

    log_debug("Capturing sample...");
    OVERLAPPED *ovlp;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->StartCapture, device->pipeline, WINBIO_PURPOSE_ENROLL, &ovlp);

    *res = async_new_res(device, ovlp);
    (*res)->args.enroll = (struct async_args_enroll) { .done = done };
    winio_set_overlapped_callback(ovlp, (winio_overlapped_cb_fnc*) enroll_cb, *res, true);
    return true;
}

bool tudor_enroll_commit(struct tudor_device *device, bool *is_duplicate) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;

    *is_duplicate = false;
    if(!device->enrolling) {
        log_error("Not currently enrolling a finger!");
        return false;
    }

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollCommit
    log_debug("Checking for duplicate enrollment...");
    BOOLEAN is_dupl;
    WINBIO_IDENTITY dupl_ident;
    UCHAR dupl_finger;
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->CheckForDuplicate, device->pipeline, &dupl_ident, &dupl_finger, &is_dupl);
    if(is_dupl) {
        *is_duplicate = true;
        return false;
    }

    log_debug("Committing enrollment...");

    //WinBioEnrollCommit obtains the enrollment hash from the engine and hands it to
    //CommitEnrollment as the record payload. Match-in-sensor engines store that payload
    //next to the template in the sensor database, so it is what links the host identity
    //to the enrollment; an empty payload leaves the enrollment unusable for verify.
    //The hash buffer belongs to the engine context (it is handed out, not
    //allocated for the caller) and stays valid until the context is cleared by
    //CommitEnrollment/DiscardEnrollment, so it must not be freed here.
    UCHAR *payload = NULL;
    SIZE_T payload_size = 0;
    if((hres = tudor_engine_adapter->GetEnrollmentHash(device->pipeline, &payload, &payload_size)) != ERROR_SUCCESS) {
        log_warn("Engine didn't produce an enrollment hash (0x%x); committing without a payload", hres);
        payload = NULL;
        payload_size = 0;
    }

    WINBIO_IDENTITY ident = tudor_guid_identity(device->enroll_guid);
    hres = tudor_engine_adapter->CommitEnrollment(device->pipeline, &ident, (UCHAR) device->enroll_finger, payload, payload_size);

    if(hres != ERROR_SUCCESS) {
        log_error("Error commiting enrollment: 0x%x!", hres);
        if(hres == WINBIO_E_DUPLICATE_ENROLLMENT) *is_duplicate = true;
        return false;
    }

    log_info("Committed enrollment of guid=%08x... finger=%x with %lu byte payload to %s storage",
        device->enroll_guid.PartA, device->enroll_finger, (unsigned long) payload_size,
        device->sensor_storage ? "sensor" : "host");
    device->enrolling = false;
    return true;
}

bool tudor_enroll_discard(struct tudor_device *device) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;

    if(!device->enrolling) {
        log_error("Not currently enrolling a finger!");
        return false;
    }

    log_info("Discarding enrollment of guid=%08x... finger=%x", device->enroll_guid.PartA, device->enroll_finger);

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollDiscard
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->DiscardEnrollment, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->ClearContext, device->pipeline);

    device->enrolling = false;
    return true;
}

static void verify_cb(OVERLAPPED *ovlp, NTSTATUS status, tudor_async_res_t res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;
    bool success = false, retry = false, matches = false;

    if(status != STATUS_SUCCESS) {
        log_error("Error starting capture: 0x%x!", status);
        goto exit;
    }

    ULONG reject_detail;
    if((hres = tudor_sensor_adapter->FinishCapture(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error finishing sensor capture: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if((hres = tudor_sensor_adapter->PushDataToEngine(res->dev->pipeline, WINBIO_PURPOSE_VERIFY, 0, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error pushing sensor data to engine: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    log_debug("Verifying sample...");
    BOOLEAN is_match;
    UCHAR *payload_ptr, *hash_ptr;
    SIZE_T payload_size, hash_size;
    WINBIO_IDENTITY ident = tudor_guid_identity(res->args.verify.guid);
    if((hres = tudor_engine_adapter->VerifyFeatureSet(res->dev->pipeline, &ident, (UCHAR) res->args.verify.finger, &is_match, &payload_ptr, &payload_size, &hash_ptr, &hash_size, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        if(hres == WINBIO_E_NO_MATCH) {
            success = true;
            matches = false;
            goto exit;
        }

        log_error("Error verifying sample: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    success = true;
    matches = is_match;

    exit:;
    *(res->args.verify.retry) = retry;
    *(res->args.verify.matches) = matches;
    async_complete_op(res, success);
}

static bool guid_equal(RECGUID a, RECGUID b) {
    return a.PartA == b.PartA && a.PartB == b.PartB && a.PartC == b.PartC
        && a.PartD == b.PartD && a.PartE == b.PartE;
}

enum tudor_finger tudor_resolve_finger(struct tudor_device *device, RECGUID guid, enum tudor_finger finger) {
    //WinBioVerify resolves WINBIO_SUBTYPE_ANY inside the framework: it enumerates the
    //storage records for the identity and calls the engine once per concrete subfactor.
    //The engine adapter never receives the wildcard. The vendor engine turns the passed
    //subfactor into its on-chip lookup key, so a wildcard matches no enrollment at all and
    //every capture comes back as WINBIO_E_BAD_CAPTURE, which the caller treats as an
    //unreadable sample and retries forever. Resolve it here instead.
    if(finger != TUDOR_FINGER_ANY) return finger;

    struct tudor_stored_record records[32];
    int num_records = tudor_storage_get_records(device, records, 32);
    if(num_records < 0) {
        log_warn("Couldn't enumerate the storage records; keeping the wildcard subfactor");
        return finger;
    }

    for(int i = 0; i < num_records && i < 32; i++) {
        if(!guid_equal(records[i].guid, guid)) continue;
        log_info("Resolved wildcard subfactor to finger %d for identity %08x...", records[i].finger, guid.PartA);
        return records[i].finger;
    }

    log_warn("No stored enrollment for identity %08x...; the engine may reject the wildcard subfactor", guid.PartA);
    return finger;
}

bool tudor_verify(struct tudor_device *device, RECGUID guid, enum tudor_finger finger, bool *retry, bool *matches, tudor_async_res_t *res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    *res = NULL;
    HRESULT hres;

    *retry = false;
    *matches = false;
    if(device->enrolling) {
        log_error("Currently enrolling a finger!");
        return false;
    }

    finger = tudor_resolve_finger(device, guid, finger);

    log_debug("Capturing sample...");
    OVERLAPPED *ovlp;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->StartCapture, device->pipeline, WINBIO_PURPOSE_VERIFY, &ovlp);

    *res = async_new_res(device, ovlp);
    (*res)->args.verify = (struct async_args_verify) { .retry = retry, .guid = guid, .finger = finger, .matches = matches };
    winio_set_overlapped_callback(ovlp, (winio_overlapped_cb_fnc*) verify_cb, *res, true);
    return true;
}

static void identify_cb(OVERLAPPED *ovlp, NTSTATUS status, tudor_async_res_t res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;
    bool success = false, retry = false, found_match = false;

    if(status != STATUS_SUCCESS) {
        log_error("Error starting capture: 0x%x!", status);
        goto exit;
    }

    ULONG reject_detail;
    if((hres = tudor_sensor_adapter->FinishCapture(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error finishing sensor capture: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if((hres = tudor_sensor_adapter->PushDataToEngine(res->dev->pipeline, WINBIO_PURPOSE_IDENTIFY, 0, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error pushing sensor data to engine: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    log_debug("Identifying sample...");
    WINBIO_IDENTITY identity;
    UCHAR subfactor;
    UCHAR *payload_ptr, *hash_ptr;
    SIZE_T payload_size, hash_size;
    if((hres = tudor_engine_adapter->IdentifyFeatureSet(res->dev->pipeline, &identity, &subfactor, &payload_ptr, &payload_size, &hash_ptr, &hash_size, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        if(hres == WINBIO_E_UNKNOWN_ID) {
            success = true;
            found_match = false;
            goto exit;
        }

        log_error("Error identifying sample: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if(identity.Type != WINBIO_ID_TYPE_GUID) {
        log_error("Invalid identification ID type: %d!", identity.Type);
        goto exit;
    }

    success = true;
    found_match = true;
    *(res->args.identify.guid) = *(RECGUID*) &identity.TemplateGuid;
    *(res->args.identify.finger) = (enum tudor_finger) subfactor;

    exit:;
    *(res->args.identify.retry) = retry;
    *(res->args.identify.found_match) = found_match;
    async_complete_op(res, success);
}

bool tudor_identify(struct tudor_device *device, bool *retry, bool *found_match, RECGUID *guid, enum tudor_finger *finger, tudor_async_res_t *res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    *res = NULL;
    HRESULT hres;

    *retry = false;
    *found_match = false;
    if(device->enrolling) {
        log_error("Currently enrolling a finger!");
        return false;
    }

    log_debug("Capturing sample...");
    OVERLAPPED *ovlp;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->StartCapture, device->pipeline, WINBIO_PURPOSE_IDENTIFY, &ovlp);

    *res = async_new_res(device, ovlp);
    (*res)->args.identify = (struct async_args_identify) { .retry = retry, .found_match = found_match, .guid = guid, .finger = finger };
    winio_set_overlapped_callback(ovlp, (winio_overlapped_cb_fnc*) identify_cb, *res, true);
    return true;
}

int tudor_storage_get_records(struct tudor_device *device, struct tudor_stored_record *records, int max_records) {
    WINBIO_STORAGE_INTERFACE *storage = device->pipeline->StorageInterface;
    if(!storage || !storage->QueryByContent || !storage->FirstRecord || !storage->NextRecord || !storage->GetCurrentRecord) {
        log_error("Storage adapter doesn't support record enumeration!");
        return -1;
    }
    if(!records || max_records < 1) {
        log_error("No buffer for storage records!");
        return -1;
    }

    //Query every record. The vendor adapter requires a non-NULL index vector even
    //for an empty one (it rejects NULL with E_POINTER and any count with
    //WINBIO_E_DATABASE_BAD_INDEX_VECTOR).
    ULONG empty_index_vector[1] = {0};
    HRESULT hres = storage->QueryByContent(device->pipeline, WINBIO_SUBTYPE_ANY, empty_index_vector, 0);
    if(hres == WINBIO_E_DATABASE_NO_RESULTS) return 0;
    if(hres != ERROR_SUCCESS) {
        log_error("Error querying storage records: 0x%x!", hres);
        return -1;
    }

    int num_records = 0;
    hres = storage->FirstRecord(device->pipeline);
    while(hres == ERROR_SUCCESS) {
        WINBIO_STORAGE_RECORD srec = {0};
        if(storage->GetCurrentRecord(device->pipeline, &srec) == ERROR_SUCCESS && srec.Identity) {
            if(num_records < max_records) {
                records[num_records] = (struct tudor_stored_record) {
                    .guid = *(RECGUID*) &srec.Identity->TemplateGuid,
                    .finger = (enum tudor_finger) srec.SubFactor,
                    .template_size = srec.TemplateBlobSize,
                    .payload_size = srec.PayloadBlobSize
                };
            }
            num_records++;
        }
        hres = storage->NextRecord(device->pipeline);
    }

    return num_records;
}

bool tudor_storage_erase_database(struct tudor_device *device) {
    WINBIO_STORAGE_INTERFACE *storage = device->pipeline->StorageInterface;
    if(!storage || !storage->EraseDatabase) {
        log_error("Storage adapter doesn't support erasing the database!");
        return false;
    }

    //EraseDatabase is the only database-wide clear the adapter interface exposes:
    //it replaces the whole database instead of deleting records one by one, so it
    //also covers entries the query API never returns (e.g. an enrollment whose
    //payload/template association is broken). It takes the same database id and
    //connection arguments as OpenDatabase.
    HRESULT hres = storage->EraseDatabase(device->pipeline, &sensor_db_id, u"", u"");
    if(hres != ERROR_SUCCESS) {
        log_error("Error erasing %s storage database: 0x%x!", device->sensor_storage ? "sensor" : "host", hres);
        return false;
    }

    //The adapter tears the database session down as part of the erase, so the
    //handle cannot be closed afterwards. Try to open a fresh session right away:
    //when that works the device stays usable after a clear, which is what
    //libfprint's clear_storage needs. When it fails, storage_erased stays set and
    //the caller has to restart the driver to get a usable (empty) database back.
    device->storage_erased = true;
    log_warn("Erased the %s storage database", device->sensor_storage ? "sensor" : "host");

    if(storage->OpenDatabase && storage->OpenDatabase(device->pipeline, &sensor_db_id, u"", u"") == ERROR_SUCCESS) {
        device->storage_erased = false;
        log_info("Reopened the storage database after erasing it");
    } else {
        log_warn("Could not reopen the storage database; the driver must be restarted to use it again");
    }

    return true;
}

int tudor_storage_count_matching_records(struct tudor_device *device, RECGUID *guid, enum tudor_finger finger) {
    struct tudor_stored_record records[128];
    int num_records = tudor_storage_get_records(device, records, 128);
    if(num_records < 0) return -1;
    if(num_records > 128) num_records = 128;

    int matched = 0;
    for(int i = 0; i < num_records; i++) {
        if(guid && memcmp(&records[i].guid, guid, sizeof(RECGUID)) != 0) continue;
        if(finger != TUDOR_FINGER_ANY && records[i].finger != finger) continue;
        matched++;
    }

    return matched;
}

bool tudor_storage_delete_records(struct tudor_device *device, RECGUID *guid, enum tudor_finger finger) {
    WINBIO_STORAGE_INTERFACE *storage = device->pipeline->StorageInterface;
    if(!storage || !storage->DeleteRecord) {
        log_error("Storage adapter doesn't support record deletion!");
        return false;
    }

    WINBIO_IDENTITY identity = {0};
    if(guid) {
        identity.Type = WINBIO_ID_TYPE_GUID;
        identity.TemplateGuid = *(GUID*) guid;
    } else {
        identity.Type = WINBIO_ID_TYPE_WILDCARD;
        identity.Wildcard = 1;
    }

    HRESULT hres = storage->DeleteRecord(device->pipeline, &identity, (UCHAR) finger);
    if(hres != ERROR_SUCCESS) {
        log_error("Error deleting %s storage record: 0x%x!", device->sensor_storage ? "sensor" : "host", hres);
        return false;
    }

    //The vendor adapter answers ERROR_SUCCESS without removing anything when the
    //template is not linked into its queryable database. Observed on 06cb:00df as
    //"Deleted 0 records with GUID ..." immediately followed by a successful
    //identify of that same GUID. Reporting success there makes fprintd tell the
    //user the fingerprint is gone while it stays enrolled on the sensor, so the
    //only honest answer is to check and fail when the record survived.
    if(device->sensor_storage) {
        int remaining = tudor_storage_count_matching_records(device, guid, finger);
        if(remaining < 0) {
            log_warn("Couldn't verify the sensor record deletion: enumeration failed");
        } else if(remaining > 0) {
            log_error("Storage adapter reported success but %d matching record(s) remain; deletion did not take effect", remaining);
            return false;
        }
    }

    return true;
}

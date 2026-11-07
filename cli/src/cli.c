#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <tudor/log.h>
#include "cli.h"

static bool abort_cmd_loop = false;
static void sigint_handler(int sig) {
    if(abort_cmd_loop) {
        log_warn("Forcefully exitting!");
        exit(EXIT_FAILURE);
    }
    abort_cmd_loop = false;
}

bool cli_capture_retry_pending(int *retries) {
    if(*retries >= CLI_MAX_CAPTURE_RETRIES) return false;
    (*retries)++;
    return true;
}

enum tudor_finger cli_parse_finger(unsigned value) {
    if(value >= TUDOR_FINGER_RH_THUMB && value <= TUDOR_FINGER_LH_LITTLE_FINGER) return (enum tudor_finger) value;
    return TUDOR_FINGER_ANY;
}

void cli_main_loop(struct tudor_device *device) {
    if(getenv("TUDOR_OPEN_ONLY")) {
        log_info("Open-only test succeeded; skipping all post-open sensor commands");
        abort_cmd_loop = true;
        return;
    }

    signal(SIGINT, sigint_handler);

    //Command loop
    while(!abort_cmd_loop) {
        //Read command
        puts("");
        puts("Commands:");
        puts("  e - enroll finger");
        puts("  v - verify finger");
        puts("  i - identify finger");
        puts("  q - query information about enrolled fingers");
        puts("  w - wipe enrolled finger(s)");
        puts("  d - erase the whole sensor database (destructive)");
        puts("  s - shutdown driver");

        printf("> ");
        char cmd = getchar();
        while(!abort_cmd_loop && isspace(cmd)) cmd = getchar();
        if(abort_cmd_loop) break;

        //Execute command
        switch(tolower(cmd)) {
            case 'e': {
                //Read identity GUID
                RECGUID guid = {0};
                printf("Enter identity index: ");
                scanf("%u", &guid.PartA);
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Read finger
                enum tudor_finger finger;
                printf("Enter finger index (1-5 = right hand thumb - little finger | 6-10 = left hand thumb - little finger): ");
                scanf("%d", (int*) &finger);
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;
                if(finger < TUDOR_FINGER_RH_THUMB || TUDOR_FINGER_LH_LITTLE_FINGER < finger) {
                    printf("Invalid finger index!");
                    goto cmdend;
                }

                //Do enrollment
                if(!tudor_enroll_start(device, guid, finger)) {
                    log_error("Error starting enrollment!");
                    goto cmdend;
                }

                puts("Put your finger on the sensor");
                while(true) {
                    //Update enrollment
                    bool is_done;
                    tudor_async_res_t async_res = NULL;
                    if(!tudor_enroll_capture(device, &is_done, &async_res) || !tudor_wait_async(async_res)) {
                        if(async_res) tudor_cleanup_async(async_res);

                        if(!is_done) {
                            log_warn("Retrying enrollment update...");
                            continue;
                        }

                        log_error("Error capturing sample for enrollment!");
                        tudor_enroll_discard(device);
                        goto cmdend;
                    }
                    tudor_cleanup_async(async_res);

                    if(is_done) break;
                    puts("Driver requested more samples");
                }

                //Commit the enrollment
                bool is_dupl;
                if(!tudor_enroll_commit(device, &is_dupl)) {
                    if(is_dupl) {
                        log_warn("Detected duplicate enrollment!");
                        tudor_enroll_discard(device);
                        goto cmdend;
                    }

                    log_error("Error commiting enrollment!");
                    tudor_enroll_discard(device);
                    goto cmdend;
                }

                puts("Successfully enrolled finger");
            } goto cmdend;
            case 'v': {
                //Read identity GUID
                RECGUID guid = {0};
                printf("Enter identity index: ");
                scanf("%u", &guid.PartA);
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Read finger. Passing an exact finger is the only way to ask the
                //engine for a specific subfactor; 0 (or anything invalid) keeps the
                //previous behaviour of accepting any enrolled finger.
                unsigned finger_index = 0;
                printf("Enter finger index (0 = any, 1-5 = right hand thumb-little finger, 6-10 = left hand thumb-little finger): ");
                if(scanf("%u", &finger_index) < 1) finger_index = 0;
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;
                enum tudor_finger finger = cli_parse_finger(finger_index);

                bool matches = false, verified = false;
                int retries = 0;
                while(cli_capture_retry_pending(&retries)) {

                    //Capture and verify sample
                    puts("Put your finger on the sensor");

                    bool retry;
                    tudor_async_res_t async_res = NULL;
                    if(!tudor_verify(device, guid, finger, &retry, &matches, &async_res) || !tudor_wait_async(async_res)) {
                        if(async_res) tudor_cleanup_async(async_res);

                        if(retry) {
                            log_warn("Retrying verify capture (%d/%d)...", retries, CLI_MAX_CAPTURE_RETRIES);
                            continue;
                        }

                        log_error("Error verifying captured sample!");
                        goto cmdend;
                    }
                    tudor_cleanup_async(async_res);
                    verified = true;
                    break;
                }
                if(verified) {
                    puts(matches ? "VERIFICATION SUCCESS" : "VERIFICATION FAILURE");
                } else {
                    log_error("Giving up on verification after %d rejected captures; the engine never accepted a sample (see the 'Error verifying sample' lines above)", retries);
                }
            } goto cmdend;
            case 'i': {
                bool found_match = false, identified = false;
                RECGUID match_guid;
                enum tudor_finger match_finger;

                int retries = 0;
                while(cli_capture_retry_pending(&retries)) {
                    //Capture and identify sample
                    puts("Put your finger on the sensor");
                    bool retry;
                    tudor_async_res_t async_res = NULL;
                    if(!tudor_identify(device, &retry, &found_match, &match_guid, &match_finger, &async_res) || !tudor_wait_async(async_res)) {
                        if(async_res) tudor_cleanup_async(async_res);

                        if(retry) {
                            log_warn("Retrying identify capture (%d/%d)...", retries, CLI_MAX_CAPTURE_RETRIES);
                            continue;
                        }

                        log_error("Error identifying captured sample!");
                        goto cmdend;
                    }
                    tudor_cleanup_async(async_res);
                    identified = true;
                    break;
                }   

                //Print match
                if(identified) {
                    if(found_match) {
                        printf("Found match: identity index=%u finger=%d\n", match_guid.PartA, match_finger);
                    } else puts("Found no match");
                } else {
                    log_error("Giving up on identification after %d rejected captures; the engine never accepted a sample", retries);
                }
            } goto cmdend;
            case 'q': {
                //Read identity GUID
                bool all_guids = false;
                RECGUID guid = {0};
                printf("Enter identity index (invalid number for all): ");
                if(scanf("%u", &guid.PartA) < 1) all_guids = true;
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Print records from whichever storage backend is active
                enum { MAX_LISTED_RECORDS = 64 };
                struct tudor_stored_record records[MAX_LISTED_RECORDS];
                int num_records = tudor_storage_get_records(device, records, MAX_LISTED_RECORDS);
                if(num_records < 0) {
                    log_error("Error enumerating records from %s storage!", device->sensor_storage ? "sensor" : "host");
                    goto cmdend;
                }

                int num_matched = 0;
                for(int i = 0; i < num_records && i < MAX_LISTED_RECORDS; i++) {
                    if(!all_guids && memcmp(&guid, &records[i].guid, sizeof(RECGUID)) != 0) continue;
                    printf("RECORD | identity index=%10u finger=%d template=%lu payload=%lu\n",
                        records[i].guid.PartA, records[i].finger,
                        (unsigned long) records[i].template_size, (unsigned long) records[i].payload_size);
                    num_matched++;
                }
                printf("Total: %d record(s) in %s storage (%d match the filter)\n", num_records, device->sensor_storage ? "sensor" : "host", num_matched);
                if(num_records > MAX_LISTED_RECORDS) printf("(%d record(s) not listed)\n", num_records - MAX_LISTED_RECORDS);
            } goto cmdend;
            case 'd': {
                //Erasing the database removes every enrollment in one go, including
                //records the query API cannot enumerate (a broken/orphaned entry
                //otherwise keeps the on-chip template list unusable). It needs a
                //separate approval because it cannot be undone.
                const char *erase_approval = getenv("TUDOR_DB_ERASE_APPROVED");
                if(!erase_approval || strcmp(erase_approval, "1") != 0) {
                    log_error("Refusing to erase the sensor database: set TUDOR_DB_ERASE_APPROVED=1 to allow it");
                    goto cmdend;
                }
                if(!device->sensor_storage) {
                    log_error("Only the sensor database can be erased; the host store has no erase operation");
                    goto cmdend;
                }

                puts("This deletes every enrollment in the sensor database, including records the query API cannot list.");
                printf("Type ERASE to continue: ");
                char confirmation[16] = {0};
                if(!fgets(confirmation, sizeof(confirmation), stdin) || strncmp(confirmation, "ERASE", 5) != 0) {
                    puts("Aborted database erase");
                    goto cmdend;
                }

                if(tudor_storage_erase_database(device)) puts("Sensor database erased");
            } goto cmdend;
            case 'w': {
                //Read identity GUID
                bool all_guids = false;
                RECGUID guid = {0};
                printf("Enter identity index (invalid number for all): ");
                if(scanf("%u", &guid.PartA) < 1) all_guids = true;
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Read finger
                enum tudor_finger finger;
                printf("Enter finger index (invalid number for all): ");
                if(scanf("%d", (int*) &finger) < 1) finger = TUDOR_FINGER_ANY;
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Deleting sensor records is irreversible and always needs an explicit approval
                const char *delete_approval = getenv("TUDOR_DB_DELETE_APPROVED");
                if(device->sensor_storage && (!delete_approval || strcmp(delete_approval, "1") != 0)) {
                    log_error("Refusing to delete enrollments from the sensor database: set TUDOR_DB_DELETE_APPROVED=1 to allow it");
                    goto cmdend;
                }

                //Ask for confirmation
                printf("Do you really want to wipe all enrolled fingers with identity index=%u finger=%d?\n", guid.PartA, finger);
                printf("y/n: ");
                char yn = getchar();
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(yn != 'y') {
                    puts("Aborted wipe");
                    goto cmdend;
                }

                //Wipe records
                if(device->sensor_storage) {
                    if(tudor_storage_delete_records(device, all_guids ? NULL : &guid, finger)) {
                        puts("Deleted matching enrollment(s) from the sensor database");
                    }
                } else {
                    int num_wiped = tudor_wipe_records(device, all_guids ? NULL: &guid, finger);
                    printf("Succesfully wiped %d enrolled finger(s)\n", num_wiped);
                }
            } goto cmdend;
            case 's': abort_cmd_loop = true; goto cmdend;
            default: printf("Unknown command '%c'!\n", cmd);
        }
        cmdend:;
    }
}
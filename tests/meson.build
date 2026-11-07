offline_test_inc = include_directories('../libtudor/src', '../libtudor/inc')
offline_test_cli_inc = include_directories('../libtudor/src', '../libtudor/inc', '../cli/src')
offline_test_args = ['-D_GNU_SOURCE']

capi_test = executable('capi_test',
    [
        'capi_test.c',
        '../libtudor/src/winapi/crypt/crypt.c',
        '../libtudor/src/winapi/crypt/rsa_aes.c',
        '../libtudor/src/winapi/crypt/evp_digest.c',
        '../libtudor/src/winapi/crypt/hmac.c'
    ],
    dependencies: [libcrypto_dep],
    c_args: offline_test_args,
    link_args: ['-Wl,--wrap=RAND_bytes_ex', '-ldl'],
    include_directories: offline_test_inc
)
test('capi-offline', capi_test, args: [libtudor])

commit_storage_test = executable('commit_storage_test',
    ['commit_storage_test.c'],
    dependencies: [libtudor_dep, thread_dep, libusb_dep],
    c_args: offline_test_args,
    include_directories: offline_test_inc
)
test('commit-storage-offline', commit_storage_test)

identity_storage_test = executable('identity_storage_test',
    ['identity_storage_test.c'],
    dependencies: [libtudor_dep, thread_dep, libusb_dep],
    c_args: offline_test_args,
    include_directories: offline_test_inc
)
test('identity-storage-offline', identity_storage_test)

vendor_storage_test = executable('vendor_storage_test',
    ['vendor_storage_test.c'],
    dependencies: [libtudor_dep, thread_dep, libusb_dep],
    c_args: offline_test_args,
    include_directories: offline_test_inc
)
test('vendor-storage-offline', vendor_storage_test)

cli_retry_test = executable('cli_retry_test',
    ['cli_retry_test.c', '../cli/src/cli.c'],
    dependencies: [libtudor_dep, thread_dep, libusb_dep],
    c_args: offline_test_args,
    include_directories: offline_test_cli_inc
)
test('cli-retry-offline', cli_retry_test)

getlocaltime_test = executable('getlocaltime_test',
    ['getlocaltime_test.c', '../libtudor/src/winapi/time.c'],
    c_args: offline_test_args,
    link_args: ['-Wl,--wrap=gettimeofday', '-ldl'],
    include_directories: offline_test_inc
)
test('getlocaltime-offline', getlocaltime_test, args: [libtudor])

sync_event_test = executable('sync_event_test',
    ['sync_event_test.c'],
    dependencies: [libtudor_dep, thread_dep, libusb_dep],
    c_args: offline_test_args,
    include_directories: offline_test_inc
)
test('sync-event-offline', sync_event_test)

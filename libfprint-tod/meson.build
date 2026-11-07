tudor_tod_src = [
    'src/device.c',
    'src/data.c',
    'src/ipc.c',
    'src/open.c',
    'src/suspend.c',
    'src/enroll.c',
    'src/verify.c',
    'src/identify.c',
    'src/list.c'
]

libfprint_tod_dep = dependency('libfprint-2-tod-1')
udev_dep = dependency('udev')
libusb_dep = dependency('libusb-1.0')
gusb_dep = dependency('gusb')
json_glib_dep = dependency('json-glib-1.0')

tudor_tod = shared_module('tudor_tod',
    sources: tudor_tod_src,
    c_args: ['-D_GNU_SOURCE', '-Wno-missing-braces'],
    dependencies: [libfprint_tod_dep, libusb_dep, gusb_dep, json_glib_dep],
    include_directories: [libtudor_inc, tudor_host_inc, tudor_host_launcher_inc],
    install: true,
    install_dir: libfprint_tod_dep.get_variable(pkgconfig: 'tod_driversdir')
)

install_data('60-tudor-libfprint-tod.rules',
    install_dir: udev_dep.get_variable(pkgconfig: 'udevdir'),
)

#Offline protocol checks: the vfuncs are compiled in but never called, no sensor is opened.
tod_ipc_test = executable('tod_ipc_test',
    ['../tests/tod_ipc_test.c'] + tudor_tod_src,
    c_args: ['-D_GNU_SOURCE', '-Wno-missing-braces'],
    dependencies: [libfprint_tod_dep, dependency('libfprint-2'), libusb_dep, gusb_dep, json_glib_dep],
    include_directories: ['src', libtudor_inc, tudor_host_inc, tudor_host_launcher_inc]
)
test('tod-ipc-offline', tod_ipc_test)

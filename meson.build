project('libfprint-goodixtls', ['c'],
    version: '0.1.0',
    license: 'LGPL-2.1-or-later',
    default_options: [
        'buildtype=debugoptimized',
        'warning_level=1',
        'c_std=c99',
    ],
    meson_version: '>= 0.53.0')

cc = meson.get_compiler('c')

libfprint_tod_dep = dependency('libfprint-2-tod-1')
gmodule_dep = dependency('gmodule-2.0')
udev = dependency('udev')
m_dep = cc.find_library('m', required: false)

# Stock OpenSSL. The sensor's 48-byte PSK is well within OpenSSL's limit, and
# its oversized image record never reaches the library: goodix_tls.c decrypts
# that one itself. No patched crypto library is needed.
openssl_dep = dependency('openssl', version: '>= 3.0')

tod_driversdir = libfprint_tod_dep.get_variable(pkgconfig: 'tod_driversdir')

goodixtls_driver = shared_module('goodixtls-tod-driver',
    sources: [
        'goodixtls.c',
        'config_pcap.c',
        'goodix_sift.c',
        'goodix_tls.c',
    ],
    dependencies: [
        libfprint_tod_dep,
        gmodule_dep,
        openssl_dep,
        m_dep,
    ],
    install: true,
    install_dir: tod_driversdir,
)

# Honour the sysconfdir prefix so an --destdir package install stays inside
# DESTDIR, rather than writing to a hard-coded /etc.
install_data('goodixtls-spidev.conf',
    install_dir: get_option('sysconfdir') / 'modprobe.d')
install_data('60-libfprint-2-goodixtls.rules',
    install_dir: udev.get_variable(pkgconfig: 'udevdir') / 'rules.d',
)

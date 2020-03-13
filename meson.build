# Summary.
project('hound', 'c',
    version: '0.5',
    license: 'apache-2.0',
    default_options: [
        'c_std=c11',
        'buildtype=debug',
        'warning_level=3',
        'werror=true'],
    meson_version: '>= 0.53.0')

cflags = ['-Wshadow', '-fvisibility=internal']
add_project_arguments(cflags, language: 'c')

pkg = import('pkgconfig')

# Includes. Exclude the driver directory from the install, as driver includes
# will be installed only if the associated driver is enabled.
include = include_directories('include')
install_subdir(
    'include/hound',
    install_dir: 'include',
    exclude_directories: 'driver')

# Config header generation.
conf = configuration_data()

# sysconfdir is special, as when the prefix is /usr, it gets set to /etc. So we
# need to treat this specially so it always gets installed in the right place.
prefix = get_option('prefix')
sysconfdir = get_option('sysconfdir')
if prefix == '/usr'
    base = sysconfdir
else
    base = join_paths(prefix, sysconfdir)
endif
hound_confdir = join_paths(base, 'hound')

schemadir = join_paths(hound_confdir, 'schema')
conf.set_quoted('CONFIG_HOUND_SCHEMADIR', schemadir)
configure_file(
    input: 'include/hound-private/config.h.in',
    output: 'config.h',
    configuration : conf)

src = [
    'src/core/ctx.c',
    'src/core/driver.c',
    'src/core/driver-ops.c',
    'src/core/error.c',
    'src/core/entrypoint.c',
    'src/core/error.c',
    'src/core/hound.c',
    'src/core/io.c',
    'src/core/log.c',
    'src/core/queue.c',
    'src/core/parse/common.c',
    'src/core/parse/config.c',
    'src/core/parse/schema.c',
    'src/core/refcount.c',
    'src/core/util.c',
    'src/driver/util.c'
]

# Dependencies.
threads_dep = dependency('threads')
xlib_dep = dependency('xlib')
lib_deps = [
    threads_dep,
    xlib_dep,
    dependency('yaml-0.1')
]

# Drivers.
drivers = {
    'gps': {
        'deps': ['libgps'],
        'header': 'gps.h',
        'schema': 'schema/driver/deploy/gps.yaml',
        'src': ['src/driver/gps.c']
    },
    'iio': {
        'deps': [],
        'schema': 'schema/driver/deploy/iio.yaml',
        'src': ['src/driver/iio.c']
    },
    'obd': {
        'deps': ['yobd'],
        'header': 'obd.h',
        'schema': 'schema/driver/deploy/sae-standard.yaml',
        'src': ['src/driver/obd.c']
    }
}

foreach name, props : drivers
    if not get_option(name)
        message('skipping driver "' + name + '" by user request')
        continue
    endif
    src += props.get('src')
    foreach dep : props.get('deps')
        lib_deps += dependency(dep)
    endforeach
    install_data(props.get('schema'), install_dir: schemadir)
    driver_header = 'include/hound/driver/' + name + '.h'
    if 'header' in props
      install_headers(
          'include/hound/driver/' + props.get('header'),
          subdir: 'hound/driver')
    endif
endforeach

lib = library(
    'hound',
    src,
    include_directories: include,
    install: true,
    dependencies: lib_deps,
    version: meson.project_version())
pkg.generate(
    name: 'hound',
    description: 'A generic, performant sensor gathering library',
    libraries: [lib],
    version: meson.project_version())
hound_dep = declare_dependency(link_with: lib, include_directories: include)

if get_option('obd') and (get_option('install-tools') or get_option('build-tests'))
    # OBD-II server simulator, used for unit tests and installable as a
    # standalone utility.
    obdsim = executable(
        'obdsim',
        ['test/obd/sim.c'],
        include_directories: include_directories('test/include'),
        dependencies: [dependency('yobd'), xlib_dep, threads_dep],
        install: get_option('install-tools'))
endif

if get_option('install-tools')
    if get_option('obd')
        install_data('scripts/yobd-to-hound', install_dir: get_option('bindir'))
    endif
endif

if get_option('build-tests')
    subdir('test')
endif

# Documentation.
run_target('docs', command: 'meson/makedoc')

# Static analysis.
run_target('check', command: 'meson/check')
run_target('clang-tidy', command: 'meson/clang-tidy')
run_target('schema-check', command: 'meson/schema-check')

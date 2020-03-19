# Includes. Exclude the driver directory from the install, as driver includes
# will be installed only if the associated driver is enabled.
include = '../include'
install_subdir(
    join_paths(include, 'hound'),
    install_dir: 'include',
    exclude_directories: 'driver')

# Config header generation.
conf = configuration_data()

# sysconfdir is special, as when the prefix is /usr, it gets set to /etc. So we
# need to treat this specially so it always gets installed in the right place.
prefix = get_option('prefix')
sysconfdir = get_option('sysconfdir')
if prefix == '/usr'
    base = ''
    pkgconfig_base = ''
else
    base = prefix
    pkgconfig_base = '${prefix}'
endif
confdir = join_paths(sysconfdir, 'hound')
schemadir = join_paths(confdir, 'schema')

hound_schemadir = join_paths(base, schemadir)

confdir_pkgconfig = join_paths(pkgconfig_base, confdir)
schemadir_pkgconfig = join_paths('${confdir}', 'schema')
pkgconfig_vars = ['confdir=' + confdir_pkgconfig,
                  'schemadir=' + schemadir_pkgconfig]

conf.set_quoted('CONFIG_HOUND_SCHEMADIR', schemadir)
configure_file(
    input: join_paths(include, 'hound-private/config.h.in'),
    output: 'config.h',
    configuration : conf)

src = [
    'core/ctx.c',
    'core/driver.c',
    'core/driver-ops.c',
    'core/error.c',
    'core/entrypoint.c',
    'core/error.c',
    'core/hound.c',
    'core/io.c',
    'core/log.c',
    'core/queue.c',
    'core/parse/common.c',
    'core/parse/config.c',
    'core/parse/schema.c',
    'core/refcount.c',
    'core/util.c',
    'driver/util.c'
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
        'schema': 'gps.yaml',
        'src': ['driver/gps.c']
    },
    'iio': {
        'deps': [],
        'schema': 'iio.yaml',
        'src': ['driver/iio.c']
    },
    'obd': {
        'deps': ['yobd'],
        'header': 'obd.h',
        'src': ['driver/obd.c']
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
    if 'schema' in props
        schema = join_paths(
            meson.source_root(),
            'schema/driver/deploy',
            props.get('schema'))
        install_data(schema, install_dir: schemadir)
    endif
    driver_header = join_paths('include/hound/driver', name + '.h')
    if 'header' in props
      install_headers(
          join_paths(include, 'hound/driver', props.get('header')),
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
    version: meson.project_version(),
    variables: pkgconfig_vars)
hound_dep = declare_dependency(link_with: lib, include_directories: include)

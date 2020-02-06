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
hound_confdir = join_paths(
    get_option('prefix'),
    get_option('sysconfdir'),
    'hound')
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
        'schema': 'schema/driver/deploy/standard-pids.yaml',
        'src': ['src/driver/obd.c']
    }
}
driver_tests = []
foreach name, props : drivers
    if get_option(name).disabled()
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

# Tools.
if not get_option('obd').disabled()
    # OBD-II server simulator.
    obdsim = executable(
        'obdsim',
        ['test/obd/sim.c'],
        include_directories: include_directories('test/include'),
        dependencies: [dependency('yobd'), xlib_dep, threads_dep])
endif

# Automated unit tests.
run_target('can-setup', command: ['meson/vcan', 'setup'])
run_target('can-destroy', command: ['meson/vcan', 'destroy'])

add_test_setup(
    'valgrind',
    exe_wrapper: ['valgrind', '--trace-children=yes'],
    timeout_multiplier: 100)
# meson won't let us pass a directory to files(), so we use current_source_dir()
# as a workaround.
prod_schema_dir = join_paths(
    meson.source_root(),
    meson.current_source_dir(),
    'schema',
    'driver',
    'deploy')
test_schema_dir = join_paths(
    meson.source_root(),
    meson.current_source_dir(),
    'test',
    'schema')
tests = {
    'nop': {
        'src': ['test/driver/nop.c', 'test/nop.c'],
        'deps': [],
        'unit-test': {
            'args': [test_schema_dir, files('test/config/nop.yaml')],
            'is-parallel': true,
        },
    },
    'counter': {
        'src': ['test/driver/counter.c', 'test/counter.c'],
        'deps': ['valgrind'],
        'unit-test': {
            'args': [test_schema_dir, files('test/config/counter.yaml')],
            'is-parallel': true,
        },
    },
    'file': {
        'src': ['test/driver/file.c', 'test/file.c'],
        'deps': [],
        'unit-test': {
            'args': [test_schema_dir, files('test/data/testfile')],
            'is-parallel': true,
        }
    }
}
driver_tests = {
    'gps': {
        'deps': ['libgps'],
        'src': ['test/gps.c'],
    },
    'iio': {
        'deps': [],
        'src': ['test/iio.c'],
    },
    'obd': {
        'deps': ['valgrind', 'yobd'],
        'src': ['test/obd/test.c'],
        'unit-test': {
            'args': ['hound-vcan0', obdsim.full_path(), prod_schema_dir],
            'is-parallel': false
        }
    }
}
# Add in any enabled driver tests.
foreach name, t : driver_tests
    if not get_option(name).disabled()
        tests += {name: t}
    endif
endforeach

foreach name, t : tests
    deps = [threads_dep, xlib_dep, hound_dep]
    foreach dep : t.get('deps')
        deps += dependency(dep)
    endforeach
    exe = executable(
        name + '-test',
        t.get('src'),
        include_directories: include_directories('test/include'),
        dependencies: deps)
    if 'unit-test' in t
        props = t.get('unit-test')
        test(
            name,
            exe,
            args: props.get('args'),
            is_parallel: props.get('is-parallel'),
            timeout: 50)
    endif
endforeach

# Documentation.
run_target('docs', command: 'meson/makedoc')

# Static analysis.
run_target('check', command: 'meson/check')
run_target('clang-tidy', command: 'meson/clang-tidy')
run_target('schema-check', command: 'meson/schema-check')

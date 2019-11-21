# Summary.
project('hound', 'c',
    version: '0.5',
    license: 'proprietary',
    default_options: [
        'c_std=c11',
        'buildtype=debug',
        'warning_level=3',
        'werror=true'])

cflags = ['-Wshadow', '-fvisibility=internal']
add_project_arguments(cflags, language: 'c')

pkg = import('pkgconfig')

# Includes.
include = include_directories('include')
install_subdir('include/hound', install_dir: 'include')

# Config header generation.
conf = configuration_data()
hound_confdir = join_paths(
    get_option('prefix'),
    get_option('sysconfdir'),
    'hound')
hound_schemadir = join_paths(hound_confdir, 'schema')
conf.set_quoted('CONFIG_HOUND_SCHEMADIR', hound_schemadir)
configure_file(
    input: 'include/hound-private/config.h.in',
    output: 'config.h',
    configuration : conf)

# Library.
gps_dep = dependency('libgps')
yaml_dep = dependency('yaml-0.1')
thread_dep = dependency('threads')
xlib_dep = dependency('xlib')
lib_deps = [
    gps_dep,
    yaml_dep,
    thread_dep,
    xlib_dep
]

# Install schemas.
install_subdir(
    'schema/deploy',
    install_dir: hound_schemadir,
    strip_directory: true)


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
    'src/core/refcount.c',
    'src/core/schema.c',
    'src/core/util.c',
    'src/driver/can.c',
    'src/driver/gps.c',
    'src/driver/iio.c',
    'src/driver/util.c',
]

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

# Tests.
run_target('can-setup', command: ['meson/vcan', 'setup'])
run_target('can-destroy', command: ['meson/vcan', 'destroy'])

valgrind_dep = dependency('valgrind')
manual_test_deps = [hound_dep] + [thread_dep] + [xlib_dep]
unit_test_deps = manual_test_deps + [valgrind_dep]
test_include = include_directories('test/include')

add_test_setup(
    'valgrind',
    exe_wrapper: ['valgrind'],
    timeout_multiplier: 100)
# meson won't let us pass a directory to files(), so we use current_source_dir()
# as a workaround.
prod_schema_dir = join_paths(
    meson.source_root(),
    meson.current_source_dir(),
    'schema',
    'deploy')
test_schema_dir = join_paths(
    meson.source_root(),
    meson.current_source_dir(),
    'test',
    'schema')
tests = [
    ['nop', ['test/driver/nop.c', 'test/nop.c'], test_schema_dir, true],
    [
        'counter',
        ['test/driver/counter.c', 'test/counter.c'],
        test_schema_dir,
        true,
    ],
    [
        'file',
        ['test/driver/file.c', 'test/file.c'],
        [test_schema_dir, files('test/data/testfile')],
        true
    ],
    ['can', ['test/can.c'], ['hound-vcan0', prod_schema_dir], false]
]
foreach t : tests
    exe = executable(
        t.get(0),
        t.get(1),
        include_directories: test_include,
        dependencies: unit_test_deps)
    test(t.get(0), exe, args: t.get(2), is_parallel: t.get(3), timeout: 50)
endforeach

# These tests are manual.
executable(
    'iio_test',
    'test/iio.c',
    include_directories: test_include,
    dependencies: manual_test_deps)
executable(
    'gps_test',
    'test/gps.c',
    include_directories: test_include,
    dependencies: manual_test_deps + [gps_dep])

# Documentation.
run_target('docs', command: 'meson/makedoc')

# Static analysis.
run_target('check', command: 'meson/check')
run_target('clang-tidy', command: 'meson/clang-tidy')
run_target('schema-check', command: 'meson/schema-check')

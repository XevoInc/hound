# Summary.
project('hound', 'c',
    version: '0.3',
    license: 'proprietary',
    default_options: [
        'c_std=c11',
        'buildtype=debug',
        'warning_level=3',
        'werror=true'])

cflags = ['-Wshadow', '-fvisibility=hidden']
add_project_arguments(cflags, language: 'c')

pkg = import('pkgconfig')

# Includes.
include = include_directories('include')
install_subdir('include/hound', install_dir: 'include')

# Library.
thread_dep = dependency('threads')
xlib_dep = dependency('xlib', fallback: ['xlib', 'xlib_dep'])
lib_deps = [
    thread_dep,
    xlib_dep
]

src = [
    'src/core/ctx.c',
    'src/core/driver.c',
    'src/core/error.c',
    'src/core/entrypoint.c',
    'src/core/error.c',
    'src/core/hound.c',
    'src/core/io.c',
    'src/core/log.c',
    'src/core/queue.c',
    'src/core/refcount.c',
    'src/core/util.c',
    'src/driver/can.c',
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
test_deps = [hound_dep] + [thread_dep] + [valgrind_dep] + [xlib_dep]
test_include = include_directories('test/include')

add_test_setup(
    'valgrind',
    exe_wrapper: ['valgrind'],
    timeout_multiplier: 100)
tests = [
    ['nop', ['test/driver/nop.c', 'test/nop.c'], [] ],
    ['counter', ['test/driver/counter.c', 'test/counter.c'], [] ],
    ['file', ['test/driver/file.c', 'test/file.c'], files('test/data/testfile') ],
    ['can', ['test/can.c'], ['hound-vcan0'] ]
]
foreach t : tests
    exe = executable(
        t.get(0),
        t.get(1),
        include_directories: test_include,
        dependencies: test_deps)
    test(t.get(0), exe, args: t.get(2), timeout: 50)
endforeach

# Documentation.
run_target('docs', command: 'meson/makedoc')

# Static analysis.
run_target('check', command: 'meson/check')
run_target('clang-tidy', command: 'meson/clang-tidy')

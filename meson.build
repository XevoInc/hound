# Summary.
project('hound', 'c',
    version: '0.1',
    license: 'proprietary',
    default_options: [
        'c_std=c11',
        'buildtype=debug',
        'warning_level=3',
        'werror=true'])
# Sadly we have to duplicate this because meson won't let us have anything but
# project() as the first line in the file.
version = '0.1'

cflags = ['-Wshadow', '-fvisibility=hidden']
add_project_arguments(cflags, language: 'c')

pkg = import('pkgconfig')

# Enable threads.
thread_deps = dependency('threads')

# Includes.
include = include_directories('include')
install_subdir('include/hound', install_dir : 'include/hound')

# libhound.
src = [
    'src/core/assert.c',
    'src/core/ctx.c',
    'src/core/driver.c',
    'src/core/entrypoint.c',
    'src/core/error.c',
    'src/core/hound.c',
    'src/core/io.c',
    'src/core/log.c',
    'src/core/queue.c',
    'src/core/refcount.c',
    'src/core/util.c'
]

# Library
lib = library(
    'hound',
    src,
    include_directories: include,
    install: true,
    dependencies: thread_deps,
    version: version)
pkg.generate(
    name: 'hound',
    description: 'A generic, performant sensor gathering library',
    libraries: [lib],
    subdirs: 'hound',
    version: version)

# Tests.
tests = [
    ['nop', ['test/driver/nop.c', 'test/nop.c']],
    ['counter', ['test/driver/counter.c', 'test/counter.c']]
    ]
foreach t : tests
    exe = executable(
        t.get(0),
        t.get(1),
        include_directories: include,
        link_with: lib,
        dependencies: [thread_deps])
    test(t.get(0), exe)
endforeach

# Documentation.
run_target('docs', command: 'meson/makedoc')

# Static analysis.
run_target('check', command: 'meson/check')
run_target('clang-tidy', command: 'meson/clang-tidy')

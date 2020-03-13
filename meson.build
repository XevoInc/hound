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

subdir('src')

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

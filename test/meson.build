# Scripts to setup the testing infrastructure.
run_target('can-setup', command: ['meson/vcan', 'setup'])
run_target('can-destroy', command: ['meson/vcan', 'destroy'])

add_test_setup(
    'valgrind',
    exe_wrapper: ['valgrind', '--trace-children=yes'],
    timeout_multiplier: 100)
# meson won't let us pass a directory to files(), so we use current_source_dir()
# as a workaround.
example_schema_dir = join_paths(
    meson.source_root(),
    'schema',
    'driver',
    'example')
test_schema_dir = join_paths(meson.current_source_dir(), 'schema')

tests = {
    'nop': {
        'src': ['driver/nop.c', 'nop.c'],
        'deps': [],
        'unit-test': {
            'args': [test_schema_dir, files('config/nop.yaml')],
            'is-parallel': true,
        },
    },
    'counter': {
        'src': ['driver/counter.c', 'counter.c'],
        'deps': ['valgrind'],
        'unit-test': {
            'args': [test_schema_dir, files('config/counter.yaml')],
            'is-parallel': true,
        },
    },
    'file': {
        'src': ['driver/file.c', 'file.c'],
        'deps': [],
        'unit-test': {
            'args': [test_schema_dir, files('data/testfile')],
            'is-parallel': true,
        }
    }
}

# Driver tests.
if get_option('gps')
    tests += {
        'gps': {
            'deps': ['libgps'],
            'src': ['gps.c'],
        }
    }
endif
if get_option('iio')
    tests += {
        'iio': {
            'deps': [],
            'src': ['iio.c'],
        }
    }
endif
if get_option('obd')
    tests += {
        'obd': {
            'deps': ['valgrind', 'yobd'],
            'src': ['obd/test.c'],
            'unit-test': {
                'args': ['hound-vcan0', obdsim.full_path(), example_schema_dir],
                'is-parallel': false
            }
        }
    }
endif
if get_option('mqtt')
    tests += {
        'mqtt': {
            'deps': ['libmosquitto', 'msgpack', 'valgrind'],
            'src': ['mqtt.c'],
            'unit-test': {
                'args': [example_schema_dir],
                'is-parallel': false
            }
        }
    }
endif

foreach name, t : tests
    deps = [threads_dep, xlib_dep, hound_dep]
    foreach dep : t.get('deps')
        deps += dependency(dep)
    endforeach
    exe = executable(
        name + '-test',
        t.get('src'),
        include_directories: include_directories('include'),
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

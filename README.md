# Hound
Hound -- named after the dog with excellent sense of smell -- is a library for
Linux intended to easily and performantly read data from sensors on embedded
devices.  Common sensors handled include CAN and many analog sensors (e.g.
accelerometer, gyroscope, and anything else handled by the Linux Kernel
Industrial I/O subsystem). The idea is to abstract away the differences between
sensor types by binding all the sensor to a common API. In doing so, application
code can write to a single API without concern for the underlying device,
driver. In addition, application code can worry less about a host of performance
issues, like queueing samples, minimizing latency, etc.

## Build

### Prerequisites
- meson: `pip3 install meson`

- ninja: `pip3 install ninja`

- libgps: `sudo apt install libgps-dev`

- libYAML: `sudo apt install libyaml-dev`

- libmosquitto: `sudo apt install libmosquitto-dev`. Also a mosquitto broker for
  the unit tests: `sudo apt install mosquitto`.

- libmsgpack: `sudo apt install libmsgpack-dev`.

- xlib: compile from source: https://github.com/XevoInc/xlib

- yobd: compile from source: https://github.com/XevoInc/yobd

- (optional, for developers) `clang-tidy`. This is used for static analysis and
  is thus a build but not runtime requirement.

- (optional, for developers) `valgrind`. This is used in unit tests.

- (optional, for developers) Python requirements, as documented in
  `dev-requirements.txt`. These are some build-time tools that are not required
  for runtime. They are used for sanity-checking schemas. You can install them
  from your distro or via `pip install -r requirements.txt`.

### Custom requirements building

Note that, if any of your build prerequisites do not come from standard distro
packaging, you will need also need to tweak the following env vars:

- `PKG_CONFIG_PATH` needs to be set only when you run `meson` and doesn't matter
  after that. It should be set to the directory containing the `.pc` files used
  by the prerequisite you built.
- `LD_LIBRARY_PATH` needs to be set whenever you actually load the Hound
  library, such as when you run the unit tests with `ninja test`. It should be
  set to the directory containing the built prerequisite libraries.

### Debian unstable

To get your build requirements, you just need to run:

```
sudo apt-get -y install meson ninja-build
```

### Fedora packages

To get your build requirements, you just need to run:

```
sudo dnf -y install meson ninja-build
```

Note that on fedora you will substitute the `ninja-build` command instead of
the `ninja` command for the various build commands on this page.

## Instructions

### First time builds

```
mkdir build
cd build
meson ..
ninja
```

### Rebuilding

To rebuild at any time, you just need to rerun the last `ninja` command:

```
ninja
```

You can run this command from any directory you like. For instance, to rebuild
from the top level directory, use the ninja `-C` option to point ninja at the
build directory:

```
ninja -C build
```

Also, there is nothing special about the directory name `build`. If you prefer a
different name than `build`, this is not a problem, and you can have different
build directories with different configurations; meson and ninja don't care.

### Compiling with clang instead of gcc

If you want to use clang, it's the usual meson methodology:

```
mkdir build.clang
cd !$
CC=clang meson ..
ninja
```

### Running tests
To run the unit tests, first use the `can-setup` script to setup virtual CAN
devices. Note that this script requires sudo:
```
ninja can-setup
```
After that, you can use either of these commands to run the tests:
```
ninja test
meson test
```

To run the tests under valgrind, you can use:
```
meson test --setup valgrind
```
Note that `ninja test` actually calls `meson test`.

Before checking in, you should run:
```
ninja check
```

Which runs unit tests, does static analysis, and anything else that is deemed
"good hygiene" prior to checking in. This list may change over time, but the
`check` target should remain valid.

### Static analysis
Static analysis uses `clang-tidy` and can be run with:
```
ninja clang-tidy
```

Note that you will need to install the `clang-tidy` tool, either via distro or
some other method.

### Generating documentation
Code documentation is handled by `doxygen` and can be built with:
```
ninja docs
```

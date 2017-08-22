# Hound
Hound -- named after the dog with excellent sense of small -- is a library for
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
- `meson` (recent). Unless your distro has a recent meson (probably 2016 or
  later), you should install via pip: `pip3 install meson`, or get it from
  source at http://mesonbuild.com/. If you install via `pip3` without sudo, then
  you will also need to add `$HOME/.local/bin` to your `PATH` to find the
  `meson` executable. You also may be able to get it from a later version
  available in your distro (e.g. Debian sid as opposed to stable). If you
  encounter strange errors when you run `meson`, it is likely you need a newer
  version. At time of writing (August 22, 2017), version `0.41.2` works, but an
  older version may work as well.

- `ninja` (>= 1.6). If your distro's version is older, the following directions
  will download and install a more recent version:
    - ```wget https://github.com/ninja-build/ninja/releases/download/v1.7.2/ninja-linux.zip```
    - ```unzip ninja-linux.zip```
    - ```sudo install -o root -g root -m755 ninja /usr/bin/ninja # or put it somewhere else in your PATH```

- (optional, for developers) `clang-tidy`. This is used for static analysis and
  is thus a build but not runtime requirement.

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

### Fedora 24 and 25 packages

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
Do this to run unit tests:
```
ninja test
```
or
```
mesontest --setup valgrind
# ninja test actually calls mesontest
```

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

# Driver initialization
Drivers are initialized with:
- `name`: this name must match a string supplied by a driver in a library
  constructor via a call to `driver_register`. This is used by the hound core to
  find the appropriate driver ops struct.
- `path`: a path to a device file or other location that a driver would
  understand. This is used by the hound core to allow for multiple drivers
  backing different devices. For instance, if a system has two CAN interfaces,
  then the yobd driver might be initialized twice, with `path` specifying a
  different interface in each case.
- `schema base`: the location in which to look for schema files
- `initialization args`: driver-specific initialization data. Each argument is a
  structure containing type and data.

Since the driver `initialization data` is different per driver, its format is
checked by each driver and documented below.

## GPS initialization data
No initialization data needed; just supply a count of 0 and a NULL array.

## IIO initialization data
A single uint64 value specifying the number of nanoseconds to keep in the
device's I/O buffer (the size of the kernel's internal circular buffer used for
collecting samples that haven't yet been read by userspace).

## OBD-II initialization data
A single null-terminated string (type "bytes") argument indicating the yobd
schema to use. This will be directly passed to yobd.

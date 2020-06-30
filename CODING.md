# Coding Standard

This coding standard applies to all code in master. The coding standard is in place for a few reasons:

- Code quality: Some of the rules are in place because they help reduce bugs.

- Readability: Having consistent code means the brain can turn off its style checker and focus only on content. In addition, having a consistent style makes it easier for new team members to join and quickly become productive.

- Code reviews: If we agree on a coding standard, we can spend our code review time on code quality rather than arguing about style.

PEP-8 has some excellent guidelines regarding coding standards in general: [A Foolish Consistency is the Hobgoblin of Little Minds](https://www.python.org/dev/peps/pep-0008/#a-foolish-consistency-is-the-hobgoblin-of-little-minds). Those guidelines apply to this coding standard as well, notably that if there's a good reason to deviate from the coding standard to make the code more readable or maintainable, this is allowable but should be discussed with others during code review.

## File Organization

Files should be grouped into the following sections:

- Copyright header, in Doxygen format.

- `#include` guards for header files.
- Any required feature test macros (e.g. `_POSIX_C_SOURCE`)

- All `#includes`, sorted in alphabetical order.
- Any other `#defines`, sorted in alphabetical order.

- `struct` definitions or `typedefs`, each documented Doxygen-style.
- `static` and (hopefully not) global variables.
- Function definitions (for C files) or declarations (for headers), with each documented Doxygen-style. There should be a blank line between each of these.

Sections should be separated by one blank line.

## File Naming

Files should be named like `this-is-a-filename`. Implementation files get a `.c` extension and headers get a `.h` extension. Don’t use **under_scores** or **UPPERCASE** characters.

### Headers

There are two categories of headers:

- Public, exported headers that should get installed when the project is installed and usable by other projects.

- Private headers used only inside this project.

All headers should have `#include` guards right after the copyright notice, formatted like this:

```c
#ifndef PROJECT_PATH_H_
#define PROJECT_PATH_H_

...

the rest of the header file

...

#endif /* PROJECT_PATH_H_ */
```

The name of the `#include` guard is a mangling of the path after `include/`, where `/` is translated into `_`, and we always finish with a trailing `_`. This name mangling is designed to provide a unique `#include` guard that shouldn’t conflict with anything else as well as relieving developers of thinking of names. Below are a few example mappings to give you the idea:

    include/jio/error.h → JIO_ERROR_H_
    include/jio/core/io.h → JIO_CORE_IO_H_
    include/jio-private/msg.h → JIO_PRIVATE_MSG_H_
    include/jio-private/parse/header.h → JIO_PRIVATE_PARSE_HEADER_H_

Don’t use `#pragma` once, as it’s technically non-standard and is not 100% reliable when factoring in search paths and determining when two files are the same. See this [Stack Overflow answer](https://stackoverflow.com/questions/1143936/pragma-once-vs-include-guards/34884735#34884735) from a GCC developer for details.

### Repository Organization

Repositories should contain the following files/directories:

- `src`: All .c files go here.
- `include/PROJECT: Public headers go here. Anything here should be automatically installed by **CMake**. For example, for a project called `jio`, the directory should be `include/jio`.
- `include/PROJECT-private`: Private headers go here. These should not be installed or referenced outside of this repository.
- `test`: Unit tests go here.
- `meson.build`: The base **Meson** file goes here. Any subdirectories inside `src` and `test` should additionally contain a `meson.build` file. If this is the case, the parent `meson.build` file should contain a `subdir` invocation to call into the child `meson.build`.

Subdirectories of any of these directories are permitted, at the author’s discretion.

## Formatting

- 4 spaces per indent level. No tabs.

- `\n` for line endings (Unix-style).

- Maximum 80 characters per line. Code that doesn’t fit in 80 characters or less is generally a function call with a lot of parameters. Such code should be formatted like this:

  ```c
  int x = some_function_that_doesnt_fit(
      param_one,
      param_two,
      param_three,
      other_param,
      yet_another_param);
  ```

- No trailing whitespace at the end of the line. This includes empty lines. Please configure your editors to automatically remove this cancer upon our souls. If you find a file that has this issue, please fix it as a separate commit.

- Generally put spaces between operators: `x + y`, not `x+y`. The exception is where careful spacing makes order of operations clearer or the expression more readable: `ab + xy` is preferred over `a * b + x * y` or `(a * b) + (x * y)`.

- Don’t put a space before or after `++` or `--`: `++x`, not `++ x`.

- Use only `++x` instead of `x++`. Since later guidance tells us not to use these unless they are on the same line, this is mostly an aesthetic consideration for consistency. In C++, this matters (due to operator overloading), and `++x` should generally be preferred, so we might as well go with that and establish good habits when you switch codebases and write C++.

- No space after `*` when used for pointer dereference: `*p` not `* p`.

- Declare pointers with a space after the type but not before the variable name, like this: `const char *p`. When declaring a variable `const`, put the `const` to the left of the type. For instance, `const char *s`, not `char const *s`. This is an exception to the normal rule that `const` applies to the type to its left, but it’s an exception that everybody is used to, so it makes the code more readable for most people.

- Add a space after casts: `(int) x`, not `(int)x`.

- Put parentheses “against the wall”: No space after `(` or before `)`. Example: `if ((c = getchar() != EOF)` instead of if `( ( c = getchar() ) != EOF )`.

- Generally, don’t do weird things with formatting just because C lets you.

### Comments and whitespace

Single-line comments look like this:

```c
/* This is a comment. */
```

Multi-line comments look like this:

```c
/*
 * This is a comment containing many lines.
 * Note: This comment is formatted correctly.
 */
```

Always capitalize the first letter of comments, unless it refers to a variable that shouldn’t be capitalized. Always end the comment with a period, like in prose.

Don’t use `//`, as it’s easy to start mixing the `//` and `/*` styles of multi-line comments, sowing chaos and anarchy across the codebase.

### Doxygen

All public function  and `struct` definitions should be documented using **Doxygen**. Verbose explanations are not needed or recommended. For functions, each parameter should have a matching `@param[in]` or `@param[out]` declaration, and there should be a `@return` declaration if the function returns anything. `@param[in]` is used for input parameters that are not changed, `@param[out]` is used for output parameters that are written to but not read, and `@param[in,out]` is used for parameters that are both read from and written to (these are likely rare). There should be blank lines between the summary, the `@param` declarations, and the `@return` declaration, if any.
Function declarations

An example is worth a thousand words:

```c
/**
 * This function sums two numbers.
 *
 * @param[in] n a number
 * @param[in] m another number
 *
 * @return the result n + m
 */

static void sum_two(int n, int m)
{
    return n + m;
}
```

Functions that don’t fit on one line should be handled like this:

```c
[doxygen stuff here]
static int sum_two(
    struct somestruct param_one,
    int param_two,
    double some_other_param,
    const char *another_param)
{
    [some implementation here]
}
```

Note the closing parenthesis is not on its own line, and the opening brace is on its own line. These are purposeful deviations from normal formatting conventions.

### Functions

- Functions with no arguments should have an explicit `void`, like this: `do_something(void)` instead of just `do_something()`.

- All public (exported) functions that can fail should return an error code rather than providing it by pointer. If output parameters are required (a function that needs to return both an error and a result), the output parameters should be last in the parameter list. Example:

  `static jio_err get_fd_index(int fd, size_t len, const int *a, size_t *index)`

  If `get_fd_index` succeeds, it returns JIO_OK and populates index. Otherwise, it returns an error code and may or may not set index. Of course, this behavior should be documented with **Doxygen**.

- Any function not used outside its own file should be declared `static`.

### Variable declarations

Declare all local variables at the top of function, like in C89. These should be alphabetized by variable name. This allows the use of the `goto error` pattern without variable scoping issues. Specifically, jumping past the initialization of a variable can cause bugs when you accidentally reference that variable in one of the error labels. GCC is very conservative about warning about this, and fixing up the code can become quite painful, but declaring everything at the top of the function makes the issue go away.

### Variable naming

- **Variables, functions, and types**: `all_lower_case`

- **Macros**: `ALL_UPPER_CASE`

- **Anything exported** (variable, function, type, etc.) must be prefixed with a project-specific namespace. For example, for the **jio** project, anything exported should be prefixed with `jio_`. This namespacing helps prevent symbol collision with other libraries.

- **Typedefs**: Don’t typedef `structs` or pointers, with the important exception of function pointers. Typedef’ing primitive types is OK (e.g. `typedef uint32_t data_id`). Always typedef `enums` and declare them as anonymous like this:

  ```c
  typedef enum {
      BLUEBERRY,
      CHERRY,
      APPLE,
      MANGO
  } fruit;
  ```

  This forces everyone to use the type as `fruit` instead of `enum fruit`.

- Any `enums` that might get persisted anywhere (disk, cloud, etc.) or need to remain stable across versions must have explicitly defined values, and already-defined values cannot be changed.

- Static variables start with `s_`.

- Global variables start with `g_`. Each global variable must have a comment right above it indicating why a global variable is needed.

- Thread-local variables start with `t_`.

- Names should generally be short and sweet, Unix-style, though not cryptic (no `creat`!). If there’s any uncertainty about the name’s meaning, think of a better name or add more characters to make it clear.

## Control statements

### Conditionals

```c
if (x == a) {
    do something
}
else if (x == b) {
    do something else
}
else if (y == c) {
    do yet another thing
}
else {
    do a thing
}
```

A few notes on conditionals:

- No Yoda conditionals!

- It is discouraged but not absolutely banned to put an assignment inside a conditional, for example:

  ```c
  /* Do NOT generally do this. */
  if ((err = func(arg)) != 0) {
  ```

  Occasionally, it’s very awkward to avoid this, so if it’s clearly more readable to put the assignment inside the conditional, it’s permitted. However, always parenthesize the assignment expression: `if ((err = func(arg)) != 0)` and not `if (err = func(arg) != 0)`.

- Always be explicit with pointer comparisons: `if (p != NULL)` rather than `if (p)`.

- Always be implicit with boolean comparisons: `if (done)` rather than `if (done == true)`.

- Parenthesize any complex expressions inside a conditional if there is any doubt at all as to the order of evaluation, or if parenthesizing enhances readability.

### while and do-while loops

```c
while (condition) {
    do something
}
```


```c
do {
    do something
} while (condition);
```

### for loops


```c
for (i = 0; i < len; ++i) {
    do something
}
```

### switch statements

```c
switch (x) {
    case 0:
        break;
    case 1:
        break;
}
```

When switching on `enums` and adding a case for every possible value, don’t add a `default` case. This way, if you change the `enum` and forget to add a matching case for it, the compiler will complain. When switching on an open universe of values (nearly anything except an `enum`), always consider if you need a default case: It’s easy to forget.

In the rare case that you need to fall through a case, explicitly add a comment for it like this:

```c
switch (x) {
    case 0:
    /* fall-through */
    case 1:
}
```

This way, the reader won’t think this is a mistake and break your code by adding a break.

### Ternary statements

Ternary statements are discouraged by not explicitly banned. Please be reasonable and don’t put them inside complex expressions.

### Braces in control statements

Always add braces to control statements, even when they are a single line. Not doing this has caused some very expensive bugs.

## Compilation

### Compiler flags

Always compile with the following flags for GCC/Clang:

```sh
-std=c11 -Werror -Wall -Wextra -Wpedantic -Wshadow -fvisibility=internal
```

Notably, compiling with `-std=c11` makes sure that anything not standard C11 requires a `#define` to expose feature test macros for POSIX or GNU-specific functions. Importantly, this means we can grep for these feature test macros and identify what would need to be changed if we need to port to another system.

### Symbol visibility

`-fvisibility=internal` makes all symbols private (not exported in the object file) by default. To declare a public symbol (which should be done only from public headers), you can put the following right above the declaration:

```c
__attribute__ ((visibility ("default")))
```

This works with both GCC and Clang.

As a convenience, projects may wish (but are not required) to put the following definition in a private header:

```c
#define PUBLIC_API __attribute__ ((visibility ("default")))
```

In which case, you can declare public functions like this:

```c
PUBLIC_API int somefunc(int y);
```

Explicitly declaring symbols public disallows other projects from accidentally using symbols that are meant to be private, makes the public library interface clear, and decreases the size of binaries.

## Good practices

### Macros

Avoid using macros whenever an inline function will do. Any macro functions must fully parenthesize all arguments and be used with caution.

### Assertions and logging

Use the **[xlib](https://github.com/XevoInc/xlib/)** library for assertions and logging so that all such code goes through one path.

### Threads

Use **pthreads** unless you have a good reason not to. It provides portability to other Unixes (e.g. QNX) in case we ever need it, and it’s well-understood and well-documented. Although C11 threads are appealing, they lack a lot of what **pthreads** has and use **pthreads** under the hood in most implementations anyway.

### Atomics

Use C11 atomic types when you need atomics. Think about what memory ordering you need and use the correct one rather than always picking the default. If unsure, be conservative and pick the slower memory ordering.

### Error codes

Error codes should be defined as an `enum`. `enums` give nice names to error codes, group all error codes in one place, and let you write switch statements for error codes without a default case. Each value should be explicitly defined to a value, and the values in the header should never be changed after a stable release (in order to allow error codes to persist and trust error values across versions). The success error code shall be suffixed with `_OK` and be set to 0. All other error codes should have negative values. This choice is made so that both `errno` codes and project-specific codes can fit into an `int` without colliding.

Projects should directly return positive `errno` when syscalls or standard library routines fail (e.g. if poll fails with an unhandleable code, return that directly). The exception to this is if a non-`errno` value contains more information than the corresponding `errno` value. For instance, if **jio** failed to allocate some records, returning `JIO_REC_ALLOC_FAIL` is better than returning `ENOMEM`, as it indicates where the allocation failed. Note that it's not required to always create more specific error codes, but if you choose to do so, you should use them in favor of `errno`. Thus the general rule can be summarized as: Always return the error code that contains the most information. If there's a tie, favor `errno`.

### Error handling and cleanup

Use the `goto error` pattern for cleanly exiting from a function. [This page](https://eli.thegreenplace.net/2009/04/27/using-goto-for-error-handling-in-c) documents the pattern well. Note that all error labels should have recognizable names (e.g. `err_socket_open` if a socket fails to open, rather than just `err1`).

### Early-exit, not single-return

More generally, we recommend early-exit instead of single-return. This means structuring your code as below. Note that `goto err` type of cleanup is omitted here but should be present if any cleanup is needed.

```c
err = f1();
if (err != JIO_OK) {
    return err;
}

do stuff

err = f2();
if (err != JIO_OK) {
    return err;
}

do more stuff

err = f3();
if (err != JIO_OK) {
    return err;
}

do final stuff

return JIO_OK;
```

And not like this:

```c
err = f1();
if (err == JIO_OK) {
    do stuff
    err = f2();
    if (err == JIO_OK) {
        do more stuff
        err = f3();
        if (err == JIO_OK) {
            do final stuff
        }
    }
}

return err;
```

Early-exit encourages linear code, unburdened by error cases, thus increasing readability. It also makes it much easier to stay within 80 character lines.

Note that using `continue` or `break` to weed out errors inside loops (instead of nesting deeper) is also encouraged, for the same reasons as above.

### Miscellaneous good practices

- Use `const` liberally.

- Use `sizeof(*p)` for checking the size of an underlying data type. For instance, do `malloc(len * sizeof(*p))` instead of `malloc(len * sizeof(int))`. The reason for this is that if you ever change the underlying type of `p`, only the former will continue to work correctly.

- Use assertions liberally for any errors that result from an internal bug (within program or library) and input that you completely control. If you’re pretty sure a thing should never happen but it would be very bad if that thing happened, you should probably assert on that thing. Continuing in such a context is likely worse than crashing loudly and logging a nice error message. Don’t use assertions for recoverable errors or for error conditions that could be caused by something outside of the program. General rule: If a process/library completely controls the input, assert on bad input. If the input comes from outside the process/library, handle any input gracefully. This means that a library should never crash just by the API being “used incorrectly”, but if internal routines have a clear bug, they may assert.

- Use C11 `static_assert` liberally for anything that can be reasoned about at compile-time. A good example is asserting on type sizes in order to assure safe casts or fitting types into memory.

- Don’t use `++` and `--` operators unless they are the only expression in the statement. For instance, doing `++x` by itself is OK, but don’t do `x = ++y`.

- Avoid using `extern` and prefer putting definitions in headers.

- Static variables should be used with caution, as they introduce shared state to functions (making them not automatically reentrant) and also to libraries (making them not necessarily shareable without caution).

- Use `size_t` whenever you index an array or deal with memory locations (e.g. how many bytes to copy in memcpy). `size_t` helps avoid performance issues with word size mismatches and guarantees the variable is large enough to hold the largest memory address. [This article](https://www.embedded.com/why-size_t-matters/) explains more.

- Don’t sloppily use `int` for everything. Think about what each type should really be: `unsigned` variables eliminate an entire class of bugs when appropriate. Use fixed-width types whenever overflow, portability, efficiency, or mapping to hardware is a concern. Use the C99 `_fast` and `_least` versions of fixed-width types when appropriate.

- For logic values, the C99 `bool` type rather than `int`, and never `#define TRUE` or `#define FALSE`.

- Use `volatile` whenever a variable might be changed by hardware, another process, or another thread.

- Avoid recursion, for efficiency and maintainability reasons. If you have to use recursion, make sure you can be confident that the recursion won’t get too deep and cause a stack overflow.

- Casts are a code-smell. Whenever you use one, think about if there’s a way around it and whether you have handled any overflow or other conversion issues that come up because of it.

- Long comments are a code-smell and likely indicate the code is too complex and should be simplified.

- Long functions are a code-smell and likely indicate the function should be broken up. A function may be too long if it doesn’t fit inside a single editor window with a reasonable font size.

- Don’t comment on what code does; comment on why the code does what it does. The exception to this is a comment helping the eye quickly identify sections of code, like this:

  ```c
  /* Read the file. */
  ...

  /* Check for commands to execute. */
  ...

  /* Execute commands. */
  ...
  ```

  ...

## Example code

Sometimes it’s easier to glance at code that matches the style than read a bunch of documentation. Below is an example compliant code. Note that the function liberally asserts on its inputs. This is appropriate because it’s called only by code internal to the project. If this was a publicly-exported function, it should return error codes for the conditions on which it currently asserts and should never assert on bad inputs.

 ```c
/**
 * @file      main.c
 * @brief     Main entrypoint for the Joe I/O library.
 * @author    Joe Smith <jsmith@somesite.com>
 * @copyright Copyright (C) 2020 Joe Smith. All Rights Reserved.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <jio/jio.h>
#include <poll.h>
#include <priv/defs.h>
#include <priv/error.h>
#include <priv/logging.h>
#include <pthread.h>
#include <xlib/xvec.h>

#define BUF_SIZE (100*1024)
#define PAUSE_SIGNAL SIGUSR1

/**
 * Describe this struct and what it's for.
 */
struct jio_ctx {
    int fd;
    int signal;
    uint8_t buf[BUF_SIZE];
};

static pthread_t s_poll_thread;
static pthread_mutex_t s_poll_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_poll_cond = PTHREAD_COND_INITIALIZER;

/**
 * Gets the index of an fd from an fd array.
 *
 * @param fd a file descriptor
 * @param len the length of the array of file descriptors
 * @param a an array of file descriptors
 * @param index filled in with the index where the fd was found.
 * If an error occurred, not guaranteed to be set.
 *
 * @return an error code
 */
static jio_err get_fd_index(int fd, size_t len, const int *a, size_t *index)
{
    size_t i;

    XASSERT_GT(fd, 0);
    XASSERT_NOT_NULL(a);
    XASSERT_NOT_NULL(index);

    for (i = 0; i < len; ++i) {
        if (fd == a[i]) {
            *index = i;
            return JIO_OK;
        }
    }
    return JIO_INDEX_NOT_FOUND;
}
 ```

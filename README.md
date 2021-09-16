# DMS LLVM fork

DMS implementation is at `llvm/lib/Transforms/Utils/DMS.cpp`, and DMS tests
are at `llvm/test/Transforms/DMS`.

There is also a script `summarize_dynamic_counts.sh` in the root of this repo
which is useful for collecting results from running dynamically instrumented
programs -- i.e., the data in the dynamically generated `dms_dynamic_counts`
directories.

## Building

To build, use the following:
```
cd llvm
cmake -G Ninja -B build -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" -DLLVM_TARGETS_TO_BUILD="X86" .
cd build
ninja
```

For a release (optimized) build, you can add `-DCMAKE_BUILD_TYPE=Release` or
`-DCMAKE_BUILD_TYPE=RelWithDebInfo` to the `cmake` command.
If you do either of these, you may want to also re-enable assertions with
`-DLLVM_ENABLE_ASSERTIONS=1`.

Once you've built the first time, if you've made changes and want to rebuild,
you don't have to run `cmake` again---just use `ninja`.

## Running tests

To run all LLVM regression tests, run `ninja check-llvm` from the `llvm/build` directory.

To run only the DMS regression tests, run `./build/bin/llvm-lit ./test/Transforms/DMS` from the `llvm` directory. You can add `-v` for more verbose output.

## Compiling with DMS passes

We've integrated our passes into Clang, so this is now really easy.
Just use the Clang in `./build/bin`, and add the flag `-fdms=<option>` to your
command, where `<option>` is one of:
* `static`: this runs the static DMS pass, which doesn't modify the code
  but just reports static statistics about clean/dirty pointers etc.
* `paranoid-static`: same as `static`, but doesn't trust LLVM struct types --
  see comments in `DMS.h`.
* `dynamic`: this runs the dynamic DMS pass, which instruments the code so
  that at runtime it will count dynamic statistics about clean/dirty pointers
  etc, and print those counts when the program finishes.

You can also specify more than one of these options, comma-separated: e.g.,
`-fdms=static,dynamic`.

## Running the DMS passes on individual bitcode files

### Static DMS passes:

From the `llvm` directory: `./build/bin/opt -passes=<pass> -disable-output file.ll`
where `<pass>` is either `static-dms` or `paranoid-static-dms`.
(The `-disable-output` flag avoids writing the "transformed" bitcode, which is
uninteresting for these static passes which don't do any transformations.)
You can use either a `.ll` (text-format) or `.bc` (binary-format bitcode) file
as input.
To get detailed debugging information, add the `-debug` flag.

### Dynamic DMS pass:

From the `llvm` directory: `./build/bin/opt -passes=dynamic-dms file.ll -o=file_instrumented.bc`
(again, you can use either a `.ll` or `.bc` file as input).
To get `.ll` _output_ instead of `.bc`, add the `-S` flag (and, to avoid
confusion, change the extension of the output filename).
To get detailed debugging information, add the `-debug` flag.

## Compiling with DMS passes -- the old way

These instructions are probably not useful or needed anymore, because you can
just use the Clang flags described above (much easier), but just in case,
here's the instructions.

1. If you don't already have bitcode, compile your C or C++ file to bitcode
   using any `clang` you want (doesn't have to be the one built here) and any
   compiler options you want, and adding the flags `-c -emit-llvm`.
   To get `.ll` text format instead of `.bc` bitcode format (for debugging),
   use `-S` instead of `-c`.

2. Run whichever DMS pass(es) you want using the instructions above for
   individual bitcode files.

3. For static DMS passes, you're already done. For dynamic DMS passes, to
   actually finish compiling the code so you can run it, just use `clang`,
   e.g. `clang file_instrumented.bc -o file`. This can again be done with any
  `clang` you want.

Original README follows.

---

# The LLVM Compiler Infrastructure

This directory and its sub-directories contain source code for LLVM,
a toolkit for the construction of highly optimized compilers,
optimizers, and run-time environments.

The README briefly describes how to get started with building LLVM.
For more information on how to contribute to the LLVM project, please
take a look at the
[Contributing to LLVM](https://llvm.org/docs/Contributing.html) guide.

## Getting Started with the LLVM System

Taken from https://llvm.org/docs/GettingStarted.html.

### Overview

Welcome to the LLVM project!

The LLVM project has multiple components. The core of the project is
itself called "LLVM". This contains all of the tools, libraries, and header
files needed to process intermediate representations and convert them into
object files.  Tools include an assembler, disassembler, bitcode analyzer, and
bitcode optimizer.  It also contains basic regression tests.

C-like languages use the [Clang](http://clang.llvm.org/) front end.  This
component compiles C, C++, Objective-C, and Objective-C++ code into LLVM bitcode
-- and from there into object files, using LLVM.

Other components include:
the [libc++ C++ standard library](https://libcxx.llvm.org),
the [LLD linker](https://lld.llvm.org), and more.

### Getting the Source Code and Building LLVM

The LLVM Getting Started documentation may be out of date.  The [Clang
Getting Started](http://clang.llvm.org/get_started.html) page might have more
accurate information.

This is an example work-flow and configuration to get and build the LLVM source:

1. Checkout LLVM (including related sub-projects like Clang):

     * ``git clone https://github.com/llvm/llvm-project.git``

     * Or, on windows, ``git clone --config core.autocrlf=false
    https://github.com/llvm/llvm-project.git``

2. Configure and build LLVM and Clang:

     * ``cd llvm-project``

     * ``cmake -S llvm -B build -G <generator> [options]``

        Some common build system generators are:

        * ``Ninja`` --- for generating [Ninja](https://ninja-build.org)
          build files. Most llvm developers use Ninja.
        * ``Unix Makefiles`` --- for generating make-compatible parallel makefiles.
        * ``Visual Studio`` --- for generating Visual Studio projects and
          solutions.
        * ``Xcode`` --- for generating Xcode projects.

        Some Common options:

        * ``-DLLVM_ENABLE_PROJECTS='...'`` --- semicolon-separated list of the LLVM
          sub-projects you'd like to additionally build. Can include any of: clang,
          clang-tools-extra, libcxx, libcxxabi, libunwind, lldb, compiler-rt, lld,
          polly, or cross-project-tests.

          For example, to build LLVM, Clang, libcxx, and libcxxabi, use
          ``-DLLVM_ENABLE_PROJECTS="clang;libcxx;libcxxabi"``.

        * ``-DCMAKE_INSTALL_PREFIX=directory`` --- Specify for *directory* the full
          path name of where you want the LLVM tools and libraries to be installed
          (default ``/usr/local``).

        * ``-DCMAKE_BUILD_TYPE=type`` --- Valid options for *type* are Debug,
          Release, RelWithDebInfo, and MinSizeRel. Default is Debug.

        * ``-DLLVM_ENABLE_ASSERTIONS=On`` --- Compile with assertion checks enabled
          (default is Yes for Debug builds, No for all other build types).

      * ``cmake --build build [-- [options] <target>]`` or your build system specified above
        directly.

        * The default target (i.e. ``ninja`` or ``make``) will build all of LLVM.

        * The ``check-all`` target (i.e. ``ninja check-all``) will run the
          regression tests to ensure everything is in working order.

        * CMake will generate targets for each tool and library, and most
          LLVM sub-projects generate their own ``check-<project>`` target.

        * Running a serial build will be **slow**.  To improve speed, try running a
          parallel build.  That's done by default in Ninja; for ``make``, use the option
          ``-j NNN``, where ``NNN`` is the number of parallel jobs, e.g. the number of
          CPUs you have.

      * For more information see [CMake](https://llvm.org/docs/CMake.html)

Consult the
[Getting Started with LLVM](https://llvm.org/docs/GettingStarted.html#getting-started-with-llvm)
page for detailed information on configuring and compiling LLVM. You can visit
[Directory Layout](https://llvm.org/docs/GettingStarted.html#directory-layout)
to learn about the layout of the source code tree.

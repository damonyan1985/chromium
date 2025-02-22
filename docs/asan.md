# AddressSanitizer (ASan)

[AddressSanitizer](https://github.com/google/sanitizers) (ASan) is a fast memory
error detector based on compiler instrumentation (LLVM). It is fully usable for
Chrome on Linux and Mac. There's a mostly-functional Windows port in progress
too. Additional info on the tool itself is available at
http://clang.llvm.org/docs/AddressSanitizer.html.

For the memory leak detector built into ASan, see
[LeakSanitizer](https://sites.google.com/a/chromium.org/dev/developers/testing/leaksanitizer).
If you want to debug memory leaks, please refer to the instructions on that page
instead.

## Buildbots and trybots

The [Chromium Memory
waterfall](https://ci.chromium.org/p/chromium/g/chromium.memory/console) (not to
be confused with the Memory FYI waterfall) contains buildbots running Chromium
tests under ASan on Linux (Linux ASan/LSan bots for the regular Linux build,
Linux Chromium OS ASan for the chromeos=1 build running on Linux), OS X (both 32
and 64 bits), Chromium OS (x86 and amd64 builds running inside VMs). Linux and
Linux Chromium OS bots run with --no-sandbox, but there's an extra Linux bot
that enables the sandbox (but disables LeakSanitizer).

The trybots running Chromium tests on Linux and OSX are: linux_asan (everything
except browser_tests and content_browsertests), linux_browser_asan
(browser_tests and content_browsertests), mac_asan (many tests including
browser_tests and content_browsertests), linux_chromeos_asan (the chromeos=1
build running on a Linux machine, many tests including browser_tests and
content_browsertests).

(Outdated) Blink bots: WebKit Linux ASAN buildbot and linux_layout_asan trybot.

## Pre-built Chrome binaries

You can grab fresh Chrome binaries built with ASan
[here](https://commondatastorage.googleapis.com/chromium-browser-asan/index.html).

## Build tests with ASan

If you're on mac or linux64, building with ASan is easy. Start by compiling
`base_unittests` to verify the build is working for you (see below), then you
can compile `chrome`, `browser_tests`, etc.. Make sure to compile release
builds.

Make sure you've run `tools/clang/scripts/update.py` (see
https://chromium.googlesource.com/chromium/src/+/master/docs/clang.md for
details).

### Configuring the build

Create an asan build directory by running:
```
gn args out/asan
```

Enter the following build variables in the editor that will pop up:
```
is_asan = true
is_debug = false  # Release build.
```

Build with:
```
ninja -C out/asan base_unittests
```

### Goma build

ASan builds should work seamlessly with Goma (except for Windows); just add
`use_goma=1` to your `GYP_DEFINES` or `use_goma=true` in your "gn args" Don't
forget to use ninja -j <jobs> to take advantage of goma.

### Build options

If you want your stack traces to be precise, you will have to disable inlining
by setting the GN arg:
```
enable_full_stack_frames_for_profiling = true
```

Note that this incurs a significant performance hit. Please do not do this on
buildbots.

If you're working on reproducing ClusterFuzz reports, you might want to add:
```
v8_enable_verify_heap = true
```

in order to enable the --verify-heap command line flag for v8 in Release builds.

## Verify the ASan tool works

**ATTENTION (Linux only)**: These instructions are for running ASan in a way
that is compatible with the sandbox. However, this is not compatible with
LeakSanitizer. If you want to debug memory leaks, please use the instructions on
the
[LeakSanitizer](https://sites.google.com/a/chromium.org/dev/developers/testing/leaksanitizer)
page instead.

Now, check that the tool works. Run the following:
```
out/asan/base_unittests
--gtest_filter=ToolsSanityTest.DISABLED_AddressSanitizerLocalOOBCrashTest
--gtest_also_run_disabled_tests 2>&1 | tools/valgrind/asan/asan_symbolize.py
```

The test will crash with the following error report:
```
==26552== ERROR: AddressSanitizer stack-buffer-overflow on address 0x7fff338adb14 at pc 0xac20a7 bp 0x7fff338adad0 sp 0x7fff338adac8
WRITE of size 4 at 0x7fff338adb14 thread T0
    #0 0xac20a7 in base::ToolsSanityTest_DISABLED_AddressSanitizerLocalOOBCrashTest_Test::TestBody() ???:0
    #1 0xcddbd6 in testing::Test::Run() testing/gtest/src/gtest.cc:2161
    #2 0xcdf63b in testing::TestInfo::Run() testing/gtest/src/gtest.cc:2338
... lots more stuff
Address 0x7fff338adb14 is located at offset 52 in frame <base::ToolsSanityTest_DISABLED_AddressSanitizerLocalOOBCrashTest_Test::TestBody()> of T0's stack:
  This frame has 2 object(s):
    [32, 52) 'array'
    [96, 104) 'access'
==26552== ABORTING
... lots more stuff
```

Congrats, you have a working ASan build! 🙌

## Run chrome under ASan

And finally, have fun with the `out/Release/chrome binary`. The filter script
`tools/valgrind/asan/asan_symbolize.py` should be used to symbolize the output.
(Note that `asan_symbolize.py` is absolutely necessary if you need the symbols -
there is no built-in symbolizer for ASan in Chrome).

ASan should perfectly work with Chrome's sandbox. You should only need to run
with `--no-sandbox` on Linux if you're debugging ASan.
Note: you have to disable the sandbox on Windows until it is supported.

You may need to run with `--disable-gpu` on Linux with NVIDIA driver older than
295.20.

You will likely need to define environment variable
[`G_SLICE=always-malloc`](https://developer.gnome.org/glib/unstable/glib-running.html)
to avoid crashes inside gtk.
NSS_DISABLE_ARENA_FREE_LIST=1 and NSS_DISABLE_UNLOAD=1 are required as well.

When filing a bug found by AddressSanitizer, please add a label
`Stability-AddressSanitizer`.

## ASan runtime options

ASan's behavior can be changed by exporting the `ASAN_OPTIONS` env var. Some of
the useful options are listed on this page, others can be obtained from running
an ASanified binary with `ASAN_OPTIONS=help=1`. Note that Chromium sets its own
defaults for some options, so the default behavior may be different from that
observed in other projects.
See `base/debug/sanitizer_options.cc` for more details.

## NaCl support under ASan

On Linux (and soon on Mac) you can build and run Chromium with NaCl under ASan.
Untrusted code (nexe) itself is not instrumented with ASan in this mode, but
everything else is.

To do this, remove `disable_nacl=1` from `GYP_DEFINES`, and define
`NACL_DANGEROUS_SKIP_QUALIFICATION_TEST=1` in your environment at run time.

Pipe chromium output (stderr) through ``tools/valgrind/asan/asan_symbolize.py
`pwd`/`` to get function names and line numbers in ASan reports.
If you're seeing crashes within `nacl_helper_bootstrap`, try deleting `out/Release/nacl_helper`.

## Building on iOS

It's possible to build and run Chrome tests for iOS simulator (which are x86
binaries essentially) under ASan. Note that you'll need a Chrome iOS checkout
for that. It isn't currently possible to build iOS binaries targeting ARM.

Configure your build with `is_asan = true` as described above. Replace your
build directory as needed:
```
ninja -C out/Release-iphonesimulator base_unittests
out/Release-iphonesimulator/iossim -d "iPhone" -s 7.0 out/Release-iphonesimulator/base_unittests.app/ \
--gtest_filter=ToolsSanityTest.DISABLED_AddressSanitizerLocalOOBCrashTest --gtest_also_run_disabled_tests 2>&1 |
tools/valgrind/asan/asan_symbolize.py
```

You'll see the same report as shown above (see the "Verify the ASan tool works"
section), with a number of iOS-specific frames.

## Building on Android

Follow [AndroidBuildInstructions](android_build_instructions.md) with minor
changes:

    target_os="android"
    is_clang=true
    is_asan=true
    is_debug=false

Running ASan applications on Android requires additional device setup. Chromium
testing scripts take care of this, so testing works as expected:
```
build/android/test_runner.py instrumentation --test-apk ContentShellTest
--test_data content:content/test/data/android/device_files -v -v -v --tool=asan
--release
```

To run stuff without Chromium testing script (ex. ContentShell.apk, or any third
party apk or binary), device setup is needed:
```
tools/android/asan/third_party/asan_device_setup.sh --lib
third_party/llvm-build/Release+Asserts/lib/clang/*/lib/linux/libclang_rt.asan-arm-android.so
# wait a few seconds for the device to reload
```

It only needs to be run once per device. It is safe to run it multiple times.
When this is done, the device will run ASan apks as well as normal apks without
any further setup.

To run command-line tools (i.e. binaries), prefix them with `asanwrapper`:
```
adb shell /system/bin/asanwrapper /path/to/binary
```

Use `build/android/asan_symbolize.py` to symbolize stack from `adb logcat`. It needs the `--output-directory` argument and takes care of translating the device path to the unstripped binary in the output directory.

## Building with v8_target_arch=arm

This is needed to detect addressability bugs in the ARM code emitted by V8 and
running on an instrumented ARM emulator in a 32-bit x86 Linux Chromium. **You
probably don't want this, and these instructions have bitrotted because they
still reference GYP. If you do this successfully, please update!** See
[http://crbug.com/324207](https://bugs.chromium.org/p/chromium/issues/detail?id=324207)
for some context.

First, you need to install the 32-bit chroot environment using the
`build/install-chroot.sh` script (as described in
https://code.google.com/p/chromium/wiki/LinuxBuild32On64). Second, install the
build deps:
```
precise32 build/install-build-deps.sh  # assuming your schroot wrapper is called 'precise32'
```

You'll need to make two symlinks to avoid linking errors:
```
sudo ln -s $CHROOT/usr/lib/i386-linux-gnu/libc_nonshared.a /usr/lib/i386-linux-gnu/libc_nonshared.a
sudo ln -s $CHROOT/usr/lib/i386-linux-gnu/libpthread_nonshared.a /usr/lib/i386-linux-gnu/libpthread_nonshared.a
```

Now configure and build your Chrome:
```
GYP_GENERATOR_FLAGS="output_dir=out_asan_chroot" GYP_DEFINES="asan=1 disable_nacl=1 v8_target_arch=arm sysroot=/var/lib/chroot/precise32bit/ chroot_cmd=precise32 host_arch=x86_64 target_arch=ia32" gclient runhooks
ninja -C out_asan_chroot/Release chrome
```

**Note**: `disable_nacl=1` is needed for now.

## AsanCoverage

AsanCoverage is a minimalistic code coverage implementation built into ASan. For
general information see
[https://code.google.com/p/address-sanitizer/wiki/AsanCoverage](https://github.com/google/sanitizers)
To use AsanCoverage in Chromium, add `use_sanitizer_coverage = true` to your GN
args. See also the `sanitizer_coverage_flags` variable for configuring it.

Chrome must be terminated gracefully in order for coverage to work. Either close
the browser, or SIGTERM the browser process. Do not do `killall chrome` or send
SIGKILL.
```
$ kill <browser_process_pid>
$ ls
...
chrome.22575.sancov
gpu.6916123572022919124.sancov.packed
zygote.13651804083035800069.sancov.packed
...
```

The `gpu.*.sancov.packed` file contains coverage data for the GPU process,
whereas the `zygote.*.sancov.packed` file contains coverage data for the
renderers (but not the zygote process). Unpack them to regular `.sancov` files
like so:
```
$ $LLVM/projects/compiler-rt/lib/sanitizer_common/scripts/sancov.py unpack *.sancov.packed
sancov.py: unpacking gpu.6916123572022919124.sancov.packed
sancov.py: extracting chrome.22610.sancov
sancov.py: unpacking zygote.13651804083035800069.sancov.packed
sancov.py: extracting libpdf.so.12.sancov
sancov.py: extracting chrome.12.sancov
sancov.py: extracting libpdf.so.10.sancov
sancov.py: extracting chrome.10.sancov
```

Now, e.g., to list the offsets of covered functions in the libpdf.so binary in
renderer with pid 10:
```
$ $LLVM/projects/compiler-rt/lib/sanitizer_common/scripts/sancov.py print libpdf.so.10.sancov
```

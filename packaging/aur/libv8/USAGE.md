# libv8 AUR package

This package contains a pinned, static V8 16.0.94 embedder build. It is not a
drop-in replacement for Node.js and is not a shared `libv8.so`.

## Linking an embedder

The `v8.pc` file exports a linker script (`libv8_all.ld`) that groups the
monolithic library with the Rust/ICU static libraries and compiler builtins
needed by this V8 revision:

```sh
clang++ -std=c++20 -DV8_COMPRESS_POINTERS \
  -DV8_COMPRESS_POINTERS_IN_SHARED_CAGE -DV8_31BIT_SMIS_ON_64BIT_ARCH \
  $(pkg-config --cflags v8) hello-world.cc \
  $(pkg-config --libs v8) -fuse-ld=lld -o hello
```

`-fuse-ld=lld` is required: V8 16 emits experimental CREL relocations that the
default GNU linker cannot read. The include convention is the V8 source one,
so headers are reached as `include/v8.h`, `include/libplatform/...` through
`-I/usr/lib/libv8`.

Copy `/usr/lib/libv8/icudtl.dat` next to your executable (or load it explicitly
with `v8::V8::InitializeICU`) when using i18n.

## Scope and security

The build in this package disables V8's in-process sandbox (the pinned default
needs hardened libc++) and the Chromium-specific clang plugins. Treat this
library as a computation engine inside a separately confined worker process;
do not run untrusted JavaScript in a privileged host. It is optional for
Tompouce — the default QuickJS backend does not use it.

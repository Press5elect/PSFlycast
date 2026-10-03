# PSFlyCast: cross-compile with the payload SDK PS5_PAYLOAD_SDK names
# (shell/ps5/build.sh sets it; see the README's Building section).
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang")
set(CMAKE_CXX_COMPILER "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang++")
set(CMAKE_AR "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ar")
set(CMAKE_RANLIB "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ranlib")
set(CMAKE_FIND_ROOT_PATH "$ENV{PS5_PAYLOAD_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(PKG_CONFIG_EXECUTABLE "/bin/false" CACHE FILEPATH "" FORCE)

set(FLYCAST_PS5 ON CACHE BOOL "" FORCE)

# Link-only probes resolve against the console's libraries; cross CMake never
# runs them.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -nostartfiles -nodefaultlibs -Wl,-e,0 -lkernel_web -lSceLibcInternal -lScePosixForWebKit")
# The console's processor is a Zen 2: compiled for it, the disc image
# decoders (zstd, LZMA, FLAC) and the texture hash take their AVX2 and BMI2
# paths, and loops are vectorised eight wide. -ffp-contract=off keeps
# floating point as it was: no multiply and add is fused into one FMA, so
# the emulator computes what it computes on every other x86-64 build.
set(PS5_CPU_FLAGS "-march=znver2 -ffp-contract=off")
set(CMAKE_C_FLAGS_INIT "-O2 ${PS5_CPU_FLAGS} -fPIC -w -Dstatic_assert=_Static_assert -DZSTD_TRACE=0")
set(CMAKE_CXX_FLAGS_INIT "-O2 ${PS5_CPU_FLAGS} -fPIC -w -DZSTD_TRACE=0")

# libzip's link probes find the C11 Annex K functions in the console's libc,
# which the SDK's headers do not declare; libzip's fallbacks are used instead.
foreach(probe HAVE_MEMCPY_S HAVE_STRNCPY_S HAVE_STRERROR_S HAVE_STRERRORLEN_S)
    set(${probe} "" CACHE INTERNAL "")
endforeach()

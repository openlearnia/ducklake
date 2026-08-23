# Release-only Linux triplet used by the Blacksmith bundle build.
#
# Declare the host OS so vcpkg selects its Unix ports (not the Windows
# fallbacks), while explicitly providing the native timezone compiler that
# PostgreSQL's configure probes when vcpkg reports a Linux system toolchain.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
set(ENV{ZIC} /usr/bin/zic)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/native-linux-toolchain.cmake")

# Release-only Linux triplet used by the Blacksmith bundle build.
#
# Do not set VCPKG_CMAKE_SYSTEM_NAME here: this is a native x86_64 Linux
# build.  Marking it as a cross build makes PostgreSQL's libpq configure
# require a target-side zic/timezone tool and breaks the vcpkg libpq port.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

# The Blacksmith image is already a native Linux build host. This chainload
# file exists to satisfy vcpkg's non-Windows triplet validation without
# declaring a foreign system and triggering PostgreSQL cross-build checks.
set(CMAKE_C_COMPILER gcc CACHE FILEPATH "Native C compiler")
set(CMAKE_CXX_COMPILER g++ CACHE FILEPATH "Native C++ compiler")

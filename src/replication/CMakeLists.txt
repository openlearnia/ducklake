add_library(ducklake_replication OBJECT ducklake_replication.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_replication>
    PARENT_SCOPE)

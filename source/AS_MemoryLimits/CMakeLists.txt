set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${GRAFT_OUT}/graft/grafted")
graft_plugin(as_memory_limits NAME AS_MemoryLimits VERSION 4 SOURCES plugin.cpp MemoryLimits.cpp)
target_compile_definitions(as_memory_limits PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(as_memory_limits PRIVATE /utf-8)
target_link_libraries(as_memory_limits PRIVATE bcrypt)

set(data "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
file(MAKE_DIRECTORY "${data}")
if(NOT EXISTS "${data}/MemoryLimits.ini")
    configure_file(MemoryLimits.ini "${data}/MemoryLimits.ini" COPYONLY)
endif()

add_executable(as_memory_limits_selftest EXCLUDE_FROM_ALL tests/selftest.cpp MemoryLimits.cpp)
target_compile_definitions(as_memory_limits_selftest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(as_memory_limits_selftest PRIVATE /utf-8 /UNDEBUG)
target_link_libraries(as_memory_limits_selftest PRIVATE bcrypt)
set_target_properties(as_memory_limits_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${GRAFT_OUT}/tests/as_memory_limits")
add_custom_target(as_memory_limits_checks
    COMMAND "$<TARGET_FILE:as_memory_limits_selftest>"
    DEPENDS as_memory_limits_selftest as_memory_limits)

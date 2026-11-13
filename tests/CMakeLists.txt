add_executable(indexed_memory_generator indexed_memory_generator.cpp)
target_link_libraries(indexed_memory_generator PRIVATE nwiirecomp_lib)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/indexed_memory.inc"
    COMMAND indexed_memory_generator "${CMAKE_CURRENT_BINARY_DIR}/indexed_memory.inc"
    DEPENDS indexed_memory_generator
    VERBATIM)
add_executable(indexed_memory_test indexed_memory.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/indexed_memory.inc")
target_include_directories(indexed_memory_test PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/nWiiRuntime/include")
# MMU inline slow paths reference hardware functions even though the fixtures
# access only RAM. Resolve them from the runtime.
target_link_libraries(indexed_memory_test PRIVATE nwiiruntime)
add_test(NAME indexed_memory COMMAND indexed_memory_test)

add_executable(ipc_interrupt_test ipc_interrupt.cpp)
target_link_libraries(ipc_interrupt_test PRIVATE nwiiruntime)
add_test(NAME ipc_interrupt COMMAND ipc_interrupt_test)

add_executable(write_gather_test write_gather.cpp)
target_link_libraries(write_gather_test PRIVATE nwiiruntime)
add_test(NAME write_gather COMMAND write_gather_test)

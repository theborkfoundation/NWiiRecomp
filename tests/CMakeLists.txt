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

add_executable(translator_edge_test translator_edge_test.cpp)
target_link_libraries(translator_edge_test PRIVATE nwiiruntime)
add_test(NAME translator_edge COMMAND translator_edge_test)

add_executable(recompiler_resume_generator recompiler_resume_generator.cpp)
target_link_libraries(recompiler_resume_generator PRIVATE nwiirecomp_lib)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/recompiler_resume.inc"
    COMMAND recompiler_resume_generator "${CMAKE_CURRENT_BINARY_DIR}/recompiler_resume.inc"
    DEPENDS recompiler_resume_generator
    VERBATIM)
add_executable(recompiler_resume_test recompiler_resume_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/recompiler_resume.inc")
target_include_directories(recompiler_resume_test PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/nWiiRuntime/include")
target_link_libraries(recompiler_resume_test PRIVATE nwiiruntime)
# Compile the generated body optimized: block layout and therefore the sign of
# the resume-table offsets differ from an unoptimized build.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(recompiler_resume_test PRIVATE -O2)
endif()
add_test(NAME recompiler_resume COMMAND recompiler_resume_test)

add_executable(recompiler_cmake_test recompiler_cmake_test.cpp)
target_link_libraries(recompiler_cmake_test PRIVATE nwiirecomp_lib)
add_test(NAME recompiler_cmake COMMAND recompiler_cmake_test)

add_executable(config_backend_test config_backend_test.cpp)
target_link_libraries(config_backend_test PRIVATE nwiiruntime)
add_test(NAME config_backend COMMAND config_backend_test)

add_executable(shader_dialect_test shader_dialect_test.cpp)
target_link_libraries(shader_dialect_test PRIVATE nwiiruntime)
add_test(NAME shader_dialect COMMAND shader_dialect_test)

add_executable(device_bounds_test device_bounds_test.cpp)
target_link_libraries(device_bounds_test PRIVATE nwiiruntime)
add_test(NAME device_bounds COMMAND device_bounds_test)

add_executable(translator_semantics_generator translator_semantics_generator.cpp)
target_link_libraries(translator_semantics_generator PRIVATE nwiirecomp_lib)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/translator_semantics.inc"
    COMMAND translator_semantics_generator "${CMAKE_CURRENT_BINARY_DIR}/translator_semantics.inc"
    DEPENDS translator_semantics_generator
    VERBATIM)
add_executable(translator_semantics_test translator_semantics_test.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/translator_semantics.inc")
target_include_directories(translator_semantics_test PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/nWiiRuntime/include")
target_link_libraries(translator_semantics_test PRIVATE nwiiruntime)
add_test(NAME translator_semantics COMMAND translator_semantics_test)

add_executable(context_reschedule_test context_reschedule.cpp)
target_link_libraries(context_reschedule_test PRIVATE nwiiruntime)
add_test(NAME context_reschedule COMMAND context_reschedule_test)

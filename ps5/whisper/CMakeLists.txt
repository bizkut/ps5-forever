cmake_minimum_required(VERSION 3.20)
project(forever_whisper LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_CPU ON CACHE BOOL "" FORCE)
set(GGML_VULKAN ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_BLAS OFF CACHE BOOL "" FORCE)
set(GGML_LLAMAFILE OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
foreach(feature AVX AVX2 FMA F16C)
    set(GGML_${feature} ON CACHE BOOL "" FORCE)
endforeach()
foreach(feature BMI2 AVX_VNNI AVX512 AVX512_VBMI AVX512_VNNI AVX512_BF16 AMX_TILE AMX_INT8 AMX_BF16)
    set(GGML_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
add_compile_definitions(GGML_MAX_NAME=160)
# PS5's math functions come from the title's libc imports, not a host libm.
set(MATH_LIBRARY "")
add_subdirectory("${GGML_SOURCE_DIR}" ggml-build)
# Do not add /usr/include to a target build just to find SPIR-V headers.
target_include_directories(ggml-vulkan PRIVATE "${CMAKE_BINARY_DIR}/include-shim")
# whisper's bundled GGML is deliberately not built: all backends use one pin.
add_library(whisper STATIC
    "${WHISPER_SOURCE_DIR}/src/whisper.cpp"
    "${WHISPER_SOURCE_DIR}/src/aneforge/whisper-aneforge.cpp")
target_include_directories(whisper PUBLIC "${WHISPER_SOURCE_DIR}/include"
    PRIVATE "${WHISPER_SOURCE_DIR}/src")
target_compile_definitions(whisper PRIVATE WHISPER_VERSION="1.9.5")
target_link_libraries(whisper PUBLIC ggml)
install(TARGETS whisper ARCHIVE DESTINATION lib)
install(FILES "${WHISPER_SOURCE_DIR}/include/whisper.h" DESTINATION include)
install(DIRECTORY "${GGML_SOURCE_DIR}/include/" DESTINATION include
    FILES_MATCHING PATTERN "*.h")

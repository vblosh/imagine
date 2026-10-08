cmake_minimum_required(VERSION 3.20)

project(imagine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Face analysis stays optional so existing installations do not need ONNX Runtime
# or locally supplied model weights.
option(IMAGINE_ENABLE_FACE_ANALYSIS "Build local InsightFace inference with ONNX Runtime 1.30.0" OFF)
set(ORT_ROOT "$ENV{ORT_ROOT}" CACHE PATH "ONNX Runtime 1.30.0 SDK root (contains include/ and lib/)")
if (IMAGINE_ENABLE_FACE_ANALYSIS)
    if (NOT ORT_ROOT)
        message(FATAL_ERROR "IMAGINE_ENABLE_FACE_ANALYSIS requires -DORT_ROOT=<ONNX Runtime 1.30.0 SDK root>")
    endif()
    # find_path/find_library/find_file cache successful results. Invalidate them
    # when switching from the CPU SDK to the CUDA SDK (or the other way around)
    # in an existing build directory so imported paths and copied DLLs follow ORT_ROOT.
    if (DEFINED IMAGINE_ORT_RESOLVED_ROOT AND NOT "${IMAGINE_ORT_RESOLVED_ROOT}" STREQUAL "${ORT_ROOT}")
        unset(IMAGINE_ORT_INCLUDE_DIR CACHE)
        unset(IMAGINE_ORT_IMPORT_LIBRARY CACHE)
        unset(IMAGINE_ORT_RUNTIME_DLL CACHE)
        unset(IMAGINE_ORT_INCLUDE_DIR)
        unset(IMAGINE_ORT_IMPORT_LIBRARY)
        unset(IMAGINE_ORT_RUNTIME_DLL)
    endif()
    find_path(IMAGINE_ORT_INCLUDE_DIR onnxruntime_cxx_api.h PATHS "${ORT_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(IMAGINE_ORT_IMPORT_LIBRARY NAMES onnxruntime PATHS "${ORT_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    file(STRINGS "${IMAGINE_ORT_INCLUDE_DIR}/onnxruntime_c_api.h" IMAGINE_ORT_API_VERSION_LINE
         REGEX "^[ \t]*#define[ \t]+ORT_API_VERSION[ \t]+[0-9]+")
    if (NOT IMAGINE_ORT_API_VERSION_LINE MATCHES "ORT_API_VERSION[ \t]+30([ \t]|$)")
        message(FATAL_ERROR "ORT_ROOT must point to the ONNX Runtime 1.30.0 SDK (API version 30)")
    endif()

    if (WIN32)
        find_file(IMAGINE_ORT_RUNTIME_DLL NAMES onnxruntime.dll
                  PATHS "${ORT_ROOT}/lib" "${ORT_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
        add_library(onnxruntime SHARED IMPORTED GLOBAL)
        set_target_properties(onnxruntime PROPERTIES
            IMPORTED_IMPLIB "${IMAGINE_ORT_IMPORT_LIBRARY}"
            IMPORTED_LOCATION "${IMAGINE_ORT_RUNTIME_DLL}"
            INTERFACE_INCLUDE_DIRECTORIES "${IMAGINE_ORT_INCLUDE_DIR}"
        )
        file(GLOB IMAGINE_ORT_RUNTIME_DLLS CONFIGURE_DEPENDS "${ORT_ROOT}/lib/*.dll" "${ORT_ROOT}/bin/*.dll")
    else()
        add_library(onnxruntime UNKNOWN IMPORTED GLOBAL)
        set_target_properties(onnxruntime PROPERTIES
            IMPORTED_LOCATION "${IMAGINE_ORT_IMPORT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IMAGINE_ORT_INCLUDE_DIR}"
        )
        set(IMAGINE_ORT_RUNTIME_DLLS "")
    endif()
    set(IMAGINE_ORT_RESOLVED_ROOT "${ORT_ROOT}" CACHE INTERNAL "SDK root used for resolved ONNX Runtime paths" FORCE)
    message(STATUS "Face analysis enabled with ONNX Runtime 1.30.0 from ${ORT_ROOT}")
else()
    message(STATUS "Face analysis unavailable (enable with -DIMAGINE_ENABLE_FACE_ANALYSIS=ON and -DORT_ROOT=...)")
endif()

# Compiler options & warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif (MSVC)
    add_compile_options(/permissive- /utf-8)
endif()

if (WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Native host optimization
option(ENABLE_NATIVE_OPT "Enable -march=native host optimizations" OFF)
if (ENABLE_NATIVE_OPT AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Enabling -march=native host CPU optimizations")
    add_compile_options(-march=native)
endif()

# Sanitizers
option(ENABLE_SANITIZERS "Build with Address and Undefined sanitizers" OFF)
if (ENABLE_SANITIZERS)
    message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Coverage
option(ENABLE_COVERAGE "Build with coverage" OFF)
if (ENABLE_COVERAGE)
    message(STATUS "Enabling gcov test coverage")
    add_compile_options(--coverage)
    add_link_options(--coverage)
endif()

# External Dependencies
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# Core Library
set(IMAGINE_LIB_SOURCES
    src/common/types.cpp
    src/common/logger.cpp
    src/db/connection.cpp
    src/db/schema.cpp
    src/db/catalog_db.cpp
    src/metadata/hasher.cpp
    src/metadata/exif_reader.cpp
    src/metadata/media_reader.cpp
    src/thumbnail/generator.cpp
    src/thumbnail/cache.cpp
    src/faces/engine.cpp
    src/faces/service.cpp
    src/clip/engine.cpp
    src/clip/tokenizer.cpp
    src/clip/service.cpp
    src/concurrency/thread_pool.cpp
    src/core/query.cpp
    src/core/event_suggestions.cpp
    src/core/importer.cpp
    src/core/catalog.cpp
    src/server/api_router.cpp
    src/server/web_server.cpp
)

add_library(imagine_lib STATIC ${IMAGINE_LIB_SOURCES})

target_include_directories(imagine_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(imagine_lib
    PUBLIC
        sqlite3
        stb
        easyexif
        nlohmann_json::nlohmann_json
        httplib::httplib
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        Threads::Threads
        $<$<BOOL:${TURBOJPEG_FOUND}>:turbojpeg_lib>
        $<$<BOOL:${WIN32}>:ws2_32>
        $<$<BOOL:${WIN32}>:crypt32>
)
if (IMAGINE_ENABLE_FACE_ANALYSIS)
    target_link_libraries(imagine_lib PUBLIC onnxruntime)
    target_compile_definitions(imagine_lib PUBLIC IMAGINE_FACE_ANALYSIS_BUILT=1)
else()
    target_compile_definitions(imagine_lib PUBLIC IMAGINE_FACE_ANALYSIS_BUILT=0)
endif()

# CLI Application
add_executable(imagine apps/imagine_cli.cpp)
target_link_libraries(imagine PRIVATE imagine_lib)
if (WIN32)
    target_sources(imagine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.manifest)
endif()

# Standalone C++ example: also built in feature-off configurations so it can
# report a clear unavailable status instead of disappearing from the build.
add_executable(face_inference_example apps/face_inference_example.cpp)
target_link_libraries(face_inference_example PRIVATE imagine_lib)
if (WIN32)
    target_link_libraries(face_inference_example PRIVATE psapi)
endif()

function(imagine_copy_ort_runtime target_name)
    if (WIN32 AND IMAGINE_ENABLE_FACE_ANALYSIS)
        foreach(ort_dll IN LISTS IMAGINE_ORT_RUNTIME_DLLS)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ort_dll}" "$<TARGET_FILE_DIR:${target_name}>"
                VERBATIM)
        endforeach()
    endif()
endfunction()
imagine_copy_ort_runtime(imagine)
imagine_copy_ort_runtime(face_inference_example)

# Tests
include(CTest)
if (BUILD_TESTING)
    enable_testing()
    
    set(IMAGINE_TEST_SOURCES
        tests/test_main.cpp
        tests/common_test.cpp
        tests/concurrency_test.cpp
        tests/catalog_test.cpp
        tests/db_test.cpp
        tests/exif_test.cpp
        tests/thumbnail_test.cpp
        tests/faces_engine_test.cpp
        tests/clip_test.cpp
        tests/importer_test.cpp
        tests/query_test.cpp
        tests/event_suggestions_test.cpp
        tests/server_test.cpp
        tests/media_test.cpp
    )

    add_executable(imagine_tests ${IMAGINE_TEST_SOURCES})
    target_link_libraries(imagine_tests PRIVATE imagine_lib GTest::gtest)
    imagine_copy_ort_runtime(imagine_tests)
    if (WIN32)
        target_sources(imagine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.manifest)
    endif()
    add_test(NAME imagine_unit_tests COMMAND imagine_tests)

    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_FOUND)
        add_test(NAME imagine_ui_tests
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/run_ui_tests.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()
endif()

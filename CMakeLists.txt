cmake_minimum_required(VERSION 3.20)

project(imagine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler options & warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
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
    add_compile_options(-fprofile-arcs -ftest-coverage)
    add_link_options(-lgcov)
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
    src/thumbnail/generator.cpp
    src/thumbnail/cache.cpp
    src/concurrency/thread_pool.cpp
    src/core/query.cpp
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
        OpenSSL::Crypto
        pthread
)

# CLI Application
add_executable(imagine apps/imagine_cli.cpp)
target_link_libraries(imagine PRIVATE imagine_lib)

# Tests
include(CTest)
if (BUILD_TESTING)
    enable_testing()
    
    set(IMAGINE_TEST_SOURCES
        tests/test_main.cpp
        tests/db_test.cpp
        tests/exif_test.cpp
        tests/thumbnail_test.cpp
        tests/importer_test.cpp
        tests/query_test.cpp
        tests/server_test.cpp
    )

    add_executable(imagine_tests ${IMAGINE_TEST_SOURCES})
    target_link_libraries(imagine_tests PRIVATE imagine_lib GTest::gtest)
    add_test(NAME imagine_unit_tests COMMAND imagine_tests)

    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_FOUND)
        add_test(NAME imagine_ui_tests
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/run_ui_tests.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()
endif()

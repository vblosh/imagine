include(FetchContent)

# SQLite 3 amalgamation
FetchContent_Declare(
    sqlite3_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3_amalgamation)

add_library(sqlite3 STATIC "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite3_amalgamation_SOURCE_DIR}")
target_compile_definitions(sqlite3 PUBLIC
    SQLITE_ENABLE_RTREE=1
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_JSON1=1
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)
target_link_libraries(sqlite3 PUBLIC ${CMAKE_DL_LIBS} pthread)

# STB single-header libraries for image loading, resizing, and writing
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${stb_SOURCE_DIR}")

# EasyEXIF parser
FetchContent_Declare(
    easyexif
    GIT_REPOSITORY https://github.com/mayanklahiri/easyexif.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(easyexif)
add_library(easyexif STATIC "${easyexif_SOURCE_DIR}/exif.cpp")
target_include_directories(easyexif PUBLIC "${easyexif_SOURCE_DIR}")

# nlohmann_json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# cpp-httplib
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(httplib)

# OpenSSL for cryptographic SHA-256
find_package(OpenSSL REQUIRED)

# TurboJPEG (libjpeg-turbo) accelerated decoding and compression
option(ENABLE_TURBOJPEG "Enable libjpeg-turbo hardware-accelerated decoding and thumbnailing" ON)
if (ENABLE_TURBOJPEG)
    find_path(TURBOJPEG_INCLUDE_DIR NAMES turbojpeg.h PATHS "$ENV{HOME}/.local/include" /usr/local/include /usr/include)
    find_library(TURBOJPEG_LIBRARY NAMES turbojpeg PATHS "$ENV{HOME}/.local/lib" /usr/local/lib /usr/lib/x86_64-linux-gnu /usr/lib)

    if (TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIBRARY)
        message(STATUS "Found TurboJPEG: ${TURBOJPEG_LIBRARY} (headers: ${TURBOJPEG_INCLUDE_DIR})")
        set(TURBOJPEG_FOUND TRUE)
        add_library(turbojpeg_lib UNKNOWN IMPORTED)
        set_target_properties(turbojpeg_lib PROPERTIES
            IMPORTED_LOCATION "${TURBOJPEG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${TURBOJPEG_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS "IMAGINE_HAS_TURBOJPEG=1"
        )
    else()
        message(STATUS "TurboJPEG not found. Falling back to STB Image.")
        set(TURBOJPEG_FOUND FALSE)
    endif()
endif()

# GoogleTest
find_package(GTest QUIET)
if (NOT GTest_FOUND)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

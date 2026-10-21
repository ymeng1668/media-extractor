cmake_minimum_required(VERSION 3.16)
project(media_extractor VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
option(MEDIA_EXTRACTOR_BUILD_APP "Build the media_extractor command-line tool" ON)
set(MEDIA_EXTRACTOR_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin" CACHE PATH "Extractor runtime output directory")
set(MEDIA_FORMAT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/media-format-core"
    CACHE PATH "media-format-core source checkout (defaults to pinned submodule)")
set(MEDIA_FORMAT_RUNTIME_DIR "${MEDIA_EXTRACTOR_RUNTIME_DIR}" CACHE PATH "Media shared-library output directory")
if(NOT EXISTS "${MEDIA_FORMAT_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Missing media-format-core submodule. Run git submodule update --init --recursive")
endif()
add_subdirectory("${MEDIA_FORMAT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/media-format")

if(MEDIA_EXTRACTOR_BUILD_APP)
    add_executable(media_extractor app/main.cpp)
    target_link_libraries(media_extractor PRIVATE media-format::core)
    set(MEDIA_EXTRACTOR_PRIVACY_PLUGIN_DIR "${MEDIA_FORMAT_SOURCE_DIR}/plugin/python/ai/privacy-redaction"
        CACHE PATH "Privacy redaction Python plugin directory")
    set(_privacy_python "")
    if(MEDIA_FORMAT_ENABLE_PYTHON_BRIDGE)
        find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
        set(_privacy_python "${Python3_EXECUTABLE}")
    endif()
    target_compile_definitions(media_extractor PRIVATE
        MEDIA_EXTRACTOR_PRIVACY_PLUGIN_DIR="${MEDIA_EXTRACTOR_PRIVACY_PLUGIN_DIR}"
        MEDIA_EXTRACTOR_PYTHON_RUNTIME_DIR="${MEDIA_FORMAT_SOURCE_DIR}/plugin/python"
        MEDIA_EXTRACTOR_PRIVACY_PYTHON="${_privacy_python}")
    find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h REQUIRED)
    target_include_directories(media_extractor PRIVATE "${RAPIDJSON_INCLUDE_DIR}")
    set_target_properties(media_extractor PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        RUNTIME_OUTPUT_DIRECTORY "${MEDIA_EXTRACTOR_RUNTIME_DIR}")
    foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
        set_target_properties(media_extractor PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${MEDIA_EXTRACTOR_RUNTIME_DIR}")
    endforeach()
    if(APPLE)
        set_property(TARGET media_extractor PROPERTY INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_property(TARGET media_extractor PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
    install(TARGETS media_extractor RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING AND MEDIA_EXTRACTOR_BUILD_APP)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(MEDIA_EXTRACTOR_TEST_FFMPEG ffmpeg)
    find_program(MEDIA_EXTRACTOR_TEST_FFPROBE ffprobe)
    if(Python3_Interpreter_FOUND AND MEDIA_EXTRACTOR_TEST_FFMPEG)
        if(MEDIA_FORMAT_ENABLE_PYTHON_BRIDGE AND MEDIA_EXTRACTOR_TEST_FFPROBE)
            add_test(NAME media_extractor_python_privacy_cli COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/python_privacy_cli_test.py"
                "$<TARGET_FILE:media_extractor>" "${MEDIA_EXTRACTOR_TEST_FFMPEG}"
                "${MEDIA_EXTRACTOR_TEST_FFPROBE}" "${MEDIA_EXTRACTOR_PRIVACY_PLUGIN_DIR}")
            set_tests_properties(media_extractor_python_privacy_cli PROPERTIES
                SKIP_RETURN_CODE 77 TIMEOUT 120 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        endif()
        foreach(_test media_cli privacy_timing_cli)
            add_test(NAME media_extractor_${_test} COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_test}_test.py"
                "$<TARGET_FILE:media_extractor>" "${MEDIA_EXTRACTOR_TEST_FFMPEG}")
            set_tests_properties(media_extractor_${_test} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        endforeach()
        if(UNIX AND NOT APPLE)
            foreach(sdk_name rmAIResultParse rmai_info_parse)
                add_library(media_extractor_fixture_${sdk_name} SHARED tests/privacy_sdk_fixture.cpp)
                set_target_properties(media_extractor_fixture_${sdk_name} PROPERTIES
                    OUTPUT_NAME ${sdk_name}
                    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/privacy_sdk_fixture")
            endforeach()
            add_test(NAME media_extractor_privacy_combined_cli COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy_timing_cli_test.py"
                "$<TARGET_FILE:media_extractor>" "${MEDIA_EXTRACTOR_TEST_FFMPEG}"
                "$<TARGET_FILE_DIR:media_extractor_fixture_rmai_info_parse>")
            set_tests_properties(media_extractor_privacy_combined_cli PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        endif()
    endif()
endif()

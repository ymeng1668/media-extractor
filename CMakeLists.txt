cmake_minimum_required(VERSION 3.16)
project(media_extractor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h)
if(NOT RAPIDJSON_INCLUDE_DIR)
    message(FATAL_ERROR "RapidJSON headers not found. Install rapidjson and ensure rapidjson/document.h is on the include path.")
endif()

add_library(media_extractor_core
    src/media_frame.cpp
    src/forward_privacy.cpp
    src/metadata_timing.cpp
    src/privacy_render.cpp
    src/mp4_metadata.cpp
    third_party/AdpcmCodec.cpp
)
target_include_directories(media_extractor_core PUBLIC src)
target_include_directories(media_extractor_core PRIVATE "${RAPIDJSON_INCLUDE_DIR}")
target_link_libraries(media_extractor_core PRIVATE ${CMAKE_DL_LIBS})
include(cmake/prepare_ai_sdk.cmake)

add_executable(media_extractor
    app/main.cpp
)
target_link_libraries(media_extractor PRIVATE media_extractor_core)
target_include_directories(media_extractor PRIVATE "${RAPIDJSON_INCLUDE_DIR}")

set_target_properties(media_extractor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    set_target_properties(media_extractor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_SOURCE_DIR}/bin"
    )
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(forward_privacy_test tests/forward_privacy_test.cpp)
    target_link_libraries(forward_privacy_test PRIVATE media_extractor_core)
    add_test(NAME forward_privacy COMMAND forward_privacy_test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(TEST_FFMPEG ffmpeg)
    if(Python3_Interpreter_FOUND AND TEST_FFMPEG)
        add_test(NAME media_cli COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/media_cli_test.py"
            "$<TARGET_FILE:media_extractor>" "${TEST_FFMPEG}")
        set_tests_properties(media_cli PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        add_test(NAME privacy_timing_cli COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy_timing_cli_test.py"
            "$<TARGET_FILE:media_extractor>" "${TEST_FFMPEG}")
        set_tests_properties(privacy_timing_cli PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        if(UNIX AND NOT APPLE)
            foreach(sdk_name rmAIResultParse rmai_info_parse)
                add_library(privacy_fixture_${sdk_name} SHARED tests/privacy_sdk_fixture.cpp)
                set_target_properties(privacy_fixture_${sdk_name} PROPERTIES
                    OUTPUT_NAME ${sdk_name}
                    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/privacy_sdk_fixture")
            endforeach()
            add_test(NAME privacy_combined_cli COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/privacy_timing_cli_test.py"
                "$<TARGET_FILE:media_extractor>" "${TEST_FFMPEG}"
                "$<TARGET_FILE_DIR:privacy_fixture_rmai_info_parse>")
            set_tests_properties(privacy_combined_cli PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        endif()
    endif()
endif()

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
)
target_include_directories(media_extractor_core PUBLIC src)

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

cmake_minimum_required(VERSION 3.25)

project(arec_data_plane VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(arec_protocol
    data_plane/src/crypto.cpp
    data_plane/src/frame_codec.cpp
    data_plane/src/token.cpp
    data_plane/src/types.cpp
)

target_include_directories(arec_protocol PUBLIC data_plane/include)
target_link_libraries(arec_protocol
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        bcrypt
)

if(MSVC)
    target_compile_options(arec_protocol PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(arec_protocol PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
endif()

add_library(arec_data_plane_core
    data_plane/src/data_plane_client.cpp
    data_plane/src/execution_engine.cpp
)

target_link_libraries(arec_data_plane_core
    PUBLIC
        arec_protocol
        asio::asio
    PRIVATE
        ws2_32
        bcrypt
)

if(MSVC)
    target_compile_options(arec_data_plane_core PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(arec-data-plane data_plane/src/main.cpp)
target_link_libraries(arec-data-plane PRIVATE arec_data_plane_core)

if(MSVC)
    target_compile_options(arec-data-plane PRIVATE /W4 /permissive- /utf-8)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(arec-unit-tests data_plane/tests/unit_tests.cpp)
    target_link_libraries(arec-unit-tests PRIVATE arec_data_plane_core)
    if(MSVC)
        target_compile_options(arec-unit-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME arec-unit-tests COMMAND arec-unit-tests)

    add_executable(arec-integration-tests data_plane/tests/integration_tests.cpp)
    target_link_libraries(arec-integration-tests PRIVATE arec_data_plane_core)
    if(MSVC)
        target_compile_options(arec-integration-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME arec-integration-tests COMMAND arec-integration-tests)
    set_tests_properties(arec-integration-tests PROPERTIES TIMEOUT 30)
endif()

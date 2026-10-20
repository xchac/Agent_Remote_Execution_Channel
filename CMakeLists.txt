cmake_minimum_required(VERSION 3.25)

project(arec VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json CONFIG REQUIRED)

find_path(ASIO_INCLUDE_DIR asio.hpp)
if(NOT ASIO_INCLUDE_DIR)
  message(FATAL_ERROR "standalone Asio headers not found; install with: brew install asio")
endif()
add_library(asio::asio INTERFACE IMPORTED)
set_target_properties(asio::asio PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${ASIO_INCLUDE_DIR}"
  INTERFACE_COMPILE_DEFINITIONS ASIO_STANDALONE
)

add_subdirectory(data_plane)


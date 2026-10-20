find_package(Threads REQUIRED)

add_library(arec-data-plane-core STATIC
  src/cache.cpp
  src/data_plane.cpp
  src/frame.cpp
  src/protocol.cpp
  src/token.cpp
  src/tools.cpp
)
target_include_directories(arec-data-plane-core PUBLIC include)
target_link_libraries(arec-data-plane-core
  PUBLIC asio::asio nlohmann_json::nlohmann_json Threads::Threads
)

add_executable(arec-data-plane src/main.cpp)
target_link_libraries(arec-data-plane PRIVATE arec-data-plane-core)

if(BUILD_TESTING)
  add_executable(arec-data-plane-tests tests/data_plane_tests.cpp)
  target_link_libraries(arec-data-plane-tests PRIVATE arec-data-plane-core)
  add_test(NAME arec-data-plane-tests COMMAND arec-data-plane-tests)
endif()


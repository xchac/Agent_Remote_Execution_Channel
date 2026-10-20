add_executable(arec-data-plane src/main.cpp)
target_link_libraries(arec-data-plane PRIVATE asio::asio nlohmann_json::nlohmann_json)

if(BUILD_TESTING)
  add_executable(arec-data-plane-tests tests/scaffold_test.cpp)
  target_link_libraries(arec-data-plane-tests PRIVATE asio::asio nlohmann_json::nlohmann_json)
  add_test(NAME arec-data-plane-tests COMMAND arec-data-plane-tests)
endif()


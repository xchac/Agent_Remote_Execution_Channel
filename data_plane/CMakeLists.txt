if(APPLE)
  add_subdirectory(mac)
elseif(WIN32)
  add_library(arec_protocol
      src/crypto.cpp
      src/frame_codec.cpp
      src/token.cpp
      src/types.cpp
  )
  target_include_directories(arec_protocol PUBLIC include)
  target_link_libraries(arec_protocol
      PUBLIC nlohmann_json::nlohmann_json
      PRIVATE bcrypt
  )
  target_compile_options(arec_protocol PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(arec_protocol
      PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
  )

  add_library(arec_data_plane_core
      src/data_plane_client.cpp
      src/execution_engine.cpp
  )
  target_link_libraries(arec_data_plane_core
      PUBLIC arec_protocol asio::asio
      PRIVATE ws2_32 bcrypt
  )
  target_compile_options(arec_data_plane_core PRIVATE /W4 /permissive- /utf-8)

  add_executable(arec-data-plane src/main.cpp)
  target_link_libraries(arec-data-plane PRIVATE arec_data_plane_core)
  target_compile_options(arec-data-plane PRIVATE /W4 /permissive- /utf-8)

  if(BUILD_TESTING)
    add_executable(arec-unit-tests tests/unit_tests.cpp)
    target_link_libraries(arec-unit-tests PRIVATE arec_data_plane_core)
    target_compile_options(arec-unit-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME arec-unit-tests COMMAND arec-unit-tests)

    add_executable(arec-integration-tests tests/integration_tests.cpp)
    target_link_libraries(arec-integration-tests PRIVATE arec_data_plane_core)
    target_compile_options(arec-integration-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME arec-integration-tests COMMAND arec-integration-tests)
    set_tests_properties(arec-integration-tests PROPERTIES TIMEOUT 30)
  endif()
else()
  message(FATAL_ERROR "AREC data plane supports macOS and Windows only")
endif()

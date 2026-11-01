cmake_minimum_required(VERSION 3.14)
project(whatsapp_proxy_translator VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(ASIO_VERSION "asio-1-38-2" CACHE STRING "Asio version tag")

include(FetchContent)
FetchContent_Declare(
  asio
  URL "https://github.com/chriskohlhoff/asio/archive/refs/tags/${ASIO_VERSION}.zip"
)
FetchContent_MakeAvailable(asio)

find_package(OpenSSL REQUIRED)

if(NOT DEFINED TRANSLATOR_VERSION OR TRANSLATOR_VERSION STREQUAL "")
  set(TRANSLATOR_VERSION "v${PROJECT_VERSION}")
endif()

if(TRANSLATOR_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  set(RC_VER_MAJOR ${CMAKE_MATCH_1})
  set(RC_VER_MINOR ${CMAKE_MATCH_2})
  set(RC_VER_PATCH ${CMAKE_MATCH_3})
else()
  set(RC_VER_MAJOR ${PROJECT_VERSION_MAJOR})
  set(RC_VER_MINOR ${PROJECT_VERSION_MINOR})
  set(RC_VER_PATCH ${PROJECT_VERSION_PATCH})
endif()

set(SOURCES
    src/main.cpp
    src/config.cpp
    src/logger.cpp
    src/cert.cpp
    src/utils.cpp
    src/server.cpp
)

if(WIN32)
    configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in
      ${CMAKE_CURRENT_BINARY_DIR}/version.rc
      @ONLY
    )
    list(APPEND SOURCES
      src/service/service_windows.cpp
      ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    )
else()
    list(APPEND SOURCES src/service/service_linux.cpp)
endif()

add_executable(translator ${SOURCES})
target_include_directories(translator PRIVATE
    src
    ${asio_SOURCE_DIR}/include
    ${asio_SOURCE_DIR}/asio/include
)
target_compile_definitions(translator PRIVATE
    ASIO_STANDALONE
    ASIO_DISABLE_RFC2818_VERIFICATION
    TRANSLATOR_VERSION="${TRANSLATOR_VERSION}"
)

if(WIN32)
    target_compile_definitions(translator PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(translator PRIVATE ws2_32 wsock32 crypt32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(translator PRIVATE Threads::Threads)
endif()

target_link_libraries(translator PRIVATE OpenSSL::SSL OpenSSL::Crypto)

install(TARGETS translator DESTINATION bin)

# Automated verification tests
enable_testing()

add_test(NAME TestVersion COMMAND translator --version)
set_tests_properties(TestVersion PROPERTIES WILL_FAIL FALSE)

add_test(NAME TestHelp COMMAND translator --help)
set_tests_properties(TestHelp PROPERTIES WILL_FAIL FALSE)

add_test(NAME TestStatus COMMAND translator --status)
set_tests_properties(TestStatus PROPERTIES WILL_FAIL FALSE)

add_test(NAME TestInstallRequiresProxy COMMAND translator --install)
set_tests_properties(TestInstallRequiresProxy PROPERTIES WILL_FAIL TRUE)

add_test(NAME TestChangeProxyRequiresAddress COMMAND translator --change-proxy)
set_tests_properties(TestChangeProxyRequiresAddress PROPERTIES WILL_FAIL TRUE)

add_test(NAME TestChangeProxyInvalidFormat COMMAND translator --change-proxy invalid_addr)
set_tests_properties(TestChangeProxyInvalidFormat PROPERTIES WILL_FAIL TRUE)

add_test(NAME TestUnknownOption COMMAND translator --unknown-flag)
set_tests_properties(TestUnknownOption PROPERTIES WILL_FAIL TRUE)

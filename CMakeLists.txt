cmake_minimum_required(VERSION 3.14)
project(whatsapp_proxy_translator)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)
FetchContent_Declare(
  asio
  URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-38-2.zip
)
FetchContent_MakeAvailable(asio)

find_package(OpenSSL REQUIRED)

set(SOURCES
    src/main.cpp
    src/config.cpp
    src/logger.cpp
    src/cert.cpp
    src/utils.cpp
    src/server.cpp
)

if(WIN32)
    list(APPEND SOURCES src/service/service_windows.cpp)
else()
    list(APPEND SOURCES src/service/service_linux.cpp)
endif()

add_executable(translator ${SOURCES})
target_include_directories(translator PRIVATE
    src
    ${asio_SOURCE_DIR}/include
    ${asio_SOURCE_DIR}/asio/include
)
target_compile_definitions(translator PRIVATE ASIO_STANDALONE ASIO_DISABLE_RFC2818_VERIFICATION)

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

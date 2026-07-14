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
  URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-36-0.zip
)
FetchContent_MakeAvailable(asio)

# Find OpenSSL for the independent proxy with SSL termination
find_package(OpenSSL REQUIRED)

add_executable(translator main.cpp)
target_include_directories(translator PRIVATE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(translator PRIVATE ASIO_STANDALONE ASIO_DISABLE_RFC2818_VERIFICATION _WIN32_WINNT=0x0A00)

target_link_libraries(translator PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(WIN32)
    target_link_libraries(translator PRIVATE ws2_32 wsock32 crypt32)
endif()

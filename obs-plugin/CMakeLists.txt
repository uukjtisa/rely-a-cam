cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Rely-A-Cam always needs the dock (frontend API) and Qt; they are not optional here.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(RAC_BUILD_TESTS "Build the protocol vector tests" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Svg)

# FFmpeg: headers and import libraries from obs-deps; at run time the DLLs OBS itself ships.
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
find_library(AVCODEC_LIB avcodec REQUIRED)
find_library(AVUTIL_LIB avutil REQUIRED)
find_library(SWSCALE_LIB swscale REQUIRED)
# nlohmann/json, header-only, also from obs-deps.
find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets Qt6::Network Qt6::Svg ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWSCALE_LIB}
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIR} ${NLOHMANN_INCLUDE_DIR} src)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp shell32 ole32 wintrust crypt32)
endif()
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON CXX_STANDARD 17)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/net/protocol.cpp
    src/net/hub.cpp
    src/decode/decoder.cpp
    src/source/phone-source.cpp
    src/source/phone-mic.cpp
    src/audio/routing.cpp
    src/audio/cable.cpp
    src/dock/dock.cpp
    src/sys/system.cpp
    src/third_party/qrcodegen/qrcodegen.cpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# The wire format against protocol/vectors, shared with the phone app's tests.
if(RAC_BUILD_TESTS)
  add_executable(rac-protocol-test tests/protocol_test.cpp src/net/protocol.cpp)
  target_include_directories(rac-protocol-test PRIVATE ${NLOHMANN_INCLUDE_DIR} src)
  set_target_properties(rac-protocol-test PROPERTIES CXX_STANDARD 17 FOLDER tests)
  target_compile_definitions(
    rac-protocol-test
    PRIVATE RAC_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/vectors/vectors.json"
  )
  enable_testing()
  add_test(NAME protocol-vectors COMMAND rac-protocol-test)
endif()

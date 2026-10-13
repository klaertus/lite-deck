cmake_minimum_required(VERSION 3.25)
project(lite_deck LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/bigobj /utf-8 /W4 /we4129 /permissive- /EHsc /Zc:__cplusplus)  # C4129: "\L" in a kernel object name is a bug
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

# Code shared by the sensors service, the daemon and the tests: no dependency on the rest of ldcore.
file(GLOB LD_SENSOR_SOURCES CONFIGURE_DEPENDS src/sensors/*.cpp)
add_library(ldsensors STATIC ${LD_SENSOR_SOURCES})
target_include_directories(ldsensors PUBLIC src)
target_link_libraries(ldsensors PUBLIC advapi32 shell32 ole32)

file(GLOB_RECURSE LD_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER LD_SOURCES EXCLUDE REGEX ".*/src/main\.cpp$")
list(FILTER LD_SOURCES EXCLUDE REGEX ".*/src/sensors/.*")
add_library(ldcore STATIC ${LD_SOURCES})
target_include_directories(ldcore PUBLIC src third_party)
target_link_libraries(ldcore PUBLIC ldsensors d2d1 dwrite windowscodecs hid setupapi ole32 oleaut32 shell32 user32 wtsapi32 advapi32 winhttp windowsapp gdi32 dxgi bcrypt ws2_32)

# The config page, embedded as RCDATA (never served from disk). Relink when a page file changes.
set(LD_WEB_RC ${CMAKE_SOURCE_DIR}/src/web/assets/web.rc)
set_source_files_properties(${LD_WEB_RC} PROPERTIES OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/web/assets/index.html;${CMAKE_SOURCE_DIR}/src/web/assets/app.css;${CMAKE_SOURCE_DIR}/src/web/assets/app.js;${CMAKE_SOURCE_DIR}/src/web/assets/appearance.js;${CMAKE_SOURCE_DIR}/src/web/assets/elements.js;${CMAKE_SOURCE_DIR}/src/web/assets/settings.js;${CMAKE_SOURCE_DIR}/src/web/assets/deck.js")
add_executable(lite-deck WIN32 src/main.cpp ${LD_WEB_RC})
target_link_libraries(lite-deck PRIVATE ldcore)

# Windows service (LocalSystem): only ldsensors + core/log are pulled from the static libraries.
add_executable(lite-deck-sensors sensors-service/main.cpp)
target_link_libraries(lite-deck-sensors PRIVATE ldsensors ldcore)

enable_testing()
file(GLOB LD_TESTS CONFIGURE_DEPENDS tests/*.cpp)
add_executable(lite-deck-tests ${LD_TESTS} ${LD_WEB_RC})
target_include_directories(lite-deck-tests PRIVATE tests)
# Reference images of the module rendering (tests/golden), read from the source tree.
target_compile_definitions(lite-deck-tests PRIVATE LD_TEST_DIR="${CMAKE_SOURCE_DIR}/tests")
target_link_libraries(lite-deck-tests PRIVATE ldcore)
add_test(NAME unit COMMAND lite-deck-tests)

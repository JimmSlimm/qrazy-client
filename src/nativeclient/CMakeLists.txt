cmake_minimum_required(VERSION 3.24)
project(qrazy_native LANGUAGES C CXX)
# The native client on Linux (Wayland, SDL3, Vulkan). The standalone executable
# and an embedding host can link the same runtime.
# The game core is NOT linked: libqrazy_core.so is opened at start (nc_coreload.cpp) and its functions are bound through the generated nc_core_table.gen.cpp
# (node src/nativeclient/gen-core-table.cjs <interface dir>; add --check to see whether it matches the headers).
#   -DQRAZY_INTERFACE_DIR=<dir>                contract headers (default: the pinned snapshot in ./interface; sync-interface.cjs updates it from qlrace2)
#   -DQRAZY_GLSLANG_PREFIX=<install prefix>    glslang built as static libraries (GLSL to SPIR-V only)
#   -DQRAZY_THIRD_PARTY=<dir>                  holding miniz/ and stb/ (miniz 3.0.2, stb_vorbis.c + stb_image.h)
set(CMAKE_CXX_STANDARD 17)
set(QRAZY_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/interface" CACHE PATH "contract headers (default: the pinned snapshot, see sync-interface.cjs)")
set(QRAZY_GLSLANG_PREFIX "" CACHE PATH "glslang install prefix")
set(QRAZY_THIRD_PARTY "" CACHE PATH "miniz and stb")
find_package(SDL3 CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)
find_package(Vulkan REQUIRED)   # headers only: the loader is opened through SDL
file(GLOB NC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/nc_*.cpp")
list(FILTER NC_SOURCES EXCLUDE REGEX "nc_main[.]cpp$")
set(NC_C_SOURCES "${QRAZY_THIRD_PARTY}/miniz/miniz.c" "${QRAZY_THIRD_PARTY}/miniz/miniz_tdef.c" "${QRAZY_THIRD_PARTY}/miniz/miniz_tinfl.c" "${QRAZY_THIRD_PARTY}/miniz/miniz_zip.c" "${QRAZY_THIRD_PARTY}/stb/stb_vorbis.c")
add_library(qrazy-native-runtime STATIC ${NC_SOURCES} ${NC_C_SOURCES})
add_executable(qrazy-native nc_main.cpp)
target_link_libraries(qrazy-native PRIVATE qrazy-native-runtime)
add_custom_command(TARGET qrazy-native POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:qrazy-native>/native-assets")
target_compile_definitions(qrazy-native-runtime PRIVATE NC_CORE_LOADER=1)
target_include_directories(qrazy-native-runtime
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${QRAZY_INTERFACE_DIR}"
  PRIVATE "${QRAZY_GLSLANG_PREFIX}/include" "${QRAZY_THIRD_PARTY}/miniz" "${QRAZY_THIRD_PARTY}/miniz-inc" "${QRAZY_THIRD_PARTY}/stb")
target_link_libraries(qrazy-native-runtime
  PUBLIC SDL3::SDL3 Vulkan::Headers
  PRIVATE "${QRAZY_GLSLANG_PREFIX}/lib/libglslang.a" "${QRAZY_GLSLANG_PREFIX}/lib/libglslang-default-resource-limits.a"
  CURL::libcurl PkgConfig::MPG123 pthread dl m)

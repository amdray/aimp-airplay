set(RAOP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libraop")
if(NOT EXISTS "${RAOP_ROOT}/libmdns/mdnssd/mdnssd.c")
  message(FATAL_ERROR "Fetch libraop and its submodules; see .github/workflows/build.yml")
endif()
set(MDNS_TOOLS "${CMAKE_CURRENT_BINARY_DIR}/mdns-tools")
file(MAKE_DIRECTORY "${MDNS_TOOLS}")
find_package(Git REQUIRED)
function(apply_vendor_patch name)
  set(patch "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/${name}.patch")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --no-index --check "${patch}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot apply ${name} adaptation: ${error}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --no-index "${patch}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot apply ${name} adaptation: ${error}")
  endif()
endfunction()
configure_file("${RAOP_ROOT}/libmdns/mdnssd/mdnssd.c" "${MDNS_TOOLS}/mdnssd.c" COPYONLY)
apply_vendor_patch(mdnssd.c)
add_library(mdns STATIC "${MDNS_TOOLS}/mdnssd.c")
target_include_directories(mdns PUBLIC "${RAOP_ROOT}/libmdns/mdnssd")
target_link_libraries(mdns PUBLIC ws2_32 iphlpapi)

# Build the sender from source: upstream ships only 32-bit Windows archives.
set(RAOP_TOOLS "${CMAKE_CURRENT_BINARY_DIR}/raop-tools")
file(MAKE_DIRECTORY "${RAOP_TOOLS}")
file(GLOB CROSS_FILES "${RAOP_ROOT}/crosstools/src/*")
foreach(FILE IN LISTS CROSS_FILES)
  get_filename_component(NAME "${FILE}" NAME)
  configure_file("${FILE}" "${RAOP_TOOLS}/${NAME}" COPYONLY)
endforeach()
if(MINGW)
  apply_vendor_patch(platform.h)
endif()
file(GLOB ALAC_SOURCES "${RAOP_ROOT}/libcodecs/alac/codec/*.c" "${RAOP_ROOT}/libcodecs/alac/codec/*.cpp")
foreach(name IN ITEMS raop_client.c rtsp_client.c)
  configure_file("${RAOP_ROOT}/src/${name}" "${RAOP_TOOLS}/${name}" COPYONLY)
  apply_vendor_patch(${name})
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/rtsp_io.h" "${RAOP_TOOLS}/rtsp_io.h" COPYONLY)
add_library(raop STATIC
  "${RAOP_TOOLS}/raop_client.c" "${RAOP_TOOLS}/rtsp_client.c"
  "${RAOP_ROOT}/src/aes.c" "${RAOP_ROOT}/src/aes_ctr.c"
  "${RAOP_ROOT}/dmap-parser/dmap_parser.c"
  "${RAOP_TOOLS}/platform.c" "${RAOP_TOOLS}/cross_net.c"
  "${RAOP_TOOLS}/cross_util.c" "${RAOP_TOOLS}/cross_log.c" "${RAOP_TOOLS}/cross_ssl.c"
  "${RAOP_ROOT}/libcodecs/addons/alac_wrapper.cpp" ${ALAC_SOURCES})
target_include_directories(raop PUBLIC "${RAOP_ROOT}/src" "${RAOP_TOOLS}"
  "${RAOP_ROOT}/libopenssl/targets/win32/x86/include")
target_include_directories(raop PRIVATE "${RAOP_ROOT}/dmap-parser"
  "${RAOP_ROOT}/libcodecs/addons" "${RAOP_ROOT}/libcodecs/alac/codec")
target_link_libraries(raop PUBLIC ws2_32 iphlpapi pthread)
target_compile_definitions(raop PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
if(MINGW)
  target_compile_options(raop PRIVATE -Wno-deprecated-declarations)
endif()
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
set(WITH_LSR_BINDINGS OFF CACHE BOOL "" FORCE)
list(APPEND CMAKE_MODULE_PATH "${RAOP_ROOT}/libcodecs/soxr/cmake/Modules")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("${RAOP_ROOT}/libcodecs/soxr" "soxr" EXCLUDE_FROM_ALL)

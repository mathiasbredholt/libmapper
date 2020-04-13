cmake_minimum_required(VERSION 3.5)
project(libmapper_test)

include_directories("include" "/usr/local/include")
file(GLOB SRCS "src/*.c")

add_executable(libmapper_test ${SRCS} "test/testselfconnection.c")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(liblo REQUIRED)
find_package(zlib REQUIRED)
target_link_libraries(libmapper_test ${LIBLO_LIBRARIES} ${ZLIB_LIBRARIES})
target_compile_options(libmapper_test PRIVATE -DDEBUG)

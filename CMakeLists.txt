cmake_minimum_required(VERSION 3.10)
project(gstwebsocketsink C CXX)

if(NOT DEFINED BIN_LIB_DESTINATION_FOLDER)
    set(BIN_LIB_DESTINATION_FOLDER  ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0)
endif()

message("BIN_LIB_DESTINATION_FOLDER=${BIN_LIB_DESTINATION_FOLDER}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-base-1.0)

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif(CCACHE_FOUND)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#
# ------- Include Components ---------
#

find_package(Boost COMPONENTS system)
message("Boost='${Boost_FOUND}'")

include(FetchContent)
FetchContent_Declare(
  websocketpp
  GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
  GIT_TAG        0.8.2
)
FetchContent_MakeAvailable(websocketpp)
add_subdirectory(${websocketpp_SOURCE_DIR} ${CMAKE_BINARY_DIR}/websocketpp)

#
# ------------ Libary --------------
#

add_library(${PROJECT_NAME} MODULE gstwebsocketsink.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${GSTREAMER_INCLUDE_DIRS}
    ${websocketpp_SOURCE_DIR}/websocketpp
    ${Boost_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME}
    ${Boost_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -fPIC
    ${GSTREAMER_CFLAGS}
)

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "${BIN_LIB_DESTINATION_FOLDER}"
)
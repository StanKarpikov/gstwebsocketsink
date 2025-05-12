cmake_minimum_required(VERSION 3.10)
project(gstwebsocketsink C CXX)

message("======== ${PROJECT_NAME} ========")

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

# This is a bit of a hack to redirect the installation of the component
option(OVERRIDE_INSTALLATION_DIR_OF_WEBSOCKETPP "Override the Websocketpp installation directory." ON)
if(OVERRIDE_INSTALLATION_DIR_OF_WEBSOCKETPP)
    # This overrides directories for websocketpp-config.cmake and websocketpp-configVersion.cmake
    set(INSTALL_CMAKE_DIR "")
    set(CMAKE_INSTALL_PREFIX_BACKUP "${CMAKE_INSTALL_PREFIX}")
    # This changes the installation root folder, it may affect all the build tree so we are going to restore it later
    set(CMAKE_INSTALL_PREFIX "${BIN_LIB_DESTINATION_FOLDER}" CACHE PATH "Install path" FORCE)
endif()

FetchContent_MakeAvailable(websocketpp)
add_subdirectory(${websocketpp_SOURCE_DIR} ${CMAKE_BINARY_DIR}/websocketpp)

if(OVERRIDE_INSTALLATION_DIR_OF_WEBSOCKETPP)
    # Restore the path
    set(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX_BACKUP}" CACHE PATH "Install path" FORCE)
endif()

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
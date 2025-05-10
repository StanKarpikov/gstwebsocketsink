cmake_minimum_required(VERSION 3.10)
project(gstwebsocketsink C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-base-1.0)
pkg_check_modules(LIBWEBSOCKETS REQUIRED libwebsockets)

add_library(${PROJECT_NAME} MODULE gstwebsocketsink.c)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${GSTREAMER_INCLUDE_DIRS}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME}
    ${GSTREAMER_LIBRARIES}
    ${LIBWEBSOCKETS_LIBRARIES}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -fPIC
    ${GSTREAMER_CFLAGS}
    ${LIBWEBSOCKETS_CFLAGS}
)

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0
)

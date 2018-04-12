cmake_minimum_required(VERSION 2.8)

SET(CMAKE_BUILD_TYPE "Debug")

link_libraries(libva.so)
link_libraries(libva-drm.so)
link_libraries(libva-x11.so)
link_libraries(libva-glx.so)
link_libraries(libX11.so.6)


SET( CMAKE_CXX_FLAGS "-std=c++11 " )
include_directories(${LIBVA_PATH})
add_executable(Main
    TestMain.cpp
    VDecAccelVA.cpp
    )


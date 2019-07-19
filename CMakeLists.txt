cmake_minimum_required (VERSION 3.5)
project (mt CXX)

# FIXME: clean up signed/unsigned comparisons and enable the warning.
add_compile_options(-std=c++11 -pedantic -Wall -Werror -Wno-sign-compare)
add_definitions(-DVERSION=\"0.1\" -D_XOPEN_SOURCE=600)

MESSAGE ( STATUS "Searching for X11..." )
find_package(X11 REQUIRED)
IF ( X11_FOUND )
#      INCLUDE_DIRECTORIES ( ${X11_INCLUDE_DIR} )
#      LINK_LIBRARIES      ( ${X11_LIBRARIES} )
      MESSAGE ( STATUS "  X11_INCLUDE_DIR:         " ${X11_INCLUDE_DIR} )
      MESSAGE ( STATUS "  X11_LIBRARIES:           " ${X11_LIBRARIES} )
ENDIF ( X11_FOUND )

If(NOT X11_FOUND)
  Message(STATUS "X11_INCLUDE_DIR: ${X11_INCLUDE_DIR}")
  Message(STATUS "X11_LIBRARIES: ${X11_LIBRARIES}")
  Message(FATAL_ERROR "libX11 and X11 headers must be installed.")
EndIf()

If(NOT X11_Xft_FOUND)
  Message(STATUS "X11_Xft_INCLUDE_PATH: ${X11_Xft_INCLUDE_PATH}")
  Message(STATUS "X11_Xft_LIB: ${X11_Xft_LIB}")
  Message(FATAL_ERROR "libXft and Xft headers must be installed.")
EndIf()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FC REQUIRED fontconfig)
pkg_check_modules(FT REQUIRED freetype2)

include_directories(${FC_INCLUDE_DIRS} ${FT_INCLUDE_DIRS})
link_directories(${FC_LBIRARY_DIRS} ${FT_LIBRARY_DIRS})
add_compile_options(${FC_CFLAGS} ${FT_CFLAGS})


add_executable(mt mt.cc config.h mt.h x.h x.cc font.h font.cc)
target_link_libraries(mt -lm -lrt -lutil
                      ${X11_LIBRARIES} ${X11_Xft_LIB}
                      ${FC_LIBRARIES} ${FT_LIBRARIES})

#
# Copyright (c) 2019 Carnegie Mellon University,
#
# All rights reserved.
#
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file. See the AUTHORS file for names of contributors.
#

#
# CMakeLists.txt  cmake for mercury-progressor
# 29-Oct-2019  chuck@ece.cmu.edu
#

#
# configure/load in standard modules we plan to use and probe the enviroment
#
include (CMakePackageConfigHelpers)

set (CMAKE_THREAD_PREFER_PTHREAD TRUE)
set (THREADS_PREFER_PTHREAD_FLAG TRUE)

find_package (Threads REQUIRED)

# where to install files for "find_package"
set (mercury-progressor-pkg-loc "share/cmake/mercury-progressor")

#
# library version set here (e.g. for shared libs).
#
set (MERCURY_PROGRESSOR_VERSION_MAJOR 1)
set (MERCURY_PROGRESSOR_VERSION_MINOR 0)
set (MERCURY_PROGRESSOR_VERSION_PATCH 0)
set (mercury-progressor-vers
    "${MERCURY_PROGRESSOR_VERSION_MAJOR}.${MERCURY_PROGRESSOR_VERSION_MINOR}")
set (MERCURY_PROGRESSOR_VERSION
    "${mercury-progressor-vers}.${MERCURY_PROGRESSOR_VERSION_PATCH}")

#
# create library target (user can specify shared vs. static using
# BUILD_SHARED_LIBS).  arrange for users of our lib to get the correct "-I"'s
#
if (${ENABLE_MARGO})
  add_library (mercury-progressor margo-progressor.c)
else ()
  add_library (mercury-progressor mercury-progressor.c)
endif ()
target_include_directories (mercury-progressor
                            PUBLIC $<INSTALL_INTERFACE:include>)

# configure config.h
configure_file ("config.h.in" "config.h" @ONLY)
# add the build directory as include path
target_include_directories (mercury-progressor BEFORE PUBLIC
                            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

# make sure our build includes are BEFORE a previously installed version
target_include_directories (mercury-progressor BEFORE PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include/mercury-progressor>)
target_link_libraries (mercury-progressor mercury ${EXTRA_LIBS})
# XXX: cmake 3.1 and newer define a Threads::Threads imported target
# that we should switch too when we are ready to require 3.1 or better.
# (3.1 was released late 2014)
if (THREADS_HAVE_PTHREAD_ARG)
    target_compile_options (mercury-progressor PUBLIC "-pthread")
endif ()
if (CMAKE_THREAD_LIBS_INIT)
    target_link_libraries (mercury-progressor "${CMAKE_THREAD_LIBS_INIT}")
endif ()

set_target_properties(mercury-progressor
                      PROPERTIES VERSION ${MERCURY_PROGRESSOR_VERSION}
                      SOVERSION ${MERCURY_PROGRESSOR_VERSION_MAJOR})

#
# installation stuff (packaging and install commands)
#
write_basic_package_version_file(
    "mercury-progressor-config-version.cmake"
    VERSION ${MERCURY_PROGRESSOR_VERSION}
    COMPATIBILITY AnyNewerVersion)

# generate our config file for find_package()
configure_file (mercury-progressor-config.cmake.in
                mercury-progressor-config.cmake @ONLY)

#
# "make install" rules
#
install (TARGETS mercury-progressor EXPORT mercury-progressor-targets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)
install (EXPORT mercury-progressor-targets
        DESTINATION ${mercury-progressor-pkg-loc}
        FILE "mercury-progressor-targets.cmake")
install (FILES "${CMAKE_CURRENT_BINARY_DIR}/mercury-progressor-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/mercury-progressor-config-version.cmake"
        DESTINATION ${mercury-progressor-pkg-loc} )
install (FILES ../include/mercury-progressor/mercury-progressor.h
         DESTINATION include/mercury-progressor)

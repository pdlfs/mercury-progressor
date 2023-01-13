#
# Copyright (c) 2019 Carnegie Mellon University,
#
# All rights reserved.
#
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file. See the AUTHORS file for names of contributors.
#

#
# CMakeLists.txt  cmake for mercury-progressor tests
# 29-Oct-2019  chuck@ece.cmu.edu
#

if (${ENABLE_MARGO})
  add_executable (test-margo-progressor test-margo-progressor.c)
  target_link_libraries (test-margo-progressor mercury-progressor)
  add_test (test-margo-progressor test-margo-progressor)
endif ()

add_executable (test-progressor test-progressor.c)
target_link_libraries (test-progressor mercury-progressor)
add_test (test-progressor test-progressor)

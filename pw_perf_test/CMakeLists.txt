# Copyright 2022 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)
include($ENV{PW_ROOT}/pw_perf_test/backend.cmake)
include($ENV{PW_ROOT}/pw_protobuf_compiler/proto.cmake)

pw_add_library(pw_perf_test STATIC
  PUBLIC_INCLUDES
    public
  HEADERS
    public/pw_perf_test/internal/framework.h
    public/pw_perf_test/internal/test_info.h
    public/pw_perf_test/perf_test.h
  PUBLIC_DEPS
    pw_perf_test.event_handler
    pw_perf_test.state
    pw_perf_test.timer
  SOURCES
    framework.cc
    perf_test.cc
    test_info.cc
)

pw_add_library(pw_perf_test.state STATIC
  PUBLIC_INCLUDES
    public
  HEADERS
    public/pw_perf_test/state.h
  PUBLIC_DEPS
    pw_perf_test.timer
    pw_perf_test.event_handler
    pw_assert
  PRIVATE_DEPS
    pw_log
    pw_numeric.integer_division
  SOURCES
    state.cc
)

if(NOT "${pw_perf_test.TIMER_INTERFACE_BACKEND}" STREQUAL "")
  pw_add_test(pw_perf_test.state_test
    SOURCES
      state_test.cc
    PRIVATE_DEPS
      pw_assert.assert
      pw_perf_test
    GROUPS
      modules
      pw_perf_test
  )
endif()

# Event handlers

pw_add_library(pw_perf_test.event_handler INTERFACE
  HEADERS
    public/pw_perf_test/event_handler.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_perf_test.log_csv_event_handler STATIC
  PUBLIC_INCLUDES
    public
  PRIVATE_DEPS
    pw_log
  PUBLIC_DEPS
    pw_perf_test.event_handler
    pw_perf_test
    pw_string.builder
  HEADERS
    public/pw_perf_test/log_csv_event_handler.h
  SOURCES
    log_csv_event_handler.cc
)

pw_add_library(pw_perf_test.log_csv_main STATIC
  PUBLIC_DEPS
    pw_perf_test.log_csv_event_handler
  SOURCES
    log_csv_main.cc
)

pw_add_library(pw_perf_test.logging_event_handler STATIC
  PUBLIC_INCLUDES
    public
  PRIVATE_DEPS
    pw_log
  PUBLIC_DEPS
    pw_perf_test.event_handler
    pw_perf_test
  HEADERS
    public/pw_perf_test/googletest_style_event_handler.h
    public/pw_perf_test/logging_event_handler.h
  SOURCES
    logging_event_handler.cc
)

pw_add_library(pw_perf_test.logging_main STATIC
  PUBLIC_DEPS
    pw_perf_test.logging_event_handler
  SOURCES
    logging_main.cc
)

# Timer facade

pw_add_library(pw_perf_test.duration_unit INTERFACE
  HEADERS
    public/pw_perf_test/internal/duration_unit.h
  PUBLIC_INCLUDES
    public
)

pw_add_facade(pw_perf_test.timer INTERFACE
  BACKEND
    pw_perf_test.TIMER_INTERFACE_BACKEND
  HEADERS
    public/pw_perf_test/internal/timer.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_perf_test.duration_unit
)

if(NOT "${pw_perf_test.TIMER_INTERFACE_BACKEND}" STREQUAL "")
  pw_add_test(pw_perf_test.timer_test
    SOURCES
      timer_test.cc
    PRIVATE_DEPS
      pw_perf_test.timer
      pw_thread.sleep
      pw_chrono.system_clock
    GROUPS
      modules
      pw_perf_test
  )
endif()

# Chrono timer facade implementation

pw_add_library(pw_perf_test.chrono_timer INTERFACE
  HEADERS
    chrono_public_overrides/pw_perf_test_timer_backend/timer.h
    public/pw_perf_test/internal/chrono_timer_interface.h
  PUBLIC_INCLUDES
    chrono_public_overrides
    public
  PUBLIC_DEPS
    pw_chrono.system_clock
    pw_perf_test.duration_unit
)

if(NOT "${pw_perf_test.TIMER_INTERFACE_BACKEND}"
    STREQUAL "pw_chrono.SYSTEM_CLOCK_BACKEND.NO_BACKEND_SET")
  pw_add_test(pw_perf_test.chrono_timer_test
    SOURCES
      chrono_test.cc
    PRIVATE_DEPS
      pw_perf_test.timer
      pw_thread.sleep
      pw_chrono.system_clock
    GROUPS
      modules
      pw_perf_test
  )
endif()

# Module-level targets

if(NOT "${pw_perf_test.TIMER_INTERFACE_BACKEND}"
    STREQUAL "pw_chrono.SYSTEM_CLOCK_BACKEND.NO_BACKEND_SET")
  add_executable(pw_perf_test.example_perf_test EXCLUDE_FROM_ALL
    examples/example_perf_test.cc
  )

  target_link_libraries(pw_perf_test.example_perf_test
    pw_perf_test
  )
endif()

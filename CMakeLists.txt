cmake_minimum_required(VERSION 3.20)

project(rcl_logging_journal)

# Default to C11
if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()
# Default to C++17
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# systemd-journald only exists on Linux.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "${PROJECT_NAME} requires Linux with systemd-journald; "
    "detected CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
endif()

find_package(ament_cmake_ros REQUIRED)
find_package(rcl_logging_interface REQUIRED)
find_package(rcpputils REQUIRED)
find_package(rcutils REQUIRED)

# libsystemd provides sd_journal_sendv() and the sd_journal read API.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
if(NOT TARGET PkgConfig::SYSTEMD)
  message(FATAL_ERROR
    "libsystemd development files were not found by pkg-config. "
    "Install them first, e.g. 'apt install libsystemd-dev' "
    "(rosdep key: libsystemd-dev).")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(
    -Wformat=2 -Wconversion -Woverloaded-virtual -Wshadow
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-qual
  )
endif()

add_library(${PROJECT_NAME} src/rcl_logging_journal.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
  PkgConfig::SYSTEMD
  rcpputils::rcpputils
  rcutils::rcutils)
target_link_libraries(${PROJECT_NAME} PUBLIC
  rcl_logging_interface::rcl_logging_interface)

target_compile_definitions(${PROJECT_NAME} PRIVATE "RCL_LOGGING_INTERFACE_BUILDING_DLL")

install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

# Optional journald.conf drop-in with robot oriented defaults.
install(DIRECTORY config
  DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # cppcheck is disabled on Linux in the ROS 2 core packages already
  # (rcl_logging_spdlog, rcl_logging_syslog); keep parity here.
  list(APPEND AMENT_LINT_AUTO_EXCLUDE
    ament_cmake_cppcheck
  )
  ament_lint_auto_find_test_dependencies()

  find_package(ament_cmake_gtest REQUIRED)
  # The test reads its own records back through the sd_journal API, so it
  # needs a running systemd-journald. See README.md "Test" for containers.
  ament_add_gtest(test_logging_journald test/test_logging_journald.cpp
    TIMEOUT 120)
  if(TARGET test_logging_journald)
    target_link_libraries(test_logging_journald
      ${PROJECT_NAME}
      PkgConfig::SYSTEMD
      rcpputils::rcpputils
      rcutils::rcutils)
    target_compile_definitions(test_logging_journald PUBLIC RCUTILS_ENABLE_FAULT_INJECTION)
  endif()

  # The micro benchmark is optional so the package still builds on images
  # that ship ros-base only (performance_test_fixture is part of desktop-dev).
  find_package(performance_test_fixture QUIET)
  if(performance_test_fixture_FOUND)
    add_performance_test(benchmark_logging_interface test/benchmark/benchmark_logging_interface.cpp)
    if(TARGET benchmark_logging_interface)
      target_link_libraries(benchmark_logging_interface ${PROJECT_NAME} rcutils::rcutils)
    endif()
  else()
    message(STATUS "performance_test_fixture not found, skipping benchmark_logging_interface")
  endif()
endif()

ament_export_dependencies(rcl_logging_interface)
ament_export_libraries(${PROJECT_NAME})
ament_export_targets(${PROJECT_NAME})
ament_package()

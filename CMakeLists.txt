cmake_minimum_required(VERSION 3.16)

project(untitled LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Test)

qt_standard_project_setup()

qt_add_executable(qawaitablefuture
  main.cpp
)

target_link_libraries(qawaitablefuture PRIVATE Qt::Core Qt::Concurrent)

enable_testing()
add_subdirectory(tests)
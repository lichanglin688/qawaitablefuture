qt_add_executable(test_qawaitablefuture
  test_qawaitablefuture.cpp
)

# 被测头文件在上一层目录
target_include_directories(test_qawaitablefuture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(test_qawaitablefuture PRIVATE Qt::Core Qt::Concurrent Qt::Test)

add_test(NAME test_qawaitablefuture COMMAND test_qawaitablefuture)

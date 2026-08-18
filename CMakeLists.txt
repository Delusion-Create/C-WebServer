cmake_minimum_required(VERSION 3.10)
project(webserver LANGUAGES CXX)

# C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项: 打开所有警告, 开启 O2 优化
add_compile_options(-Wall -Wextra -O2)

# 可执行文件输出到 out/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/out)

# 自动收集 src 下所有 .cpp 源文件
aux_source_directory(src SRC_FILES)
add_executable(webserver ${SRC_FILES})

# 链接 pthread
find_package(Threads REQUIRED)
target_link_libraries(webserver Threads::Threads)

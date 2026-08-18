#!/bin/bash

# 增强版自动构建脚本
# 设置遇到错误时退出脚本
set -e

# 定义目录变量
BUILD_DIR="build"
PROJECT_ROOT="$(pwd)"

echo "项目根目录: $PROJECT_ROOT"
echo "准备处理构建目录: $BUILD_DIR"
echo "----------------------------------------"

# 1. 检查构建目录是否存在
if [ -d "$BUILD_DIR" ]; then
    echo "信息: 构建目录 '$BUILD_DIR' 已存在，无需创建。[1,2,3](@ref)" 
else
    echo "信息: 构建目录 '$BUILD_DIR' 不存在，正在创建...[4](@ref)"
    
    # 使用 mkdir -p 创建目录，-p 选项确保需要时创建父目录[3,5](@ref)
    if mkdir -p "$BUILD_DIR"; then
        echo "成功: 构建目录 '$BUILD_DIR' 创建成功。[1,2](@ref)"
    else
        echo "错误: 无法创建构建目录 '$BUILD_DIR'。请检查权限或路径。" >&2
        exit 1
    fi
fi

echo "----------------------------------------"

# 2. 进入构建目录
echo "正在进入构建目录..."
if cd "$BUILD_DIR"; then
    echo "成功: 当前工作目录: $(pwd)"
else
    echo "错误: 无法进入构建目录 '$BUILD_DIR'。" >&2
    exit 1
fi

echo "----------------------------------------"

# 3. 执行CMake配置（这里使用 cmake --fresh .. 需要CMake 3.24+）
# 如果你使用的是旧版CMake，可以替换为 cmake ..
echo "步骤 1: 执行 CMake 配置..."
if cmake  ..; then
    echo "成功: CMake 配置完成。"
else
    echo "错误: CMake 配置失败。" >&2
    exit 1
fi

echo "----------------------------------------"

# 4. 执行编译（使用所有可用的CPU核心）
echo "步骤 2: 开始编译项目..."
CORES=$(nproc)
echo "使用 $CORES 个并行任务进行编译..."

if make -j"$CORES"; then
    echo "成功: 项目编译完成。"
else
    echo "错误: 编译过程失败。" >&2
    exit 1
fi

echo "----------------------------------------"
echo "全部构建步骤已完成！"
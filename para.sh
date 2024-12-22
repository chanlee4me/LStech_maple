#!/bin/bash

# 设置循环次数
LOOP_COUNT=5  # 将5替换为你需要的循环次数

# 需要修改的路径
CNF_DIR="/mnt/chenli/SAT/2023cnf"
OUTPUT_DIR="/mnt/chenli/SAT/istech_maple-2021"
CADICAL_PATH="/mnt/chenli/SAT/istech_maple-2021/simp/glucose"

# 每次循环处理的CNF文件个数
TOTAL_FILES=400

# 获取系统的CPU核心数
NUM_CORES=$(nproc)

# 确保输出目录存在
mkdir -p "$OUTPUT_DIR"

# 定义处理单个文件的函数，接收循环次数和文件路径作为参数
process_file() {
    loop_num="$1"
    file="$2"
    str=$(readlink -f "$file")
    echo "循环 $loop_num: 开始处理文件: $str"

    temp_file=$(mktemp)
    printf "%s," "$str" >> "$temp_file"

    output_file=$(mktemp)

    # 使用timeout命令限制程序执行时间
    timeout -s SIGTERM 3600 "$CADICAL_PATH" "$str" > "$output_file" 2>&1
    if [ $? -eq 124 ]; then
        echo "文件处理超时: $str"
    else
        echo "文件处理完成: $str"
    fi

    # 提取UNSATISFIABLE、SATISFIABLE、UNKNOWN
    status=$(grep -Eo "UNSATISFIABLE|SATISFIABLE|UNKNOWN" "$output_file")

    # 如果没有检测到状态，则标记为 TIMEOUT
    if [ -z "$status" ]; then
        status="TIMEOUT"
    fi

    echo "$status" >> "$temp_file"

    # 提取process-time所在行
    grep "CPU time" "$output_file" >> "$temp_file"

    printf "\n" >> "$temp_file"

    mv "$temp_file" "$OUTPUT_DIR/2023cnf_${loop_num}.csv.$BASHPID"
    rm "$output_file"  # 删除临时输出文件
}

export -f process_file
export CADICAL_PATH
export OUTPUT_DIR

# 主循环
for ((i=1; i<=LOOP_COUNT; i++)); do
    echo "=============================="
    echo "开始循环 $i"
    echo "=============================="

    # 结果文件的位置，根据循环次数命名
    PROCESSED_FILES="$OUTPUT_DIR/2023cnf_${i}.csv"
    touch "$PROCESSED_FILES"
    # 切换到CNF文件目录
    cd "$CNF_DIR" || { echo "无法切换到目录 $CNF_DIR"; exit 1; }

    # 生成未处理文件的列表
    find . -name "*.cnf" | while read -r file; do
        if ! grep -q "$(readlink -f "$file")" "$PROCESSED_FILES"; then
            echo "$file"
        fi
    done > "/tmp/unprocessed_files_${i}.txt"

    # 检查是否有未处理的文件
    if [ ! -s "/tmp/unprocessed_files_${i}.txt" ]; then
        echo "循环 $i: 没有未处理的文件。跳过。"
        continue
    fi

    # 使用xargs命令并行处理文件
    head -n "$TOTAL_FILES" "/tmp/unprocessed_files_${i}.txt" | \
    xargs -n 1 -P "$NUM_CORES" -I {} bash -c 'process_file "$0" "$1"' "$i" "{}"

    # 合并所有临时文件到一个csv文件中
    for tmp_file in "$OUTPUT_DIR"/2023cnf_${i}.csv.*; do
        cat "$tmp_file" >> "$PROCESSED_FILES"
        rm "$tmp_file" # 删除临时文件
    done

    echo "循环 $i 完成。输出文件: $PROCESSED_FILES"
done

echo "所有 $LOOP_COUNT 次循环处理完成"

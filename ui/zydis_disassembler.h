#pragma once

#include <Zydis/Zydis.h>

#include "type/process_arch.h"

#include <cstdint>
#include <string>
#include <vector>

// 反汇编输出的单行结果。
struct disasm_line {
    uint64_t    address = 0;   // 指令运行时地址
    std::string bytes;         // 机器码十六进制（如 "48 89 E5"）
    std::string text;          // 汇编文本（如 "mov rbp, rsp"）
    uint8_t     length = 0;    // 指令字节长度

    // 控制流信息（跳转跟随/箭头用）
    bool     is_branch = false;   // jmp/jcc/call/ret 等控制流指令
    bool     is_cond = false;     // 条件跳转（JCC）
    bool     is_call = false;
    bool     is_ret = false;
    uint64_t branch_target = 0;   // 可解析的绝对目标地址；间接寄存器目标为 0

    // 该指令中所有 memory 操作数里、纯绝对寻址（base=NONE 且 index=NONE）、
    // disp!=0 的绝对地址。用于把 mov rdx, [0x7FF7...] 这类一般指令中的立即数
    // 地址也按模块+偏移格式化（jcc/jmp/call 之外的"更一般的"显示）。
    // 已被 branch_target 覆盖的目标不再重复入栈。
    std::vector<uint64_t> mem_abs_addrs;
};

// Zydis 反汇编封装。
//
// decoder 依赖机器模式，附加进程架构变化时（32↔64 位切换）必须重建，
// 所以调用方每帧 set_arch() 传入当前架构；内部在架构改变时才重新 ZydisDecoderInit。
class disassembler {
public:
    disassembler();
    ~disassembler() = default;

    disassembler(const disassembler&) = delete;
    disassembler& operator=(const disassembler&) = delete;

    // 设置目标架构；与上次相同则不重建 decoder。
    void set_arch(process_arch arch);

    // 从目标进程读取并反汇编 addr 处 count 条指令。
    // 未附加 / 读取失败 / 解码失败会提前返回，结果可能是空或不足 count 条。
    std::vector<disasm_line> disassemble(uint64_t addr, int count);

    // 顺序解码 addr 起 max_count 条指令（一次读 max_bytes 块），返回每条指令起始地址。
    // 供反汇编视图"向上滚动"回退定位使用；读取失败返回空。
    std::vector<uint64_t> decode_starts(uint64_t addr, int max_count, int max_bytes);

private:
    process_arch   arch_ = process_arch::unknown;
    bool           decoder_ready_ = false;

    ZydisDecoder    decoder_;
    ZydisFormatter  formatter_;
};

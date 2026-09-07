#include "ui/zydis_disassembler.h"

#include "core/process_manager.h"

#include <Zydis/DecoderTypes.h>
#include <Zydis/Utils.h>

#include <cstdio>
#include <vector>

namespace {

// 一次读取的内存窗口大小。x86 指令最长 15 字节，64 字节足够覆盖当前指令。
constexpr size_t k_window_size = 64;

// 块读取大小：一次读 512 字节连续解码，把每条一次的 ReadProcessMemory
// 合并为一次。失败（跨内存区边界）时回退到逐指令窗口读取。
constexpr size_t k_block_size = 512;

// 单条指令格式化的文本缓冲区（256 足够覆盖最长的 Intel 语法文本）。
constexpr size_t k_text_size = 256;

} // namespace

disassembler::disassembler()
{
    ZydisFormatterInit(&formatter_, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_FORCE_SIZE, ZYAN_TRUE);
    ZydisFormatterSetProperty(&formatter_, ZYDIS_FORMATTER_PROP_FORCE_SEGMENT, ZYAN_TRUE);
}

void disassembler::set_arch(process_arch arch)
{
    if (arch == arch_ && decoder_ready_)
        return;

    arch_ = arch;
    decoder_ready_ = false;

    if (arch == process_arch::x86_64) {
        decoder_ready_ = ZYAN_SUCCESS(ZydisDecoderInit(
            &decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64));
    } else if (arch == process_arch::x86_32) {
        decoder_ready_ = ZYAN_SUCCESS(ZydisDecoderInit(
            &decoder_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32));
    }
}

std::vector<uint64_t> disassembler::decode_starts(uint64_t addr, int max_count,
                                                  int max_bytes)
{
    std::vector<uint64_t> out;
    if (!decoder_ready_ || max_count <= 0)
        return out;

    auto* mem = process_manager::instance().memory();
    if (!mem)
        return out;

    if (max_bytes < 16)
        max_bytes = 16;
    std::vector<uint8_t> block((size_t)max_bytes, 0);
    if (!mem->read(addr, block.data(), block.size()))
        return out;

    size_t off = 0;
    for (int i = 0; i < max_count && off + 16 <= block.size(); ++i) {
        ZydisDecodedInstruction instr;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
                &decoder_, block.data() + off, block.size() - off, &instr, operands)))
            break;
        out.push_back(addr + off);
        off += instr.length;
    }
    return out;
}

std::vector<disasm_line> disassembler::disassemble(uint64_t addr, int count)
{
    std::vector<disasm_line> lines;

    if (!decoder_ready_)
        return lines;

    auto* mem = process_manager::instance().memory();
    if (!mem)
        return lines;

    // 快路径：一次读一大块后顺序解码；失败（跨内存区边界）回退逐指令窗口读取。
    std::vector<uint8_t> block(k_block_size, 0);
    const bool have_block = mem->read(addr, block.data(), block.size());
    std::vector<uint8_t> window(k_window_size, 0);

    uint64_t cur = addr;
    size_t   off = 0;

    for (int i = 0; i < count; ++i) {
        const uint8_t* p = nullptr;
        size_t avail = 0;
        if (have_block && off + 16 <= block.size()) {
            p = block.data() + off;
            avail = block.size() - off;
        } else {
            if (!mem->read(cur, window.data(), window.size()))
                break;
            p = window.data();
            avail = window.size();
        }

        ZydisDecodedInstruction instr;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(
                &decoder_, p, avail, &instr, operands)))
            break;

        char text[k_text_size] = {};
        ZydisFormatterFormatInstruction(&formatter_, &instr, operands,
                                        instr.operand_count_visible,
                                        text, sizeof(text), cur, nullptr);

        disasm_line line;
        line.address = cur;
        line.length  = instr.length;

        char b[8];
        for (ZyanU8 k = 0; k < instr.length; ++k) {
            std::snprintf(b, sizeof(b), "%02X ", p[k]);
            line.bytes += b;
        }
        line.text = text;

        // 控制流分类 + 绝对跳转目标解析（间接寄存器目标无法静态解析，保持 0）
        if (instr.meta.branch_type != ZYDIS_BRANCH_TYPE_NONE) {
            line.is_branch = true;
            line.is_cond = (instr.meta.category == ZYDIS_CATEGORY_COND_BR);
            line.is_call = (instr.meta.category == ZYDIS_CATEGORY_CALL);
            line.is_ret  = (instr.meta.category == ZYDIS_CATEGORY_RET);
            for (ZyanU8 oi = 0; oi < instr.operand_count; ++oi) {
                const auto& op = operands[oi];
                if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                    op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                    ZyanU64 target = 0;
                    if (ZYAN_SUCCESS(
                            ZydisCalcAbsoluteAddress(&instr, &op, cur, &target))) {
                        line.branch_target = target;
                        break;
                    }
                }
            }
        }

        lines.push_back(std::move(line));

        off += instr.length;
        cur += instr.length;   // 按指令真实长度推进，避免长指令错位
    }

    return lines;
}

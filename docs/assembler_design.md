# Assembler（最小可用版）设计与实现文档

> 本文档是实现「汇编器」功能的对照蓝本，不写任何产品代码，只描述架构、API、解析器规则、接入点与代码骨架，供自行实现时参考。
>
> 范围决策（已确认）：**最小可用版** —— 多行 x86/x64 汇编文本 → Zydis 编码成机器码 → patch 写入目标进程选定地址。不做 CE 脚本（label/alloc/aobscan）。接入方式：独立 Auto-Assemble 窗口 + 反汇编视图右键「Assemble here」预填地址，两条入口都要。

---

## 1. 目标与范围边界

### 做
- 多行汇编文本 → 逐行词法/语义解析 → `ZydisEncoderRequest` → `ZydisEncoderEncodeInstruction` → 机器码字节。
- 起始地址 + 编码结果 → 顺序写回目标进程（复用现有 `IMemoryAccessor::write`，含 `VirtualProtectEx` 改保护兜底）。
- 支持：寄存器操作数、立即数（hex `0x..` / 十进制 / 负数）、内存操作数 `[reg]` / `[reg+disp]` / `[reg+reg*scale+disp]` / `[disp绝对]`、`ptr` 大小修饰、段覆盖 `es:[..]`、常见指令（mov/add/sub/and/or/xor/cmp/test/jmp/jcc/call/ret/push/pop/lea/nop/int3）。
- 独立 ImGui 窗口（CE Auto-Assemble 风格）：地址输入框 + 多行代码框 + Assemble 预览 / Write 写回 按钮 + 结果表格。
- 两条入口：Memory Viewer 菜单 `Tools → Auto assembly`（空白打开）、反汇编行右键 `Assemble here`（预填该行地址）。
- 架构自适应：随附加进程的 `process_arch`（x86_64 / x86_32）切换 `ZydisMachineMode`。

### 不做（明确边界，留作后续）
- `[enable]/[disable]` 脚本块、label、aobscan/aobscanmodule、alloc/dealloc、registersymbol —— 引擎结构上预留 `symbol_map` 钩子但最小版空实现。
- AVX-512/EVEX/MVEX 操作数（`evex`/`mvex` 字段不填）。
- 持久化「还原原始字节」备份/还原按钮（Write 前显示原字节供人工对照，不自动备份）。
- far call/jmp 的 `ptr`（段:偏移）操作数类型（`ZYDIS_OPERAND_TYPE_POINTER` 不实现）。
- 汇编语法大小写敏感性以外的方言（Masm/Intel/Nasm 混用）：统一按 Zydis Intel 风格小写归一化查表。

---

## 2. 架构总览与数据流

```
┌─────────────────────────────────────────────────────────────┐
│  assembler_window  (ImGui)                                  │
│  ┌──────────────┐  ┌────────────────┐  ┌────────────────┐   │
│  │ 地址输入框    │  │ InputTextMulti │  │ 结果表格/日志   │   │
│  │ (hex)        │  │ line (代码)     │  │ 行|机器码|错误 │   │
│  └──────┬───────┘  └────┬───────────┘  └────▲───────────┘   │
│         │               │                   │               │
│         │      [Assemble]│[Write]            │               │
│         ▼               ▼                   │               │
│      addr ──► assembler_engine.assemble(text)               │
│                        │                                    │
└────────────────────────┼────────────────────────────────────┘
                         ▼
              ┌───────────────────────────┐
              │  assembler_engine         │
              │  ┌─────────────────────┐  │
              │  │ 反查表(懒加载)      │  │ mnemonic str→enum
              │  │                     │  │ register str→enum
              │  └─────────────────────┘  │
              │  逐行:                    │
              │   tokenize               │
              │   parse_operand(s)       │
              │   build_encoder_request  │
              │   ZydisEncoderEncode      │  ──► bytes[]
              │     InstructionAbsolute  │      (含 RIP 相对回写)
              └───────────┬───────────────┘
                          ▼
              process_manager::instance()
                .memory()->write(addr, bytes, n)
                          │
                          ▼  WriteProcessMemory (+VirtualProtectEx 兜底)
                    目标进程内存
```

**数据流要点**
1. 文本→机器码是**纯本地计算**，不碰目标进程；只有 Write 才读地址并写回。
2. 编码用 `ZydisEncoderEncodeInstructionAbsolute`（带 runtime_address），让编码器自动处理 `jmp`/`call`/RIP 相对等需要相对偏移的指令——这正是 patch 场景（已知运行时地址）所需。
3. 多行指令顺序编码，每条指令的运行时地址 = 起始地址 + 前面所有指令字节长度之和。

---

## 3. 关键技术约束

### 3.1 Zydis 编码器已就绪
- `Imgui_Study.pro:56-57` 已编译 `libs/Zydis/src/Encoder.c` 与 `EncoderData.c`。
- `Zydis.h` 默认包含 `Encoder.h`（除非定义 `ZYDIS_DISABLE_ENCODER`，项目未定义）。
- 编码入口：
  - `ZydisEncoderEncodeInstruction(request, buffer, &length)` —— `Encoder.h:421`，处理相对立即数时调用方需自行换算。
  - `ZydisEncoderEncodeInstructionAbsolute(request, buffer, &length, runtime_address)` —— `Encoder.h:439`，**推荐**：编码器预测长度并回写相对操作数，patch 场景天然匹配。
- 辅助：`ZydisEncoderDecodedInstructionToEncoderRequest`（`Encoder.h:459`）能把已解码指令转回 encoder request，最小版用不到（无解码回路）。

### 3.2 字符串→枚举：无官方 API，自建反查表
- Zydis 只提供枚举→字符串方向：`ZydisMnemonicGetString`（`Mnemonic.h:65`，小写）、`ZydisRegisterGetString`（`Register.h:297`，小写）。
- 底层数组 `STR_MNEMONIC[]`（`src/Generated/EnumMnemonic.inc:1888`，1886 项）、`STR_REGISTERS[]`（`EnumRegister.inc:353`，331 项）都是 `static` 内部链接，不可直接 `extern` 引用。
- **策略**：首次使用时循环调用公开 getter 自建 `unordered_map<string, enum>`，键做小写归一化。一次性 O(n)，之后 O(1) 查表，零运行期维护成本。
  ```cpp
  static const std::unordered_map<std::string, ZydisMnemonic>& mnem_table() {
      static auto* t = []{
          auto* m = new std::unordered_map<std::string, ZydisMnemonic>;
          for (int i = ZYDIS_MNEMONIC_MIN; i <= ZYDIS_MNEMONIC_MAX_VALUE; ++i)
              if (const char* s = ZydisMnemonicGetString((ZydisMnemonic)i))
                  (*m)[s] = (ZydisMnemonic)i;   // s 已是小写
          return m;
      }();
      return *t;
  }
  ```
  寄存器同理（循环 `ZYDIS_REGISTER_MIN..ZYDIS_REGISTER_MAX_VALUE`）。注意边界常量名以头文件实际定义为准（查 `Mnemonic.h` / `Register.h` 的 `*_MAX_VALUE`，下限用 `0` 起步并跳过 `ZYDIS_*_INVALID` 即可）。

### 3.3 process_arch → ZydisMachineMode 映射
参照 `ui/zydis_disassembler.cpp:32-47` 的现成写法：

| process_arch | ZydisMachineMode | ZydisStackWidth |
|---|---|---|
| `x86_64` | `ZYDIS_MACHINE_MODE_LONG_64` | `ZYDIS_STACK_WIDTH_64` |
| `x86_32` | `ZYDIS_MACHINE_MODE_LEGACY_32` | `ZYDIS_STACK_WIDTH_32` |
| `unknown` | （拒绝编码） | — |

`process_arch` 定义在 `type/process_arch.h`，运行时由 `process_manager::instance().memory()->architecture()` 获取。

### 3.4 内存写入已具备
- `IMemoryAccessor::write`（`core/imemory_accessor.h:17`）纯虚。
- `Win32MemoryAccessor::write`（`core/win32_memory_accessor.cpp:79-115`）：先 `WriteProcessMemory`，失败则 `VirtualQueryEx`+`VirtualProtectEx` 改 `PAGE_READWRITE`/`PAGE_EXECUTE_READWRITE`（保留执行位）后重写，再恢复原保护。**只读/写拷贝/守护页都能落盘**，无需改内存访问层。

### 3.5 ZydisEncoderRequest 字段速查（`Encoder.h:269-399`）
| 字段 | 类型 | 最小版取值 |
|---|---|---|
| `machine_mode` | `ZydisMachineMode` | 由 arch 决定（见上表） |
| `allowed_encodings` | `ZydisEncodableEncoding` | `ZYDIS_ENCODABLE_ENCODING_DEFAULT`（默认） |
| `mnemonic` | `ZydisMnemonic` | 查反查表 |
| `prefixes` | `ZydisInstructionAttributes` | 0，或 `ZYDIS_ATTRIB_HAS_LOCK`/`HAS_REP` 等 OR |
| `branch_type` | `ZydisBranchType` | 非 0 时填；跳转/调用用 `ZYDIS_BRANCH_TYPE_*`，可填 `NONE` 让编码器自选 short/near |
| `branch_width` | `ZydisBranchWidth` | `ZYDIS_BRANCH_WIDTH_NONE` 让自选 |
| `address_size_hint` | `ZydisAddressSizeHint` | `NONE` |
| `operand_size_hint` | `ZydisOperandSizeHint` | 显式写 `dword ptr` 等时按需填 `_8/_16/_32/_64` |
| `operand_count` | `ZyanU8` | 显式操作数个数 |
| `operands[5]` | `ZydisEncoderOperand[ZYDIS_ENCODER_MAX_OPERANDS]` | 见下 |
| `evex`/`mvex` | struct | 不填（AVX-512/KNC 不支持） |

`ZydisEncoderOperand`（`Encoder.h:184-264`）：
| `type` | 填充成员 |
|---|---|
| `ZYDIS_OPERAND_TYPE_REGISTER` | `reg.value`（ZydisRegister）、`reg.is4=ZYAN_FALSE` |
| `ZYDIS_OPERAND_TYPE_IMMEDIATE` | `imm.s`（有符号）/`imm.u`（无符号） |
| `ZYDIS_OPERAND_TYPE_MEMORY` | `mem.{base,index,scale,displacement,size}`（displacement 是 `ZyanI64`，size 是 `ZyanU16` 字节数） |
| `ZYDIS_OPERAND_TYPE_POINTER` | 最小版不实现 |

---

## 4. 文件清单与职责

### 新增 4 个文件

| 文件 | 职责 |
|---|---|
| `ui/assembler_engine.h` | 引擎接口声明：`asm_result`/`assembler_engine`，无 ImGui 依赖，可单测 |
| `ui/assembler_engine.cpp` | 词法+操作数解析+`ZydisEncoderRequest` 构造+编码；反查表懒加载 |
| `ui/assembler_window.h` | 窗口类声明，持有 `assembler_engine` 与 UI 缓冲 |
| `ui/assembler_window.cpp` | ImGui 渲染：地址输入/代码框/Assemble/Write/结果表格 |

### 修改 5 处

| 文件 | 改动 |
|---|---|
| `ui/ui_state.h` | 加 `show_assembler_window`/`assembler_address` 字段 |
| `ui/app_context.h` | 加 `zc::signal<uint64_t> open_assembler;` |
| `main.cpp` | 实例化 `assembler_window` + 订阅 `open_assembler` 信号 + 渲染分支 |
| `ui/memory_window.cpp` | Tools 菜单 Auto assembly + 反汇编行右键 Assemble here，emit 信号 |
| `Imgui_Study.pro` | SOURCES/HEADERS 加 4 个新文件 |

---

## 5. 汇编解析器详细设计

### 5.1 公共数据结构（`assembler_engine.h`）
```cpp
#pragma once
#include <Zydis/Zydis.h>
#include "type/process_arch.h"
#include <cstdint>
#include <string>
#include <vector>

// 单条指令编码结果。
struct asm_line_result {
    bool            ok = false;
    std::string     source;        // 原始行文本（去注释后）
    uint64_t        runtime_addr = 0;  // 该指令运行时地址 = 起始+前面指令长度和
    std::vector<uint8_t> bytes;   // 成功时机器码
    std::string     error;        // 失败原因（ok=true 时为空）
};

struct asm_result {
    bool            ok = false;   // 全部行成功才 true
    process_arch   arch = process_arch::unknown;
    std::vector<asm_line_result> lines;
    // 便利：成功时所有 bytes 顺序拼接
    std::vector<uint8_t> flatten() const;
};

class assembler_engine {
public:
    void set_arch(process_arch a);   // 架构变化时重建 machine_mode（反查表不变）
    // text 多行，base_addr 为首条指令运行时地址（用于绝对编码 / RIP 相对）
    asm_result assemble(const std::string& text, uint64_t base_addr);
private:
    process_arch   arch_ = process_arch::unknown;
    ZydisMachineMode mode_ = ZYDIS_MACHINE_MODE_LONG_64;
    bool           ready_ = false;
};
```

### 5.2 词法（行预处理）
逐行处理，对每行：
1. 截断到 `;` 或 `//` 之前（行内注释）。
2. 去首尾空白；空行跳过。
3. 按空白拆出**助记符 token**（第一个 token），转小写查反查表 `mnem_table()`。查不到 → `error="unknown mnemonic: xxx"`。
4. 剩余部分按**顶层逗号**拆操作数（注意逗号在 `[...]` 内不算分隔符，需用括号深度计数）。
5. 每个操作数 token 去首尾空白，交 `parse_operand`。

> 大小写：助记符、寄存器、`ptr` 关键字一律小写归一化后查表；立即数/位移按各自进制解析。

### 5.3 操作数语法（最小版 BNF）
```
operand   := [size_ptr] ( register | memory | immediate )
size_ptr  := ('byte'|'word'|'dword'|'qword'|'movs'|'word'|...) 'ptr'   ; 仅内存/立即消歧
register  := reg_name                  ; 查 reg_table() → ZYDIS_REGISTER_*
memory    := [seg ':'] '[' expr ']'    ; expr 见下
immediate := ('0x' hex) | (dec) | ('-' (hex|dec)) | char_literal
expr      := [base] ['+' index ['*' scale]] ['+'|'-' disp]
            | disp                     ; [0x403000] 绝对
base      := register
index     := register
scale     := '1'|'2'|'4'|'8'
disp      := immediate
```

`parse_operand` 返回内部中间结构，再由 `build_encoder_request` 转成 `ZydisEncoderOperand`：
```cpp
enum class op_kind { reg, imm, mem };
struct operand {
    op_kind kind;
    ZydisRegister reg = ZYDIS_REGISTER_NONE;
    // mem
    ZydisRegister mem_base  = ZYDIS_REGISTER_NONE;
    ZydisRegister mem_index = ZYDIS_REGISTER_NONE;
    uint8_t      mem_scale = 0;     // 0 表示无 index
    int64_t      mem_disp  = 0;
    uint16_t     mem_size  = 0;     // 字节数，由 ptr 修饰或寄存器宽度推断
    // imm
    int64_t      imm = 0;
    // 段覆盖
    ZydisRegister seg = ZYDIS_REGISTER_NONE;   // es/cs/ss/ds/fs/gs
};
```

### 5.4 大小推断规则（关键，消歧）
Zydis 编码器对部分指令需要显式操作数大小。策略：
- 显式 `ptr` 修饰 → `mem_size` 直接取（byte=1, word=2, dword=4, qword=8, movs=? 留 TODO）。
- 无 `ptr` 时：若另一操作数是寄存器，按寄存器宽度推 mem_size（`rax`→8, `eax`→4, `ax`→2, `al`→1）；查 `ZydisRegisterGetClass`/宽度表。最小版可内置一张 `ZydisRegister→宽度` 小表（从 `ZydisRegisterGetClass`+class→size 推，或直接枚举常见寄存器）。
- 纯立即数（如 `add eax, 1`）无需 size；编码器按助记符+寄存器推断。
- 两操作数都无寄存器宽度的罕见情况 → 报错「ambiguous operand size, use ptr」。

### 5.5 ZydisEncoderRequest 填充流程（`build_encoder_request`）
```
req = {};   // 零初始化
req.machine_mode = mode_;
req.allowed_encodings = ZYDIS_ENCODABLE_ENCODING_DEFAULT;
req.mnemonic = <查表>;
req.prefixes = <前缀 OR，见 5.6>;
req.branch_type = ZYDIS_BRANCH_TYPE_NONE;   // 让编码器自选 short/near
req.branch_width = ZYDIS_BRANCH_WIDTH_NONE;
// operand_size_hint: 若有显式 ptr 且对方无寄存器消歧，按 ptr 设 _8/_16/_32/_64
req.operand_count = N;
for (i, op in operands)
    switch op.kind:
      reg:  operands[i].type=REGISTER; operands[i].reg.value=op.reg;
      imm:  operands[i].type=IMMEDIATE; operands[i].imm.s=op.imm;
      mem:  operands[i].type=MEMORY;
            operands[i].mem.base=op.mem_base; .index=op.mem_index;
            .scale=op.mem_scale?op.mem_scale:0;
            .displacement=op.mem_disp; .size=op.mem_size;
            // 段覆盖通过 prefixes 的 ZYDIS_ATTRIB_HAS_SEGMENT_* 表达
            if (op.seg) req.prefixes |= attrib_for_segment(op.seg);
```
然后：
```cpp
uint8_t buf[ZYDIS_MAX_INSTRUCTION_LENGTH];   // 15
ZyanUSize len = sizeof(buf);
ZyanStatus st = ZydisEncoderEncodeInstructionAbsolute(&req, buf, &len, runtime_addr);
if (!ZYAN_SUCCESS(st)) error = format("encode failed: 0x%08X", st);
else bytes.assign(buf, buf+len);
```
> 用 `EncodeInstructionAbsolute` 而非 `EncodeInstruction`：前者预测长度并回写相对立即数（`jmp rel`/`call rel`/RIP 相对 `[rip+disp]`），patch 场景天然需要。

### 5.6 前缀映射
| 文本前缀 | 位 |
|---|---|
| `lock` | `ZYDIS_ATTRIB_HAS_LOCK` |
| `rep` | `ZYDIS_ATTRIB_HAS_REP` |
| `repe`/`repz` | `ZYDIS_ATTRIB_HAS_REPE` |
| `repne`/`repnz` | `ZYDIS_ATTRIB_HAS_REPNE` |
| `bnd` | `ZYDIS_ATTRIB_HAS_BND` |
| `notrack` | `ZYDIS_ATTRIB_HAS_NOTRACK` |
| `cs:/ds:/es:/fs:/gs:/ss:`（段覆盖，写在内存操作数前） | `ZYDIS_ATTRIB_HAS_SEGMENT_*` |

前缀作为助记符前的「前缀 token」识别（如 `lock add [rax], 1`），或内存操作数的 `seg:` 段覆盖。两者都汇入 `req.prefixes`。

### 5.7 错误模型
每行独立成败，失败信息含**行号**（从 1 计，含空行/注释行编号）+原因：
- `line 3: unknown mnemonic 'movq'`
- `line 5: bad operand '[rax+'` —— 词法不闭合
- `line 7: encode failed: 0xC000000` —— Zydis 编码失败（用 `ZydisFormatter` 无法解，直接给状态码；可选地用 `ZyanStatusGetModule` 取模块名）
- `line 9: ambiguous operand size, use ptr`

`asm_result.ok` 仅当所有行 `ok`。Write 按钮在 `!ok` 时禁用。

---

## 6. 支持的指令/操作数清单

### 指令（助记符覆盖，靠反查表自动覆盖 Zydis 全集，下列为验证目标）
| 助记符 | 示例 | 预期机器码（x64） | 备注 |
|---|---|---|---|
| `nop` | `nop` | `90` | |
| `ret` | `ret` | `C3` | |
| `int3` | `int3` | `CC` | |
| `mov` | `mov eax, 1` | `B8 01 00 00 00` | imm→reg |
| `mov` | `mov [rax], eax` | `89 00` | reg→mem |
| `mov` | `mov rax, 0x403000` | `48 B8 00 30 40 00 00 00 00 00` | imm64→reg（movabs） |
| `add` | `add eax, 1` | `83 C0 01` | |
| `sub` | `sub esp, 8` | `83 EC 08` | |
| `xor` | `xor eax, eax` | `31 C0` | |
| `cmp` | `cmp eax, 0` | `83 F8 00` | |
| `jmp` | `jmp 0x401000` | `E9 rel32`（5B）或 `EB rel8`（2B） | 编码器自选宽度，注意 patch 余量 |
| `je`/`jz` | `je 0x401020` | `74 rel8`/`0F 84 rel32` | 条件跳转 |
| `call` | `call 0x401050` | `E8 rel32` | |
| `push`/`pop` | `push rax` / `pop rax` | `50` / `58` | |
| `lea` | `lea rax, [rcx+8]` | `48 8D 41 08` | |
| `lock` | `lock add [rax], 1` | `F0 83 00 01` | 前缀 |
| `nop dword [rax]` | `nop dword ptr [rax]` | `0F 1F 00` | 多字节 nop |

> 指令覆盖本质由反查表（Zydis 全集 1886 个助记符）保证，上表是**回归测试**用例，不是白名单。

### 操作数形式
| 形式 | 解析路径 | ZydisEncoderRequest 填充 |
|---|---|---|
| `eax`/`rax`/`al`/`r8d`... | reg 表查中 | type=REGISTER, reg.value=enum |
| `0x40`/`64`/`-1` | 立即数 | type=IMMEDIATE, imm.s |
| `[rax]` | mem: base=rax | type=MEMORY, base, size 待推断 |
| `[rax+8]` | mem: base+disp | base, disp=8 |
| `[rax*4]` | mem: index*scale | index=rax, scale=4 |
| `[rcx+rax*8+0x10]` | SIB 完整 | base+index+scale+disp |
| `[0x403000]` | 绝对地址 mem | displacement=0x403000, base=NONE（64 位下注意 address_size_hint） |
| `dword ptr [rax]` | ptr 修饰 | mem.size=4 |
| `fs:[0x30]` | 段覆盖+绝对 | seg=fs, prefixes|=HAS_SEGMENT_FS |
| `byte ptr [rip+0x10]` | RIP 相对 | index=NONE, base=RIP, disp=0x10（编码器经 Absolute 版回写） |

---

## 7. UI 窗口布局（CE Auto-Assemble 风格）

```
┌── Auto-Assemble ────────────────────────────────────────────┐
│ Address: [0x401000________]  Arch: x64        [Assemble][Write][Clear] │
├──────────────────────────────────────────────────────────────┤
│ mov  eax, 1                          ← InputTextMultiline   │
│ ret                                                        │
│                                                            │
├──────────────────────────────────────────────────────────────┤
│ # │ Address       │ Bytes          │ Source          │ Err  │  ← 结果表格
│ 1 │ 00000000401000│ B8 01 00 00 00│ mov eax, 1     │      │
│ 2 │ 00000000401005│ C3            │ ret            │      │
├──────────────────────────────────────────────────────────────┤
│ Status: 2 lines, 6 bytes. (or) Line 2: encode failed 0x..  │
└──────────────────────────────────────────────────────────────┘
```

### 交互
- **打开即填地址**：从右键「Assemble here」来时 `addr_buf` 预填该行地址；从菜单来时为空，焦点置地址框。
- **Assemble**：仅编码，不写进程；填结果表格；`ok=false` 时 Status 行红字显示首个错误行。
- **Write**：先 Assemble，`ok` 才允许点；从 `addr_buf` 解析起始地址，逐条 `memory->write(line.runtime_addr, line.bytes)`（或一次拼 flatten 后单次 write 覆盖连续区间——注意跨内存区边界可能失败，逐条更稳）。写完刷新结果表格 Status。
- **未附加进程**：禁用 Assemble/Write，提示「No process attached.」
- **架构标签**：随 `process_arch` 实时显示 x64/x32；切换进程自动 `engine_.set_arch()`。
- **可选**：Write 前在结果表格加「原始字节」列（`memory->read` 当前字节），供人工对照（不持久化备份，符合边界）。

---

## 8. 接入点（代码骨架，非完整产品代码）

### 8.1 `ui/ui_state.h` 加字段
```cpp
// 在 ui_state 末尾加：
bool     show_assembler_window = false;
uint64_t assembler_address     = 0;   // 右键预填地址；0=空白打开
```

### 8.2 `ui/app_context.h` 加信号
```cpp
// 在 application_context 的信号区加（与 open_memory_viewer 并列）：
zc::signal<uint64_t> open_assembler;   // 地址参数；0 表示空白打开
```

### 8.3 `main.cpp` 注册窗口 + 订阅
```cpp
// 实例化（与 hex_window 并列，约 :89）：
memory_window   hex_window(g_ui_state);
assembler_window asm_window(g_ui_state);     // 新增

// 订阅信号（与 open_memory_viewer 订阅并列，约 :94）：
app_ctx.open_assembler.connect([](uint64_t addr) {
    g_ui_state.show_assembler_window = true;
    if (addr) g_ui_state.assembler_address = addr;
});

// 渲染循环（与 show_memory_window 分支并列，约 :162）：
if (g_ui_state.show_assembler_window) {
    asm_window.render();
}
```

### 8.4 `ui/memory_window.cpp` 两处 emit
```cpp
// Tools 菜单（约 :659，替换 // TODO）：
if (ImGui::MenuItem("Auto assembly")) {
    application_context::instance().open_assembler.emit(0);
}

// 反汇编行右键菜单（约 :353，BeginPopupContextItem 内，加一项）：
if (ImGui::MenuItem("Assemble here")) {
    application_context::instance().open_assembler.emit(ln.address);
}
```
> memory_window 已持有 `ui_state& state_`，但跨模块通讯走 app_ctx 信号是项目既定模式（见 app_context.h 注释「面板只发信号，由 main.cpp 统一改 ui_state」）。

### 8.5 `ui/assembler_window.h` 骨架
```cpp
#pragma once
#include "ui_state.h"
#include "ui/assembler_engine.h"
class assembler_window {
public:
    explicit assembler_window(ui_state& s) : state_(s) {}
    void render();
private:
    void do_assemble();
    void do_write();
    ui_state&          state_;
    assembler_engine  engine_;
    char               addr_buf_[32] = {};
    char               code_buf_[0x2000] = {};
    asm_result         last_;
    std::string        status_;        // 状态行文本（错误/成功计数）
};
```

### 8.6 `ui/assembler_window.cpp` 关键逻辑骨架
```cpp
void assembler_window::render() {
    auto& pm = process_manager::instance();
    const process_arch arch = pm.is_attached() ? pm.memory()->architecture()
                                               : process_arch::unknown;
    engine_.set_arch(arch);

    if (!ImGui::Begin("Auto-Assemble", &state_.show_assembler_window,
                      ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }

    // 首次打开且右键预填过地址 → 同步到 addr_buf（只一次）
    if (state_.assembler_address && !addr_buf_[0]) {
        snprintf(addr_buf_, sizeof(addr_buf_), "%llX",
                 (unsigned long long)state_.assembler_address);
    }

    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##addr", "Address (hex)", addr_buf_, sizeof(addr_buf_));
    ImGui::SameLine(); ImGui::TextDisabled("Arch: %s",
        arch==process_arch::x86_64?"x64":arch==process_arch::x86_32?"x32":"-");
    ImGui::SameLine();
    if (ImGui::Button("Assemble")) do_assemble();
    ImGui::SameLine();
    ImGui::BeginDisabled(!last_.ok || arch==process_arch::unknown);
    if (ImGui::Button("Write"))   do_write();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { code_buf_[0]=0; last_={}; status_.clear(); }

    ImGui::InputTextMultiline("##code", code_buf_, sizeof(code_buf_),
                              ImVec2(-1, 200), ImGuiInputTextFlags_AllowTabInput);

    // 结果表格：# / Address / Bytes / Source / Err
    if (!last_.lines.empty() && ImGui::BeginTable("##asm_result",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,160))) {
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Error"); ImGui::TableHeadersRow();
        for (int i=0;i<(int)last_.lines.size();++i) {
            auto& ln=last_.lines[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", i+1);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%016llX",(ull)ln.runtime_addr);
            ImGui::TableSetColumnIndex(2); {
                std::string b; for(auto c:ln.bytes){char s[4];snprintf(s,4,"%02X ",c);b+=s;}
                if(!ln.ok) ImGui::PushStyleColor(ImGuiCol_Text,{1,0.4f,0.4f,1});
                ImGui::TextUnformatted(b.c_str());
                if(!ln.ok) ImGui::PopStyleColor();
            }
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(ln.source.c_str());
            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(ln.error.c_str());
        }
        ImGui::EndTable();
    }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
    ImGui::End();
}

void assembler_window::do_assemble() {
    uint64_t base=0; parse_address_text(addr_buf_, base); // 复用 memory_window 的，或自备
    last_ = engine_.assemble(code_buf_, base);
    if (!last_.ok) {
        for (auto& l:last_.lines) if(!l.ok){status_="Line "+...+": "+l.error; break;}
    } else {
        status_="OK: "+std::to_string(last_.lines.size())+" insns, "
                +std::to_string(last_.flatten().size())+" bytes.";
    }
}

void assembler_window::do_write() {
    auto* mem=process_manager::instance().memory();
    if(!mem){status_="no process";return;}
    size_t ok=0, fail=0;
    for (auto& l:last_.lines) if(l.ok){
        if (mem->write(l.runtime_addr, l.bytes.data(), l.bytes.size())) ++ok;
        else {++fail; l.error="write failed";}
    }
    status_="wrote "+std::to_string(ok)+" insns"+(fail?", "+std::to_string(fail)+" failed":"");
}
```
（`parse_address_text` 见 `ui/memory_window.cpp:29`，可抽到公共头复用。）

---

## 9. 实现步骤（建议顺序）

1. **引擎核心** `assembler_engine.cpp`
   - 反查表（mnem/reg）懒加载函数。
   - 行预处理（注释/空行/拆助记符+操作数，括号深度计数处理 `[..,..]`）。
   - `parse_operand`：先尝试 reg，再 immediate，再 memory（`[`开头）；memory 内部用 `+`/`-`/`*` 拆 base/index/scale/disp。
   - `build_encoder_request` + `ZydisEncoderEncodeInstructionAbsolute`。
   - 大小推断（ptr 优先，其次对方寄存器宽度，否则报错）。
   - 先跑通 `nop`/`ret`/`mov eax,1`/`mov [rax],eax`/`jmp`。

2. **窗口 UI** `assembler_window.cpp`
   - 先只做 Assemble 预览（不 Write），验证编码结果表格。
   - 再加 Write + 状态行。

3. **接入** ui_state/app_context/main.cpp/memory_window.cpp 四处改动 + .pro。

4. **构建验证** qmake + make/jom，确认编译通过。

5. **联调**：附加真实进程 → 反汇编视图右键 Assemble here → 输入 `nop` → Write → 反汇编刷新确认该地址变 `nop`。

---

## 10. 测试用例

### 10.1 引擎单测（固定文本→预期机器码）
| 输入（x64, base=0x401000） | 预期 bytes | 备注 |
|---|---|---|
| `nop` | `90` | |
| `ret` | `C3` | |
| `int3` | `CC` | |
| `mov eax, 1` | `B8 01 00 00 00` | imm32→reg32 |
| `mov rax, 1` | `48 C7 C0 01 00 00 00` | imm32→reg64（符号扩展） |
| `xor eax, eax` | `31 C0` | |
| `add eax, 1` | `83 C0 01` | |
| `mov [rax], eax` | `89 00` | reg→mem, size 由 eax 推断=4 |
| `mov dword ptr [rax], 1` | `C7 00 01 00 00 00` | 显式 size |
| `lea rax, [rcx+8]` | `48 8D 41 08` | |
| `push rax` | `50` | |
| `nop dword ptr [rax]` | `0F 1F 00` | 多字节 nop |
| `lock add [rax], 1` | `F0 83 00 01` | 前缀 |
| `jmp 0x401005` | `EB 03`（2B）或 `E9 ..`（5B） | 编码器自选宽度 |
| `jmp 0x500000` | `E9 rel32`（5B） | 超出 short 范围 |
| `call 0x401050` | `E8 rel32`（5B） | |
| `je 0x401002` | `74 00`（近，2B） | |

> jmp/jcc 宽度由 `branch_type=NONE` 让编码器自选；若要强制 short/near，填 `branch_width`。**patch 时注意：若原指令 5 字节、新指令 2 字节，多余 3 字节需用 nop 填充**——最小版不自动填充，由用户在文本里补 `nop`（文档应提示此风险）。

### 10.2 端到端（live_ui）
- 附加测试进程 → Assemble `nop; ret` → Write → `memory->read` 验证字节为 `90 C3`。

### 10.3 错误用例
| 输入 | 预期错误 |
|---|---|
| `movq eax, 1` | `unknown mnemonic 'movq'`（Zydis 无此名） |
| `mov [rax` | `unmatched '['` |
| `mov [rax], [rbx]` | `encode failed 0x..`（双 mem 非法） |
| `add [rax], 1`（无 ptr、无 reg） | `ambiguous operand size, use ptr` |

---

## 附录 A：风险与提示

1. **patch 宽度差**：替换指令若比原指令短，剩余字节不会自动 nop，会残留旧机器码导致反汇编错位。窗口应在结果表格显示每条字节长度，并在 Write 前对「覆盖区间总长 vs 用户未填 nop」给出 warning（非阻塞）。
2. **相对跳转目标在覆盖区间外**：`jmp rel` 的 rel 基于新指令地址计算，`EncodeInstructionAbsolute` 已正确处理；但若用户脚本里 label 跨行引用（最小版不支持 label），目标地址需手工算好填绝对地址。
3. **32 位进程的绝对地址 mem**：`mov [0x403000], eax` 在 32 位下 displacement 占满地址，需 `address_size_hint=32`；64 位下默认 64 位绝对（movabs 风格或 RIP 相对，编码器择优）。最小版先按编码器默认，遇歧义报错提示加 ptr。
4. **反查表边界**：`ZYDIS_MNEMONIC_*`/`ZYDIS_REGISTER_*` 的 MIN/MAX 常量以头文件 `*_MAX_VALUE` 为准；循环上限用 `<= MAX_VALUE`，下限从 `ZYDIS_MNEMONIC_INVALID+1` 类起步。`ZydisMnemonicGetString` 对无效枚举返回 `NULL`，需判空。
5. **多行文本缓冲**：`code_buf_` 用 `0x2000` 起步，足够放典型 patch 脚本；`InputTextMultiline` 配 `AllowTabInput` 便于缩进。

## 附录 B：后续扩展路径（最小版预留）

- `symbol_map`：`unordered_map<string,uint64_t>`，最小版空。加 label 时：两遍扫描——第一遍收集 `labelname:` → 地址（按指令长度累加），第二遍把操作数里的 label 名替换为查到的绝对地址再编码。
- `aobscan`：扩展 `IMemoryAccessor` 或在引擎外加一层，扫描目标进程特征码得地址填入 symbol_map。
- `alloc`：扩展 `IMemoryAccessor` 加 `alloc(size)`（`VirtualAllocEx` + `PAGE_EXECUTE_READWRITE`），返回基址入 symbol_map。
- `[enable]/[disable]` 块：按行标记段，分别编码写入/还原（还原需持久化原字节，最小版没存）。

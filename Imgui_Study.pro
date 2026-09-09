TEMPLATE = app
CONFIG += console c++20
CONFIG -= app_bundle
CONFIG -= qt

DEFINES += UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
DEFINES += __AVX2__
DEFINES += ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD

QMAKE_CXXFLAGS += -mavx2 -mbmi2

INCLUDEPATH += $$PWD $$PWD/libs $$PWD/libs/Imgui $$PWD/ui $$PWD/core $$PWD/scan $$PWD/type
INCLUDEPATH += $$PWD/libs/Zydis/include $$PWD/libs/Zydis/src $$PWD/libs/Zycore/include
INCLUDEPATH += $$PWD/libs/S_inject

SOURCES += \
    core/process_manager.cpp \
    core/win32_memory_accessor.cpp \
    core/win32_memory_region_enumerator.cpp \
    core/win32_module_enumerator.cpp \
    core/win32_process_enumerator.cpp \
    scan/scan_engine.cpp \
    scan/scan_service.cpp \
    scan/scan_result_repository.cpp \
    scan/scan_data_provider.cpp \
    scan/process_memory_snapshot_manager.cpp \
    scan/win32_process_memory_snapshot.cpp \
    libs/Imgui/imgui.cpp \
    libs/Imgui/imgui_draw.cpp \
    libs/Imgui/imgui_tables.cpp \
    libs/Imgui/imgui_widgets.cpp \
    libs/Imgui/imgui_demo.cpp \
    libs/Imgui/imgui_impl_win32.cpp \
    libs/Imgui/imgui_impl_dx11.cpp \
    libs/Imgui/TextEditor.cpp \
    main.cpp \
    ui/address_list_panel.cpp \
    ui/function_graph.cpp \
    ui/symbol_table.cpp \
    ui/assembler_window.cpp \
    ui/debug_panel.cpp \
    ui/file_browser.cpp \
    ui/hex_view.cpp \
    ui/inject_window.cpp \
    ui/zydis_disassembler.cpp \
    ui/memory_window.cpp \
    ui/process_detail_window.cpp \
    ui/process_list_window.cpp \
    ui/process_icon_cache.cpp \
    ui/scan_panel.cpp \
    ui/result_panel.cpp \
    ui/settings_window.cpp \
    ui/top_menu.cpp \
    libs/S_inject/src/Injector.cpp \
    libs/S_inject/src/S-Wisper.c \
    libs/S_inject/src/crypto.cpp \
    libs/S_inject/src/error.cpp \
    libs/S_inject/src/helper.cpp \
    libs/S_inject/src/network.cpp \
    libs/S_inject/src/poolparty/HandleHijacker.cpp \
    libs/S_inject/src/poolparty/Misc.cpp \
    libs/S_inject/src/poolparty/Native.cpp \
    libs/S_inject/src/poolparty/PoolParty.cpp \
    libs/S_inject/src/poolparty/ThreadPool.cpp \
    libs/S_inject/src/poolparty/WinApi.cpp \
    libs/S_inject/src/poolparty/WorkerFactory.cpp \
    libs/S_inject/src/app/S-Wisper-asm-x64.S \
    libs/Zydis/src/Zydis.c \
    libs/Zydis/src/MetaInfo.c \
    libs/Zydis/src/Mnemonic.c \
    libs/Zydis/src/Register.c \
    libs/Zydis/src/SharedData.c \
    libs/Zydis/src/String.c \
    libs/Zydis/src/Utils.c \
    libs/Zydis/src/Decoder.c \
    libs/Zydis/src/DecoderData.c \
    libs/Zydis/src/Encoder.c \
    libs/Zydis/src/EncoderData.c \
    libs/Zydis/src/Disassembler.c \
    libs/Zydis/src/Formatter.c \
    libs/Zydis/src/FormatterBuffer.c \
    libs/Zydis/src/FormatterATT.c \
    libs/Zydis/src/FormatterBase.c \
    libs/Zydis/src/FormatterIntel.c \
    libs/Zydis/src/Segment.c \
    libs/Zycore/src/API/Memory.c \
    libs/Zycore/src/API/Process.c \
    libs/Zycore/src/API/Synchronization.c \
    libs/Zycore/src/API/Terminal.c \
    libs/Zycore/src/API/Thread.c \
    libs/Zycore/src/Allocator.c \
    libs/Zycore/src/ArgParse.c \
    libs/Zycore/src/Bitset.c \
    libs/Zycore/src/Format.c \
    libs/Zycore/src/List.c \
    libs/Zycore/src/ZycoreString.c \
    libs/Zycore/src/Vector.c \
    libs/Zycore/src/Zycore.c

LIBS += -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -ldwmapi -lshell32
# S-inject backend link-time dependencies
LIBS += -lntdll -lwininet -lbcrypt -ladvapi32 -lpsapi -lws2_32 -lcrypt32

HEADERS += \
    core/imemory_accessor.h \
    core/imemory_region_enumerator.h \
    core/imodule_enumerator.h \
    core/iprocess_enumerator.h \
    core/process_manager.h \
    core/string_conversion.h \
    core/win32_memory_accessor.h \
    core/win32_memory_region_enumerator.h \
    core/win32_module_enumerator.h \
    core/win32_process_enumerator.h \
    core/event/signal.h \
    libs/BS_thread_pool.hpp \
    scan/iscan_value_provider.h \
    scan/iprocess_memory_snapshot.h \
    scan/scan_data_provider.h \
    scan/scan_engine.h \
    scan/scan_result_repository.h \
    scan/scan_service.h \
    scan/process_memory_snapshot_manager.h \
    scan/win32_process_memory_snapshot.h \
    scan/sparse_memory_snapshot.h \
    scan/live_process_memory_snapshot.h \
    scan/adaptive_cache.h \
    scan/thread_pool.h \
    scan/scan_simd_accelerate.h \
    scan/temp_path_manager.h \
    scan/encoding_formatter.h \
    scan/scan_value_parser.h \
    scan/scan_value_target.h \
    scan/scan_result_store.h \
    type/scan_data_stream_define.h \
    type/memory_region.h \
    type/module_info.h \
    type/process_info.h \
    type/value_type.h \
    type/process_arch.h \
    ui/address_list_panel.h \
    ui/assembler_window.h \
    ui/debug_panel.h \
    ui/file_browser.h \
    ui/hex_view.h \
    ui/inject_window.h \
    ui/symbol_table.h \
    ui/zydis_disassembler.h \
    ui/memory_window.h \
    ui/function_graph.h \
    ui/process_detail_window.h \
    ui/process_list_window.h \
    ui/process_icon_cache.h \
    ui/top_menu.h \
    ui/ui_state.h \
    ui/theme.h \
    ui/scan_panel.h \
    ui/result_panel.h \
    ui/app_context.h \
    ui/settings_window.h \
    ui/address_value.h

DISTFILES += \
    tests/scan_perf.exe


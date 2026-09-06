TEMPLATE = app
CONFIG += console c++20
CONFIG -= app_bundle
CONFIG -= qt

DEFINES += UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
DEFINES += __AVX2__

QMAKE_CXXFLAGS += -mavx2 -mbmi2

INCLUDEPATH += $$PWD $$PWD/libs $$PWD/libs/Imgui $$PWD/ui $$PWD/core $$PWD/scan $$PWD/type

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
    main.cpp \
    ui/address_list_panel.cpp \
    ui/debug_panel.cpp \
    ui/process_detail_window.cpp \
    ui/process_list_window.cpp \
    ui/scan_panel.cpp \
    ui/result_panel.cpp \
    ui/settings_window.cpp \
    ui/top_menu.cpp

LIBS += -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -ldwmapi

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
    ui/address_list_panel.h \
    ui/debug_panel.h \
    ui/process_detail_window.h \
    ui/process_list_window.h \
    ui/top_menu.h \
    ui/ui_state.h \
    ui/scan_panel.h \
    ui/result_panel.h \
    ui/app_context.h \
    ui/settings_window.h \
    ui/address_value.h

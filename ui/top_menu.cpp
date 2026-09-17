#include "top_menu.h"
#include "imgui.h"

#include <processthreadsapi.h>


void top_menu::render() {
    if(ImGui::BeginMenuBar()){
        if(ImGui::BeginMenu("文件")){
            if(ImGui::MenuItem("保存")){

            }

            if(ImGui::MenuItem("加载")){

            }

            if(ImGui::MenuItem("打开进程", "O")){
                state_.show_process_window = true;
            }

            if(ImGui::MenuItem("退出", "Alt+F4")) ExitProcess(0);

            ImGui::EndMenu();
        }

        if(ImGui::BeginMenu("编辑")){
            if(ImGui::MenuItem("设置")){
                state_.show_settings_window = true;
            }
            ImGui::EndMenu();
        }

        if(ImGui::BeginMenu("关于")){
            if(ImGui::MenuItem("关于本程序")){
                state_.show_about_window = true;
            }
            if(ImGui::MenuItem("调试面板")){
                state_.show_about_window = false;
                state_.show_debug_window = true;
            }
            ImGui::EndMenu();
        }


        ImGui::EndMenuBar();
    }
}

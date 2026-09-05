#include "top_menu.h"
#include "imgui.h"

#include <processthreadsapi.h>


void top_menu::render() {
    if(ImGui::BeginMenuBar()){
        if(ImGui::BeginMenu("File")){
            if(ImGui::MenuItem("Exit", "Alt+F4")) ExitProcess(0);
            if(ImGui::MenuItem("Opnen Process", "O")){
                state_.show_process_window = true;
            }

            ImGui::EndMenu();
        }

        if(ImGui::BeginMenu("Edit")){
            // if(ImGui::MenuItem("Settings"))
            ImGui::EndMenu();
        }

        if(ImGui::BeginMenu("About")){
            if(ImGui::MenuItem("About CheatEngine")){
                state_.show_about_window = true;
            }
            if(ImGui::MenuItem("Debug Panel")){
                state_.show_about_window = false;
                state_.show_debug_window = true;
            }
            ImGui::EndMenu();
        }

        // if(ImGui::MenuItem("test")) MessageBoxW(nullptr, L"Test", L"Captain", MB_OK);

        ImGui::EndMenuBar();
    }
}

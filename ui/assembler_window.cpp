#include "assembler_window.h"


void assembler_window::render(){

    if (!state_.show_assembler_window)
        return;

    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if(ImGui::Begin("Assembly Window", &state_.show_assembler_window,
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings)){
        if(ImGui::BeginMenuBar()){
            if(ImGui::BeginMenu("File")){
                if(ImGui::MenuItem("Save")){

                }

                if(ImGui::MenuItem("Open")){

                }
                ImGui::EndMenu();
            }

            if(ImGui::BeginMenu("Viewer")){
                if(ImGui::MenuItem("Syntax Highlight")){

                }

                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }
    }
    // begin里面压栈了，要弹出
    ImGui::End();
}

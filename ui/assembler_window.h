#ifndef ASSEMBLER_WINDOW_H
#define ASSEMBLER_WINDOW_H

#include "ui/ui_state.h"
#include "Imgui/imgui.h"
#include "Imgui/TextEditor.h"
#include <string>


class assembler_window
{
public:
    explicit assembler_window(ui_state& ui) : state_(ui) {}

    void render();
    void load_language();
private:
    ui_state& state_;
    // std::string code_buffer;
    TextEditor editor_;
};

#endif // ASSEMBLER_WINDOW_H

#ifndef ASSEMBLER_WINDOW_H
#define ASSEMBLER_WINDOW_H

#include "ui/ui_state.h"
#include "Imgui/imgui.h"


class assembler_window
{
public:
    explicit assembler_window(ui_state& ui) : state_(ui) {}

    void render();
private:
    ui_state& state_;
};

#endif // ASSEMBLER_WINDOW_H

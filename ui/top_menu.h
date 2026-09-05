#ifndef TOP_MENU_H
#define TOP_MENU_H

#include "ui/ui_state.h"

class top_menu
{
public:
    explicit top_menu(ui_state& ui) : state_(ui) {}
    void render();

private:
    ui_state& state_;
};

#endif // TOP_MENU_H

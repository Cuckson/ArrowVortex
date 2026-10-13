#pragma once

#include <Dialogs/Dialog.h>
#include <Core/Widgets.h>
#include <Core/WidgetsLayout.h>

#include <vector>

namespace Vortex {

class DialogCustomSnap : public EditorDialog, public InputHandler {
   public:
    ~DialogCustomSnap();
    DialogCustomSnap();

    void onTick() override;

    void onAdd();
    void onSelect();
    void onRemove();

   private:
    void myCreateWidgets();
    void myUpdateSnapList();

    double myCustomSnap;

    WgDroplist* mySnapList = nullptr;
    int mySnapIndex = 0;
    std::vector<double> mySnaps;
};

};  // namespace Vortex

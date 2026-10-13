#pragma once

#include <Dialogs/Dialog.h>
#include <Core/Widgets.h>
#include <Core/WidgetsLayout.h>

#include <string>

namespace Vortex {

class DialogSnapSequence : public EditorDialog {
   public:
    ~DialogSnapSequence();
    DialogSnapSequence();

    void onSequenceChange();
    void onSelect();

   private:
    void myCreateWidgets();

    std::string mySequence;
};

};  // namespace Vortex

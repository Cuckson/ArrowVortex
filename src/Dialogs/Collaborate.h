#pragma once

#include <Dialogs/Dialog.h>

#include <Core/WidgetsLayout.h>

namespace Vortex {

class DialogCollaborate : public EditorDialog {
   public:
    ~DialogCollaborate();
    DialogCollaborate();

    void onTick() override;

   private:
    void myCreateWidgets();
    void onHost();
    void onJoin();
    void onCopy();
    void onDisconnect();
    void onNameChanged();

    std::string myName;
    std::string myPort;
    std::string myJoinAddress;
    std::string myShareAddress;
    std::string myStatus;
};

};  // namespace Vortex

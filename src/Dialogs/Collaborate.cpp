#include <Dialogs/Collaborate.h>

#include <Core/Gui.h>
#include <Core/Widgets.h>
#include <Core/WidgetsLayout.h>

#include <Editor/Session.h>

#include <cstdlib>

namespace Vortex {

DialogCollaborate::~DialogCollaborate() = default;

DialogCollaborate::DialogCollaborate() {
    myPort = std::to_string(Session::DEFAULT_PORT);
    setTitle("COLLABORATE");
    myCreateWidgets();
}

void DialogCollaborate::myCreateWidgets() {
    myLayout.row().col(320);
    myLayout.add<WgLabel>()->text.bind(&myStatus);

    myLayout.row().col(64).col(176).col(72);

    WgLineEdit* port = myLayout.add<WgLineEdit>("Port");
    port->text.bind(&myPort);
    port->setNumerical(true);
    port->setMaxLength(5);
    port->setTooltip("Port to listen on; it must be reachable by the guest");

    WgButton* host = myLayout.add<WgButton>();
    host->text.set("Host");
    host->onPress.bind(this, &DialogCollaborate::onHost);
    host->setTooltip("Start a session with the current song");

    WgLineEdit* share = myLayout.add<WgLineEdit>("Share");
    share->text.bind(&myShareAddress);
    share->setEditable(false);
    share->setTooltip("Give this address to the guest");

    WgButton* copy = myLayout.add<WgButton>();
    copy->text.set("Copy");
    copy->onPress.bind(this, &DialogCollaborate::onCopy);

    WgLineEdit* join = myLayout.add<WgLineEdit>("Join");
    join->text.bind(&myJoinAddress);
    join->setTooltip("Host address, as ip:port");

    WgButton* joinButton = myLayout.add<WgButton>();
    joinButton->text.set("Join");
    joinButton->onPress.bind(this, &DialogCollaborate::onJoin);
    joinButton->setTooltip("Join a session; the same song must be open");

    myLayout.row().col(320);
    WgButton* disconnect = myLayout.add<WgButton>();
    disconnect->text.set("Disconnect");
    disconnect->onPress.bind(this, &DialogCollaborate::onDisconnect);
}

void DialogCollaborate::onTick() {
    myStatus = gSession->getStatus();
    myShareAddress = gSession->getShareAddress();
    EditorDialog::onTick();
}

void DialogCollaborate::onHost() {
    int port = std::atoi(myPort.c_str());
    if (port <= 0 || port > 65535) port = Session::DEFAULT_PORT;
    gSession->host(port);
}

void DialogCollaborate::onJoin() { gSession->join(myJoinAddress); }

void DialogCollaborate::onCopy() {
    std::string address = gSession->getShareAddress();
    if (!address.empty()) GuiMain::setClipboardText(address);
}

void DialogCollaborate::onDisconnect() { gSession->disconnect(); }

};  // namespace Vortex

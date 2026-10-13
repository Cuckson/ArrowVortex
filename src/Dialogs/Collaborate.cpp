#include <Dialogs/Collaborate.h>

#include <Core/Gui.h>
#include <Core/Widgets.h>
#include <Core/WidgetsLayout.h>

#include <Editor/Common.h>
#include <Editor/Session.h>

#include <cstdlib>

namespace Vortex {

DialogCollaborate::~DialogCollaborate() = default;

DialogCollaborate::DialogCollaborate() {
    myPort = std::to_string(Session::DEFAULT_PORT);
    myName = gSession->getUserName();
    setTitle("COLLABORATE");
    myCreateWidgets();
}

void DialogCollaborate::myCreateWidgets() {
    myLayout.row().col(320);
    myLayout.add<WgLabel>()->text.bind(&myStatus);

    myLayout.row().col(64).col(176).col(72);

    WgLineEdit* name = myLayout.add<WgLineEdit>("Name");
    name->text.bind(&myName);
    name->setMaxLength(Session::MAX_NAME_LENGTH);
    name->onChange.bind(this, &DialogCollaborate::onNameChanged);
    name->setTooltip("Your name, shown to the other users next to your cursor");
    myLayout.addBlank();

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
    joinButton->setTooltip(
        "Join a session. Your simfile is replaced by the host's. If you don't "
        "have the host's song open, it is downloaded to the \"saves\" "
        "folder first");

    myGuestList = myLayout.add<WgDroplist>("Guest");
    myGuestList->value.bind(&myGuestIndex);
    myGuestList->setTooltip("Guests in the session (host only)");

    WgButton* kick = myLayout.add<WgButton>();
    kick->text.set("Kick");
    kick->onPress.bind(this, &DialogCollaborate::onKick);
    kick->setTooltip("Remove the selected guest from the session");

    myLayout.row().col(320);
    WgButton* disconnect = myLayout.add<WgButton>();
    disconnect->text.set("Disconnect");
    disconnect->onPress.bind(this, &DialogCollaborate::onDisconnect);
}

void DialogCollaborate::onTick() {
    myStatus = gSession->getStatus();
    myShareAddress = gSession->getShareAddress();
    myUpdateGuestList();
    EditorDialog::onTick();
}

void DialogCollaborate::myUpdateGuestList() {
    std::vector<uint32_t> ids;
    std::vector<std::string> names;
    if (gSession->getRole() == Session::ROLE_HOST) {
        for (const auto& peer : gSession->getPeers()) {
            ids.push_back(peer.id);
            names.push_back(peer.name);
        }
    }
    if (ids == myGuestIds && names == myGuestNames) return;

    // Keep the same guest selected if they are still there.
    uint32_t selected = UINT32_MAX;
    if (myGuestIndex >= 0 &&
        myGuestIndex < static_cast<int>(myGuestIds.size())) {
        selected = myGuestIds[myGuestIndex];
    }
    myGuestIds = ids;
    myGuestNames = names;
    myGuestIndex = 0;
    myGuestList->clearItems();
    for (size_t i = 0; i < names.size(); ++i) {
        myGuestList->addItem(names[i]);
        if (ids[i] == selected) myGuestIndex = static_cast<int>(i);
    }
}

void DialogCollaborate::onKick() {
    if (gSession->getRole() != Session::ROLE_HOST) {
        HudNote("Only the host can remove guests.");
        return;
    }
    if (myGuestIndex < 0 ||
        myGuestIndex >= static_cast<int>(myGuestIds.size())) {
        HudNote("There are no guests to remove.");
        return;
    }
    gSession->kick(myGuestIds[myGuestIndex]);
}

void DialogCollaborate::onNameChanged() { gSession->setUserName(myName); }

void DialogCollaborate::onHost() {
    gSession->setUserName(myName);
    int port = std::atoi(myPort.c_str());
    if (port <= 0 || port > 65535) port = Session::DEFAULT_PORT;
    gSession->host(port);
}

void DialogCollaborate::onJoin() {
    gSession->setUserName(myName);
    gSession->join(myJoinAddress);
}

void DialogCollaborate::onCopy() {
    std::string address = gSession->getShareAddress();
    if (!address.empty()) GuiMain::setClipboardText(address);
}

void DialogCollaborate::onDisconnect() { gSession->disconnect(); }

};  // namespace Vortex

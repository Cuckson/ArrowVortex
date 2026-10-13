#include <Dialogs/CustomSnap.h>

#include <Core/Draw.h>
#include <Core/WidgetsLayout.h>

#include <Editor/Common.h>
#include <Editor/View.h>

#include <algorithm>

namespace Vortex {

DialogCustomSnap::~DialogCustomSnap() = default;

DialogCustomSnap::DialogCustomSnap() {
    myCustomSnap = gView->getCustomSnap();

    setTitle("CUSTOM SNAPS");
    myCreateWidgets();
    myUpdateSnapList();
}

void DialogCustomSnap::myCreateWidgets() {
    myLayout.row().col(80).col(80).col(64);

    WgSpinner* scol = myLayout.add<WgSpinner>("Snapping");
    scol->value.bind(&myCustomSnap);
    scol->setRange(1.0, 192.0);
    scol->setPrecision(0, 2);
    scol->setTooltip(
        "Notes per measure; decimals are allowed (e.g. 12.5), in which case "
        "the snap continues across measure lines");

    WgButton* add = myLayout.add<WgButton>();
    add->text.set("Add");
    add->onPress.bind(this, &DialogCustomSnap::onAdd);
    add->setTooltip(
        "Add the snap to the snap list, between the regular snaps (e.g. 20th "
        "goes between 16th and 24th), and select it");

    mySnapList = myLayout.add<WgDroplist>("Snaps");
    mySnapList->value.bind(&mySnapIndex);
    mySnapList->onChange.bind(this, &DialogCustomSnap::onSelect);
    mySnapList->setTooltip("The custom snaps in the snap list");

    WgButton* remove = myLayout.add<WgButton>();
    remove->text.set("Remove");
    remove->onPress.bind(this, &DialogCustomSnap::onRemove);
    remove->setTooltip("Remove the selected custom snap from the snap list");
}

void DialogCustomSnap::onTick() {
    myUpdateSnapList();
    EditorDialog::onTick();
}

void DialogCustomSnap::myUpdateSnapList() {
    const std::vector<double>& snaps = gView->getCustomSnaps();
    if (snaps != mySnaps) {
        mySnaps = snaps;
        mySnapList->clearItems();
        for (double snap : mySnaps) mySnapList->addItem(FormatSnap(snap));
    }

    // Show the selected custom snap in the list.
    if (gView->getSnapType() == ST_CUSTOM) {
        for (size_t i = 0; i < mySnaps.size(); ++i) {
            if (mySnaps[i] == gView->getCustomSnap()) {
                mySnapIndex = static_cast<int>(i);
            }
        }
    }
    mySnapIndex = std::clamp(mySnapIndex, 0,
                             std::max(0, static_cast<int>(mySnaps.size()) - 1));
}

void DialogCustomSnap::onAdd() {
    if (myCustomSnap >= 1 && myCustomSnap <= 192) {
        gView->addCustomSnap(myCustomSnap);
    }
}

void DialogCustomSnap::onSelect() {
    if (mySnapIndex >= 0 && mySnapIndex < static_cast<int>(mySnaps.size())) {
        gView->setCustomSnap(mySnaps[mySnapIndex]);
    }
}

void DialogCustomSnap::onRemove() {
    if (mySnapIndex >= 0 && mySnapIndex < static_cast<int>(mySnaps.size())) {
        gView->removeCustomSnap(mySnaps[mySnapIndex]);
    }
}
};  // namespace Vortex

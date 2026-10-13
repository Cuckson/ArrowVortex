#include <Dialogs/CustomSnap.h>

#include <Core/Draw.h>
#include <Core/WidgetsLayout.h>

#include <Editor/View.h>

namespace Vortex {

DialogCustomSnap::~DialogCustomSnap() = default;

DialogCustomSnap::DialogCustomSnap() {
    myCustomSnap = gView->getCustomSnap();

    setTitle("CUSTOM SNAP");
    myCreateWidgets();
}

void DialogCustomSnap::myCreateWidgets() {
    myLayout.row().col(80).col(80);

    WgSpinner* scol = myLayout.add<WgSpinner>("Snapping");
    scol->value.bind(&myCustomSnap);
    scol->onChange.bind(this, &DialogCustomSnap::onChange);
    scol->setRange(1.0, 192.0);
    scol->setPrecision(0, 2);
    scol->setTooltip(
        "Notes per measure; decimals are allowed (e.g. 12.5), in which case "
        "the snap continues across measure lines");
}

void DialogCustomSnap::onChange() {
    if (myCustomSnap >= 1 && myCustomSnap <= 192) {
        gView->setCustomSnap(myCustomSnap);
    }
}
};  // namespace Vortex
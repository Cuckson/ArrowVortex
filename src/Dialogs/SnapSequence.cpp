#include <Dialogs/SnapSequence.h>

#include <Core/WidgetsLayout.h>

#include <Editor/Common.h>
#include <Editor/View.h>

namespace Vortex {

DialogSnapSequence::~DialogSnapSequence() = default;

DialogSnapSequence::DialogSnapSequence() {
    mySequence = FormatSnapSequence(gView->getCustomSnapSequence());

    setTitle("SNAP SEQUENCE");
    myCreateWidgets();
}

void DialogSnapSequence::myCreateWidgets() {
    myLayout.row().col(80).col(160);

    WgLineEdit* seq = myLayout.add<WgLineEdit>("192nd steps");
    seq->text.bind(&mySequence);
    seq->onChange.bind(this, &DialogSnapSequence::onSequenceChange);
    seq->setTooltip(
        "A repeating sequence of steps in 192nds, e.g. \"16 8 8\" moves 16 "
        "rows, then 8, then 8, and repeats. The sequence is the last entry "
        "of the snap list; leave empty to remove it");

    myLayout.row().col(244);
    WgButton* select = myLayout.add<WgButton>();
    select->text.set("Select");
    select->onPress.bind(this, &DialogSnapSequence::onSelect);
    select->setTooltip("Switch the snap to the sequence");
}

void DialogSnapSequence::onSequenceChange() {
    std::vector<int> steps = ParseSnapSequence(mySequence);
    mySequence = FormatSnapSequence(steps);
    gView->setCustomSnapSequence(steps);
}

void DialogSnapSequence::onSelect() {
    if (gView->getCustomSnapSequence().empty()) {
        HudNote("There is no snap sequence.");
    } else {
        gView->setSnapType(ST_SEQUENCE);
    }
}
};  // namespace Vortex

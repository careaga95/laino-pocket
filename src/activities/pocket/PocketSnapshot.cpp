#include "PocketSnapshot.h"

#include <cstring>

namespace pocket {

void loadEmptySnapshot(PocketSnapshot& destination) {
  destination = PocketSnapshot{};
  destination.fixture = true;
  constexpr SnapshotSectionId ids[] = {SnapshotSectionId::Agenda, SnapshotSectionId::Tasks, SnapshotSectionId::Waiting,
                                       SnapshotSectionId::Owe};
  constexpr const char* labels[] = {"Agenda", "Tasks", "Waiting On", "You Owe"};
  destination.sectionCount = MAX_SNAPSHOT_SECTIONS;
  for (size_t index = 0; index < MAX_SNAPSHOT_SECTIONS; ++index) {
    destination.sections[index].id = ids[index];
    std::strncpy(destination.sections[index].label, labels[index], sizeof(destination.sections[index].label) - 1);
  }
}

bool snapshotHasPartialSource(const PocketSnapshot& snapshot) {
  return snapshot.calendarState != SnapshotSourceState::Fresh || snapshot.tasksState != SnapshotSourceState::Fresh ||
         snapshot.commitmentsState != SnapshotSourceState::Fresh;
}

}  // namespace pocket

#pragma once

// The menu's state without ImGui or the GPU (M3): the draft the panel shows and edits, the snapshot
// it last published, the A/B switch, and when the file is due. menu.cpp drives it from the game's threads under a
// lock; menutest drives it alone.

#include "settings.h"

// Publishes a snapshot and returns it: SettingsPublish in the game, the test's stand-in in menutest.
typedef const Settings* (*MenuPublishFn)(const Settings&);

struct MenuModel
{
    Settings draft;          // what the panel shows. Its Enabled is what the user wants, as the file has it.
    Settings published;      // what the pass runs with: the draft, with Enabled off while the A/B key says so
    Settings written;        // what the file holds (as far as the menu knows)
    unsigned generation = 0; // the snapshot the draft last came from or went to
    bool open = false;
    bool abOff = false; // the A/B key: NR off for the moment. Never stored.
    bool dirty = false; // the draft differs from `written`
    double lastCommitAt = 0.0;
};

// The draft is taken from the current snapshot.
void MenuModelInit(MenuModel* m, const Settings& current);

// A snapshot the menu did not publish (the file changed and was read again): the draft follows it, and
// A/B is cleared, since the file's Enabled is what the user wrote last. True when that happened.
bool MenuModelSync(MenuModel* m, const Settings& current);

// The draft as the pass should run it.
Settings MenuModelEffective(const MenuModel& m);

// The panel changed the draft (a slider moved, a box ticked) or A/B flipped: publishes the effective snapshot and
// remembers it. Returns the snapshot. `now` is LogClock().
const Settings* MenuModelCommit(MenuModel* m, double now, MenuPublishFn publish);

// The file is written a second after the last commit, or at once when the menu is closed.
bool MenuModelWriteDue(const MenuModel& m, double now);
void MenuModelWritten(MenuModel* m);

// The rising edge of a key polled once per evaluation: true the first time it is seen down.
struct KeyEdge
{
    bool down = false;
};
bool KeyEdgeUpdate(KeyEdge* edge, bool downNow);

// The menu's state: see menu_model.h.

#include "menu_model.h"

#include "settings_write.h"

namespace
{
bool Differs(const Settings& a, const Settings& b)
{
    char diff[8];
    return SettingsDiff(a, b, diff, sizeof diff) != 0;
}
} // namespace

void MenuModelInit(MenuModel* m, const Settings& current)
{
    m->draft = current;
    m->published = current;
    m->written = current;
    m->generation = current.generation;
    m->abOff = false;
    m->dirty = false;
    m->lastCommitAt = 0.0;
}

bool MenuModelSync(MenuModel* m, const Settings& current)
{
    if (current.generation == m->generation)
        return false;
    const bool open = m->open;
    MenuModelInit(m, current);
    m->open = open;
    return true;
}

Settings MenuModelEffective(const MenuModel& m)
{
    Settings s = m.draft;
    s.enabled = m.draft.enabled && !m.abOff;
    return s;
}

const Settings* MenuModelCommit(MenuModel* m, double now, MenuPublishFn publish)
{
    const Settings* s = publish(MenuModelEffective(*m));
    m->generation = s->generation;
    m->draft.generation = s->generation;
    m->published = *s;
    m->dirty = Differs(m->written, m->draft);
    m->lastCommitAt = now;
    return s;
}

bool MenuModelWriteDue(const MenuModel& m, double now) { return m.dirty && (!m.open || now - m.lastCommitAt >= 1.0); }

void MenuModelWritten(MenuModel* m)
{
    m->written = m->draft;
    m->dirty = false;
}

bool KeyEdgeUpdate(KeyEdge* edge, bool downNow)
{
    const bool rising = downNow && !edge->down;
    edge->down = downNow;
    return rising;
}

// The menu's glue (M3): the hotkeys, the model under one lock, the file writer. See menu.h.

#include "menu.h"

#include <windows.h>

#include "freeze.h"
#include "log.h"
#include "menu_model.h"
#include "overlay_dx12.h"
#include "settings.h"
#include "settings_write.h"

namespace
{
SRWLOCK g_lock = SRWLOCK_INIT; // the model, the edges, the overlay, ImGui (menu.h: MenuLock)
MenuModel g_model;
bool g_inited = false;
KeyEdge g_menuEdge;
KeyEdge g_toggleEdge;
KeyEdge g_freezeEdge;

// The file writer: one thread, the latest snapshot wanted. A write never runs on the game's threads,
// except at shutdown, when there is no time left for another thread.
SRWLOCK g_writeLock = SRWLOCK_INIT;
HANDLE g_writeEvent = nullptr;
HANDLE g_writeThread = nullptr;
Settings g_writePending;
bool g_writeWanted = false;

void WriteNow(const Settings& s)
{
    unsigned long error = 0;
    if (SettingsWriteIni(SettingsPath(), s, &error))
    {
        SettingsNoteWritten();
        Log("menu: wrote %ls", SettingsPath());
    }
    else
        Log("menu: could not write %ls (error %lu)", SettingsPath(), error);
}

DWORD WINAPI Writer(void*)
{
    for (;;)
    {
        WaitForSingleObject(g_writeEvent, INFINITE);
        for (;;)
        {
            AcquireSRWLockExclusive(&g_writeLock);
            const bool wanted = g_writeWanted;
            const Settings s = g_writePending;
            g_writeWanted = false;
            ReleaseSRWLockExclusive(&g_writeLock);
            if (!wanted)
                break;
            WriteNow(s);
        }
    }
}

void RequestWrite(const Settings& s)
{
    AcquireSRWLockExclusive(&g_writeLock);
    g_writePending = s;
    g_writeWanted = true;
    if (g_writeEvent == nullptr)
        g_writeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_writeThread == nullptr && g_writeEvent != nullptr)
        g_writeThread = CreateThread(nullptr, 0, &Writer, nullptr, 0, nullptr);
    const bool haveThread = g_writeThread != nullptr;
    if (!haveThread)
        g_writeWanted = false;
    ReleaseSRWLockExclusive(&g_writeLock);
    if (haveThread)
        SetEvent(g_writeEvent);
    else
        WriteNow(s); // no thread to be had: here, then
}

// Under g_lock. Publishes the draft and logs what changed.
void CommitLocked(const char* why)
{
    const Settings before = g_model.published;
    const Settings* after = MenuModelCommit(&g_model, LogClock(), &SettingsPublish);
    char diff[512];
    SettingsDiff(before, *after, diff, sizeof diff);
    Log("menu%s%s: %s", why != nullptr ? " " : "", why != nullptr ? why : "", diff[0] != '\0' ? diff : "no change");
}

// A slider being dragged: each step is published as it happens and the drag is logged once, when it ends.
Settings g_dragFrom;     // what was published before the drag's first step
bool g_dragging = false; // a step was published and the drag has not been logged yet

// Under g_lock. The drag ended (the mouse let go, or the menu closed under it): one line for all its steps.
void DragEndedLocked()
{
    if (!g_dragging)
        return;
    g_dragging = false;
    char diff[512];
    SettingsDiff(g_dragFrom, g_model.published, diff, sizeof diff);
    Log("menu slider: %s", diff[0] != '\0' ? diff : "back where it started");
}

// Under g_lock. `list` is the game's command list when there is one (the overlay learns the device from it).
void SetOpenLocked(bool open, ID3D12GraphicsCommandList* list)
{
    if (g_model.open == open)
        return;
    if (open && !OverlayOpen(list))
        return; // said why in the log; the hotkeys still work without a menu
    g_model.open = open;
    if (!open)
        DragEndedLocked();
    Log("menu: %s", open ? "opened" : "closed");
    if (!open)
    {
        OverlayClose();
        FreezeRequest(false); // a frozen frame never outlives the menu: it would look like a hang
        if (g_model.draft.card)
        {
            g_model.draft.card = false;
            CommitLocked("card off with the menu");
        }
        if (MenuModelWriteDue(g_model, LogClock()))
        {
            RequestWrite(g_model.draft);
            MenuModelWritten(&g_model);
        }
    }
}

// The hotkeys count only while a window of this process is in front.
bool InFront()
{
    const HWND window = GetForegroundWindow();
    if (window == nullptr)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

bool KeyDown(unsigned vk) { return vk != 0 && vk < 256 && (GetAsyncKeyState(int(vk)) & 0x8000) != 0; }
} // namespace

void MenuOnEvaluate(ID3D12GraphicsCommandList* list)
{
    const Settings* current = SettingsCurrent();
    if (current->generation == 0)
        return; // the file has not been read yet (the NR pass reads it on this same evaluation)
    const double now = LogClock();
    AcquireSRWLockExclusive(&g_lock);
    if (!g_inited)
    {
        MenuModelInit(&g_model, *current);
        g_inited = true;
    }
    else if (MenuModelSync(&g_model, *current))
        Log("menu: the file was read again, the menu follows it");

    const bool front = InFront();
    bool menuEdge = KeyEdgeUpdate(&g_menuEdge, front && KeyDown(g_model.draft.menuKey));
    bool toggleEdge = KeyEdgeUpdate(&g_toggleEdge, front && KeyDown(g_model.draft.toggleKey));
    bool freezeEdge = KeyEdgeUpdate(&g_freezeEdge, front && KeyDown(g_model.draft.freezeKey));
    if (MenuPanelListening())
        menuEdge = toggleEdge = freezeEdge = false; // the key goes to the binding
    if (menuEdge)
        SetOpenLocked(!g_model.open, list);
    if (toggleEdge)
    {
        g_model.abOff = !g_model.abOff;
        CommitLocked(g_model.abOff ? "A/B off" : "A/B on");
    }
    if (freezeEdge)
    {
        const bool freeze = !FreezeWanted();
        FreezeRequest(freeze);
        Log("menu: freeze %s", freeze ? "asked" : "released");
        if (freeze)
            SetOpenLocked(true, list);
    }
    // The card lives on the frozen frame: when the freeze goes, by the key, the panel or a size change
    // in the pass, the card goes with it.
    if (!FreezeWanted() && g_model.draft.card)
    {
        g_model.draft.card = false;
        CommitLocked("card off with the freeze");
    }
    if (MenuModelWriteDue(g_model, now))
    {
        RequestWrite(g_model.draft);
        MenuModelWritten(&g_model);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

MenuModel& MenuModelLocked() { return g_model; }

void MenuCommitLocked(const char* why) { CommitLocked(why); }

void MenuDragStepLocked()
{
    if (!g_dragging)
    {
        g_dragFrom = g_model.published;
        g_dragging = true;
    }
    MenuModelCommit(&g_model, LogClock(), &SettingsPublish);
}

void MenuDragEndedLocked() { DragEndedLocked(); }

void MenuCloseLocked() { SetOpenLocked(false, nullptr); }

void MenuLock() { AcquireSRWLockExclusive(&g_lock); }

void MenuUnlock() { ReleaseSRWLockExclusive(&g_lock); }

bool MenuTryLock(unsigned milliseconds)
{
    const ULONGLONG until = GetTickCount64() + milliseconds;
    while (!TryAcquireSRWLockExclusive(&g_lock))
    {
        if (GetTickCount64() >= until)
            return false;
        Sleep(0);
    }
    return true;
}

bool MenuKeyPressedInWindow(unsigned vk)
{
    AcquireSRWLockExclusive(&g_lock);
    const bool close = g_inited && g_model.open && (vk == VK_ESCAPE || (vk != 0 && vk == g_model.draft.menuKey));
    if (close)
    {
        g_menuEdge.down = true; // the poll on the render thread must not see this press as a new one
        SetOpenLocked(false, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return close;
}

void MenuKeys(unsigned* menuKey)
{
    AcquireSRWLockShared(&g_lock);
    *menuKey = g_model.draft.menuKey;
    ReleaseSRWLockShared(&g_lock);
}

void MenuSetOpen(bool open)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_inited)
        SetOpenLocked(open, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
}

bool MenuIsOpen()
{
    AcquireSRWLockShared(&g_lock);
    const bool open = g_model.open;
    ReleaseSRWLockShared(&g_lock);
    return open;
}

void MenuBeforeCoreShutdown()
{
    AcquireSRWLockExclusive(&g_lock);
    OverlayRelease();
    if (g_inited)
    {
        g_model.open = false;
        if (g_model.dirty)
        {
            WriteNow(g_model.draft);
            MenuModelWritten(&g_model);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

#pragma once

// Interception of the game's DLSS calls on the driver's NGX core, _nvngx.dll: CreateFeature, EvaluateFeature and
// ReleaseFeature (D3D12), plus Shutdown and Shutdown1, by rewriting five entries of the core's export table
// (ngx_hook.cpp says how). The calls are counted and logged, and every successful SR/RR evaluation is handed to the
// Neural Rendering pass (nr_dx12.h) with the game's own command list and parameters; nothing about the calls
// themselves changes.

#include <windows.h>

// From DllMain, once the exports are forwarded: watch for the core to load and patch it the moment it does, or now if
// it is already loaded. Pins this DLL in the process, since the core's export table then leads into it. False if the
// load notification could not be registered; the game then runs without interception.
bool NgxHookStart();

// From DllMain when the process ends normally: the totals per feature.
void NgxHookStop();

// The core whose export table leads here, for its own exports (AllocateParameters, GetCapabilityParameters); nullptr
// while none is patched.
HMODULE NgxCoreModule();

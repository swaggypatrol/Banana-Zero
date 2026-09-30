#pragma once

// What fakemodel (the stand-in NR model) and ngxtest share.

// NVSDK_NGX_Handle's layout.
struct FakeModelHandle
{
    unsigned int Id;
};

// The last call the fake model received, refused or not.
struct FakeModelCall
{
    const char* function;     // "Init_Ext", "PopulateParameters_Impl", "CreateFeature", "EvaluateFeature",
                              // "ReleaseFeature", "Shutdown1"; nullptr before the first call
    unsigned long long appId; // Init_Ext
    int version;              // Init_Ext
    const void* args[4];      // Init_Ext: data path, device, feature info. PopulateParameters_Impl: params.
                              // CreateFeature: list, feature (widened), params, handle pointer. EvaluateFeature: list,
                              // handle, params, callback. ReleaseFeature: handle. Shutdown1: device.
    const void* caller;       // the return address: the code that called the fake model
    bool accepted;            // the caller check passed (the caller's module path contains "nvngx.dll")
    unsigned calls;           // calls so far
};

constexpr int kFakeModelSuccess = 1;
constexpr int kFakeModelPlatformError = int(0xBAD00002);    // what the real model answers a wrong caller
constexpr int kFakeModelInvalidParameter = int(0xBAD00005); // CreateFeature of anything but feature 18
constexpr unsigned kFakeModelFirstId = 500;                 // the first handle's Id; each Create adds one

using FakeModelLastCall = void (*)(FakeModelCall*);

#pragma once

// What fakecore (the stand-in NGX core) and ngxtest share.

// NVSDK_NGX_Handle's layout.
struct FakeHandle
{
    unsigned int Id;
};

// The last call the fake core received.
struct FakeCall
{
    const char* function; // "CreateFeature", ...; nullptr before the first call
    const void* args[4];  // as received; CreateFeature's feature number is widened into args[1]
    const void* caller;   // the return address: the code that called the fake core
    unsigned calls;       // calls so far
};

constexpr int kFakeSuccess = 1;
constexpr int kFakeInvalidParameter = int(0xBAD00005); // NVSDK_NGX_Result_FAIL_InvalidParameter
constexpr int kFakeRefusedFeature = 99;                // CreateFeature refuses this one, with kFakeInvalidParameter
constexpr unsigned kFakeFirstId = 100;                 // the first handle's Id; each Create adds one

using FakeLastCall = void (*)(FakeCall*);
using FakeSetEvaluateResult = void (*)(int);

#pragma once

// What one SR/RR evaluation hands the Neural Rendering pass, read from the game's parameters (nr_dx12.cpp,
// ReadFrame). Its own header since M3: the frozen frame (freeze.h) swaps the game's textures for its copies here.

#include <d3d12.h>

struct Frame
{
    ID3D12Resource* output;
    ID3D12Resource* depth;
    ID3D12Resource* motion;
    ID3D12Resource* exposure; // the game's exposure texture, or null
    D3D12_RESOURCE_DESC outputDesc;
    D3D12_RESOURCE_DESC depthDesc;
    D3D12_RESOURCE_DESC motionDesc;
    D3D12_RESOURCE_DESC exposureDesc;
    DXGI_FORMAT exposureView;         // what we read the exposure texture with; UNKNOWN: it is not used
    unsigned width, height;           // the frame: the DLSS output size
    unsigned baseX, baseY;            // where it lies inside the Output texture
    unsigned guideWidth, guideHeight; // the depth subrect: what the game rendered
    unsigned depthBaseX, depthBaseY;
    unsigned motionWidth, motionHeight;
    unsigned motionBaseX, motionBaseY;
    float mvScaleX, mvScaleY;
    unsigned reset;
    bool depthInverted;
    bool lowResMotion;
    bool hdr;
    bool renderSubrectGiven; // the game passed Render_Subrect_Dimensions (else the creation size stands in)
    bool havePreExposure;
    float preExposure;
};

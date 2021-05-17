#pragma once

#include <cstdio>
#include <cassert>
#include <memory>
#include <d3d11.h>
#include "../Spout/SpoutDirectX/SpoutDX/SpoutDX.h"
#include "../Spout/SpoutDirectX/SpoutDX/SpoutDX12/SpoutDX12.h"

// Debug logging macro
#if defined(_DEBUG)
#define DEBUG_LOG(fmt, ...) std::printf("KlakSpout: "#fmt"\n", __VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...)
#endif

namespace klakspout
{
    // Singleton class used for storing global variables
    class Globals final
    {
    public:

        enum class Renderer { DX11, DX12 } renderer_;

        std::unique_ptr<spoutDX> spoutDX_;
        std::unique_ptr<spoutDX12> spoutDX12_;
        bool isReady = false;
        bool isReady2 = false;

        static Globals& get()
        {
            static Globals instance;
            return instance;
        }
    };
}

#include "KlakSpoutSharedObject.h"
#include "../Unity/IUnityGraphics.h"
#include "../Unity/IUnityGraphicsD3D11.h"
#include <mutex>

#include <d3d12.h>
#include "../Unity/IUnityGraphicsD3D12.h"

namespace
{
    // Low-level native plugin interface
    IUnityInterfaces* unity_;

    // Temporary storage for shared Spout object list
    std::set<std::string> shared_object_names_;

    // Local mutex object used to prevent race conditions between the main
    // thread and the render thread. This should be locked at the following
    // points:
    // - OnRenderEvent (this is the only point called from the render thread)
    // - Plugin function that calls SharedObject or Spout API functions.
    std::mutex lock_;

    // Unity device event callback
    void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType event_type)
    {
        assert(unity_);

        // Do nothing if it's not the D3D11/D3D12 renderer.
        UnityGfxRenderer renderer = unity_->Get<IUnityGraphics>()->GetRenderer();
        if (renderer != kUnityGfxRendererD3D11 && renderer != kUnityGfxRendererD3D12) return;

        DEBUG_LOG("OnGraphicsDeviceEvent (%d)", event_type);

        auto& g = klakspout::Globals::get();

        if (event_type == kUnityGfxDeviceEventInitialize)
        {
            if (renderer == kUnityGfxRendererD3D11)
            {
                // Retrieve the D3D11 interface.
                ID3D11Device* device = unity_->Get<IUnityGraphicsD3D11>()->GetDevice();
                if (!device)
                {
                    DEBUG_LOG("Couldn't retrieve D3D11 interface");
                    return;
                }

                // Enable logging to catch Spout warnings and errors
                OpenSpoutConsole(); // Console only for debugging
                EnableSpoutLog(); // Log to console
                SetSpoutLogLevel(SPOUT_LOG_VERBOSE); // show only warnings and errors

                g.spoutDX_ = std::make_unique<spoutDX>();
                if (!g.spoutDX_->OpenDirectX11(device))
                {
                    DEBUG_LOG("OpenDirectX11 failed");
                    return;
                }

                g.renderer_ = klakspout::Globals::Renderer::DX11;
                g.isReady = true;
            }
            else if (renderer == kUnityGfxRendererD3D12)
            {
                // Enable logging to catch Spout warnings and errors
                OpenSpoutConsole(); // Console only for debugging
                EnableSpoutLog(); // Log to console
                SetSpoutLogLevel(SPOUT_LOG_VERBOSE); // show only warnings and errors

                g.spoutDX12_ = std::make_unique<spoutDX12>();
                g.renderer_ = klakspout::Globals::Renderer::DX12;
                g.isReady = true;
            }
        }
        else if (event_type == kUnityGfxDeviceEventShutdown)
        {
            if (renderer == kUnityGfxRendererD3D11)
            {
                g.spoutDX_.reset();
            }
            else
            {
                g.spoutDX12_->CloseDirectX12();
                g.spoutDX12_.reset();
            }
        }
    }

    // Unity render event callbacks
    void UNITY_INTERFACE_API OnRenderEvent(int event_id, void* data)
    {
        std::lock_guard<std::mutex> guard(lock_);

        auto& g = klakspout::Globals::get();

        if (event_id == 0 && g.renderer_ == klakspout::Globals::Renderer::DX12 && !g.isReady2)
        {
            // Retrieve the D3D12 interface.
            IUnityGraphicsD3D12v4* uInterface = unity_->Get<IUnityGraphicsD3D12v4>();
            ID3D12Device* device = uInterface->GetDevice();
            ID3D12CommandQueue* commandQueue = uInterface->GetCommandQueue();
            if (!device || !commandQueue)
            {
                DEBUG_LOG("Couldn't retrieve D3D12 interface");
                return;
            }

            if (!g.spoutDX12_->OpenDirectX12(device, reinterpret_cast<IUnknown**>(commandQueue)))
            {
                DEBUG_LOG("OpenDirectX12 failed");
                return;
            }

            g.isReady2 = true;
        }

        // Do nothing if the D3D11 interface is not available. This only
        // happens on Editor. It may leak some resoruces but we can't do
        // anything about them.
        if (!g.isReady) return;

        auto* pobj = reinterpret_cast<klakspout::SharedObject*>(data);

        if (event_id == 0) // Update event
        {
            if (!pobj->isActive()) pobj->activate();
        }
        else if (event_id == 1) // Dispose event
        {
            delete pobj;
        }
    }
}

//
// Low-level native plugin implementation
//

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces)
{
    unity_ = interfaces;

    // Replace stdout with a new console for debugging.
    #if defined(_DEBUG)
    FILE * pConsole;
    AllocConsole();
    freopen_s(&pConsole, "CONOUT$", "wb", stdout);
    #endif

    // Register the custom callback, then manually invoke the initialization event once.
    unity_->Get<IUnityGraphics>()->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    // Unregister the custom callback.
    unity_->Get<IUnityGraphics>()->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);

    unity_ = nullptr;
}

extern "C" UnityRenderingEventAndData UNITY_INTERFACE_EXPORT GetRenderEventFunc()
{
    return OnRenderEvent;
}

//
// Native plugin implementation
//

extern "C" void UNITY_INTERFACE_EXPORT * CreateSender(const char* name, int width, int height)
{
    if (!klakspout::Globals::get().isReady) return nullptr;
    return new klakspout::SharedObject(klakspout::SharedObject::Type::sender, name != nullptr ? name : "", width, height);
}

extern "C" void UNITY_INTERFACE_EXPORT * CreateReceiver(const char* name)
{
    if (!klakspout::Globals::get().isReady) return nullptr;
    return new klakspout::SharedObject(klakspout::SharedObject::Type::receiver, name != nullptr ? name : "");
}

extern "C" void UNITY_INTERFACE_EXPORT * GetTexturePointer(void* ptr)
{
    // return reinterpret_cast<const klakspout::SharedObject*>(ptr)->d3d11_resource_view_;
    return nullptr;
}

extern "C" int UNITY_INTERFACE_EXPORT SendTexture(void* ptr, void* tex)
{
    if (!klakspout::Globals::get().isReady) return false;
    return reinterpret_cast<klakspout::SharedObject*>(ptr)->SendTexture(tex);
}

extern "C" int UNITY_INTERFACE_EXPORT GetTextureWidth(void* ptr)
{
    return reinterpret_cast<const klakspout::SharedObject*>(ptr)->width_;
}

extern "C" int UNITY_INTERFACE_EXPORT GetTextureHeight(void* ptr)
{
    return reinterpret_cast<const klakspout::SharedObject*>(ptr)->height_;
}

extern "C" int UNITY_INTERFACE_EXPORT CheckValid(void* ptr)
{
    std::lock_guard<std::mutex> guard(lock_);
    return reinterpret_cast<const klakspout::SharedObject*>(ptr)->isValid();
}

extern "C" int UNITY_INTERFACE_EXPORT ScanSharedObjects()
{
    auto& g = klakspout::Globals::get();
    if (!g.isReady) return 0;
    std::lock_guard<std::mutex> guard(lock_);
    shared_object_names_.clear();

    if (g.renderer_ == klakspout::Globals::Renderer::DX11) {
        if (g.spoutDX_->sendernames.GetSenderNames(&shared_object_names_)) {
            return static_cast<int>(shared_object_names_.size());
        }
    }
    else if (g.renderer_ == klakspout::Globals::Renderer::DX12) {
        if (g.spoutDX12_->sendernames.GetSenderNames(&shared_object_names_)) {
            return static_cast<int>(shared_object_names_.size());
        }
    }

	return 0;
}

extern "C" const void UNITY_INTERFACE_EXPORT * GetSharedObjectName(int index)
{
    // static std::string temp;
    // char strBuffer[256];
    // if (g.spoutDX_->GetSender(index, strBuffer, 256))
    // {
    //     temp = strBuffer;
    //     return temp.c_str();
    // }
    // else
    // {
    //     return nullptr;
    // }

    auto count = 0;
    for (auto& name : shared_object_names_)
    {
        if (count++ == index)
        {
            // Return the name via a static string object.
            static std::string temp;
            temp = name;
            return temp.c_str();
        }
    }
    return nullptr;
}

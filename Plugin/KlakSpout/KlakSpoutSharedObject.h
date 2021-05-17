#pragma once

#include "KlakSpoutGlobals.h"

namespace klakspout
{
    // Shared Spout object handler class
    // Not thread safe. The owner should care about it.
    class SharedObject final
    {
    public:

        // Object type
        enum class Type { sender, receiver } const type_;

        // Object attributes
        const std::string name_;
        int width_, height_;
        bool active;
        ID3D11Resource* wrappedDX12Texture;

        // Constructor
        SharedObject(Type type, const std::string& name, int width = -1, int height = -1)
            : type_(type), name_(name), width_(width), height_(height), active(false), wrappedDX12Texture(nullptr)
        {
            if (type_ == Type::sender)
                DEBUG_LOG("Sender created (%s)", name_.c_str());
            else
                DEBUG_LOG("Receiver created (%s)", name_.c_str());
        }

        // Destructor
        ~SharedObject()
        {
            releaseInternals();

            if (type_ == Type::sender)
                DEBUG_LOG("Sender disposed (%s)", name_.c_str());
            else
                DEBUG_LOG("Receiver disposed (%s)", name_.c_str());
        }

        // Prohibit use of default constructor and copy operators
        SharedObject() = delete;
        SharedObject(SharedObject&) = delete;
        SharedObject& operator = (const SharedObject&) = delete;

        // Check if it's active.
        bool isActive() const
        {
            return active;
        }

        // Validate the internal resources.
        bool isValid() const
        {
            // Nothing to validate for senders.
            if (type_ == Type::sender) return true;

            // Non-active objects have nothing to validate.
            if (!isActive()) return true;

            auto& g = Globals::get();

            // This must be an active receiver, so check if the connection to
            // the sender is still valid.
            unsigned int width, height;
            HANDLE handle;
            DWORD format;
            bool found = false;

            if (g.renderer_ == Globals::Renderer::DX11)
                found = g.spoutDX_->sendernames.CheckSender(name_.c_str(), width, height, handle, format);
            else if (g.renderer_ == Globals::Renderer::DX12)
                found = g.spoutDX12_->sendernames.CheckSender(name_.c_str(), width, height, handle, format);

            return found && width_ == width && height_ == height;
        }

        // Try activating the object. Returns false when failed.
        bool activate()
        {
            assert(active == false);
            active = (type_ == Type::sender) ? setupSender() : setupReceiver();
            return active;
        }

        // Deactivate the object and release its internal resources.
        void deactivate()
        {
            releaseInternals();
        }

        bool SendTexture(void* texture)
        {
            if (!active)
            {
                return false;
            }

            auto& g = Globals::get();

            if (g.renderer_ == Globals::Renderer::DX11)
            {
                ID3D11Texture2D* dx11Texture = static_cast<ID3D11Texture2D*>(texture);
                return g.spoutDX_->SendTexture(dx11Texture);
            }
            else if (g.renderer_ == Globals::Renderer::DX12)
            {
                ID3D12Resource* dx12Resource = static_cast<ID3D12Resource*>(texture);
                if (!wrappedDX12Texture)
                    g.spoutDX12_->WrapDX12Resource(dx12Resource, &wrappedDX12Texture);
                if (wrappedDX12Texture)
                    return g.spoutDX12_->SendDX11Resource(wrappedDX12Texture);
            }

            return false;
        }

    private:

        // Release internal objects.
        void releaseInternals()
        {
            auto& g = Globals::get();

            // Senders should unregister their own name on destruction.
            if (type_ == Type::sender && active)
            {
                DEBUG_LOG("Sender being disposed (%s)", name_.c_str());

                if (g.renderer_ == Globals::Renderer::DX11)
                    g.spoutDX_->ReleaseSender();
                else if (g.renderer_ == Globals::Renderer::DX12)
                    g.spoutDX12_->ReleaseSender();
            }

            if (wrappedDX12Texture)
            {
                wrappedDX12Texture->Release();
                wrappedDX12Texture = nullptr;
            }
        }

        // Set up as a sender.
        bool setupSender()
        {
            auto& g = Globals::get();

            // Avoid name duplication.
            {
                unsigned int width, height; HANDLE handle; DWORD format; // unused
                bool found = false;

                if (g.renderer_ == Globals::Renderer::DX11)
                    found = g.spoutDX_->sendernames.CheckSender(name_.c_str(), width, height, handle, format);
                else if (g.renderer_ == Globals::Renderer::DX12)
                    found = g.spoutDX12_->sendernames.CheckSender(name_.c_str(), width, height, handle, format);

                if (found)
                    return false;
            }

            // Currently we only support RGBA32.
            //const auto format = DXGI_FORMAT_R8G8B8A8_UNORM;
            //g.spoutDX_->SetSenderFormat(format);

            if (g.renderer_ == Globals::Renderer::DX11)
                g.spoutDX_->SetSenderName(name_.c_str());
            else if (g.renderer_ == Globals::Renderer::DX12)
                g.spoutDX12_->SetSenderName(name_.c_str());

            DEBUG_LOG("Sender activated (%s)", name_.c_str());
            return true;
        }

        // Set up as a receiver.
        bool setupReceiver()
        {
            // auto& g = Globals::get();

            // // Retrieve the sender information with the given name.
            // HANDLE handle;
            // DWORD format;
            // unsigned int w, h;
            // auto res_spout = g.sender_names_->CheckSender(name_.c_str(), w, h, handle, format);

            // if (!res_spout)
            // {
            //     // This happens really frequently. Avoid spamming the console.
            //     // DEBUG_LOG("CheckSender failed (%s)", name_.c_str());
            //     return false;
            // }

            // width_ = w;
            // height_ = h;

            // // Start sharing the texture.
            // void** ptr = reinterpret_cast<void**>(&d3d11_resource_);
            // auto res_d3d = g.d3d11_->OpenSharedResource(handle, __uuidof(ID3D11Resource), ptr);

            // if (FAILED(res_d3d))
            // {
            //     DEBUG_LOG("OpenSharedResource failed (%s:%x)", name_.c_str(), res_d3d);
            //     return false;
            // }

            // // Create a resource view for the shared texture.
            // res_d3d = g.d3d11_->CreateShaderResourceView(d3d11_resource_, nullptr, &d3d11_resource_view_);

            // if (FAILED(res_d3d))
            // {
            //     d3d11_resource_->Release();
            //     d3d11_resource_ = nullptr;
            //     DEBUG_LOG("CreateShaderResourceView failed (%s:%x)", name_.c_str(), res_d3d);
            //     return false;
            // }

            // DEBUG_LOG("Receiver activated (%s)", name_.c_str());
            return true;
        }
    };
}
// RdpHost — wraps the MsTscAx.MsTscAx.9 ActiveX control + its IDispatch event sink,
// translating inbound IPC commands into ActiveX calls and outbound ActiveX events
// into IPC notifications back to hyprv.exe.
//
// pimpl pattern keeps mstscax / cppwinrt / ATL types out of the header so the
// main translation unit doesn't pay for that #import.

#pragma once

#include <windows.h>
#include <string>

#include "RdpIpc.h"

namespace hyprv::rdphost
{
    class IpcClient;

    class RdpHost
    {
    public:
        explicit RdpHost(IpcClient& ipc);
        ~RdpHost();

        RdpHost(const RdpHost&) = delete;
        RdpHost& operator=(const RdpHost&) = delete;

        // Create the AtlAxWin child + load MsTscAx, advise event sink. parent is the
        // frameless top-level window created by main.cpp.
        bool Activate(HWND parent, int width, int height);

        // The AtlAxWin child window — main.cpp's WndProc forwards focus into this.
        HWND ax() const;

        // Current negotiated session resolution. Initially zero; populated from
        // ConnectLocalVm options at activation, then updated live from mstscax's
        // RemoteDesktopSizeChange events. main.cpp uses this to compute the
        // letterbox clip region in WM_SIZE so the surrounding popup area becomes
        // transparent (parent's backdrop shows through) instead of mstscax gray.
        void GetSessionSize(int& width, int& height) const;

        // IPC handlers — invoked on the UI thread after main.cpp parses the message.
        // The string args are the UTF-8 server/domain fields decoded from RdpOptions.
        void OnConnect(const hyprv::ipc::Connect& cmd,
                       const std::string& server, const std::string& domain,
                       const std::string& username);
        void OnConnectLocalVm(const hyprv::ipc::ConnectLocalVm& cmd,
                              const std::string& server, const std::string& domain);
        void OnDisconnect();
        void OnUpdateSessionDisplaySettings(const hyprv::ipc::UpdateSessionDisplaySettings& cmd);
        void OnSetEnhanced(bool enhanced);
        void OnSendCtrlAltDel();

    private:
        struct Impl;
        Impl* m_impl;
    };
}

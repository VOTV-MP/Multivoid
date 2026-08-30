// bootstrap/refuse_dialog.cpp -- see bootstrap/refuse_dialog.h.

#include "bootstrap/refuse_dialog.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

#include <string>
#include <utility>

namespace bootstrap {
namespace {

struct DialogRequest {
    std::wstring title;
    std::wstring body;
};

DWORD WINAPI RefuseDialogThread(LPVOID raw) {
    DialogRequest* req = static_cast<DialogRequest*>(raw);
    // The up/dismissed lines are the headless drills' evidence that the dialog
    // REALLY showed (a window-probe from outside proved unreliable; the log is
    // authoritative and kill-safe because both lines flush).
    UE_LOGI("refuse dialog up (tid=%lu)", ::GetCurrentThreadId());
    ue_wrap::log::Flush();
    ::MessageBoxW(nullptr, req->body.c_str(), req->title.c_str(),
                  MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    UE_LOGI("refuse dialog dismissed");
    ue_wrap::log::Flush();
    delete req;
    return 0;
}

}  // namespace

void ShowRefuseDialog(const std::wstring& title, const std::wstring& body) {
    auto* req = new DialogRequest{title, body};
    if (HANDLE t = ::CreateThread(nullptr, 0, RefuseDialogThread, req, 0, nullptr)) {
        ::CloseHandle(t);
    } else {
        // No thread, no modal -- but the reason must not vanish with it. This is
        // the only path where the user gets nothing on screen, so the log is the
        // whole record and it flushes.
        UE_LOGE("refuse dialog could NOT be shown (CreateThread failed, gle=%lu) -- "
                "reason follows: %ls", ::GetLastError(), req->body.c_str());
        ue_wrap::log::Flush();
        delete req;
    }
}

}  // namespace bootstrap

// Wisp: WebKit's auxiliary processes as local processes of the app. WPE 2.54's WPEProcessManager
// (made for Android, where an embedder must start them itself; WebKit's ProcessLauncherGLib takes this
// path on the PS4 too: patches/webkit) hands us each process's type, WebKit identifier and IPC socket.
#include "process_manager.h"

#include "../common/wisp.h"

#include <cerrno>
#include <cstring>
#include <glib-object.h>
#include <unistd.h>
#include <wpe/wpe-platform.h>

G_BEGIN_DECLS
#define WISP_TYPE_PROCESS_MANAGER (wisp_process_manager_get_type())
G_DECLARE_FINAL_TYPE(WispProcessManager, wisp_process_manager, WISP, PROCESS_MANAGER, WPEProcessManager)
G_END_DECLS

struct _WispProcessManager {
    WPEProcessManager parent;
};
G_DEFINE_FINAL_TYPE(WispProcessManager, wisp_process_manager, WPE_TYPE_PROCESS_MANAGER)

static guint64 manager_launch(WPEProcessManager *, WPEProcessLaunchOptions *options, GError **error)
{
    const WPEProcessType type = wpe_process_launch_options_get_process_type(options);
    const guint64 id = wpe_process_launch_options_get_process_id(options);
    const int fd = wpe_process_launch_options_get_ipc_socket_fd(options);
    if (type == WPE_PROCESS_TYPE_GPU) {
        g_set_error_literal(error, WPE_PROCESS_MANAGER_ERROR, WPE_PROCESS_MANAGER_ERROR_LAUNCH_FAILED,
            "no GPU process on the PS4 (the web process draws)");
        return 0;
    }
    const WispChild child = type == WPE_PROCESS_TYPE_WEB ? WispChild::Web : WispChild::Network;
    wisp_log("launching the %s process (WebKit id %llu, IPC socket %d)", child == WispChild::Web ? "web" : "network",
        static_cast<unsigned long long>(id), fd);
    char reason[256] = "";
    const int local = wisp_launch_local(child, id, fd, reason, sizeof reason);
    // WebKit closes its copy of the child's end itself (the Android path leaks it to the child)
    if (local < 0) {
        wisp_log("launch failed: %s", reason);
        g_set_error(error, WPE_PROCESS_MANAGER_ERROR, WPE_PROCESS_MANAGER_ERROR_LAUNCH_FAILED, "%s", reason);
        return 0;
    }
    return static_cast<guint64>(local) + 1;  // 0 means failure to WebKit
}

static void manager_terminate(WPEProcessManager *, guint64 process)
{
    if (process)
        wisp_kill_local(static_cast<int>(process - 1));
}

static void wisp_process_manager_class_init(WispProcessManagerClass *klass)
{
    auto *manager_class = WPE_PROCESS_MANAGER_CLASS(klass);
    manager_class->launch = manager_launch;
    manager_class->terminate = manager_terminate;
}
static void wisp_process_manager_init(WispProcessManager *) { }

namespace wisp {

void process_manager_install()
{
    static WPEProcessManager *manager = nullptr;
    if (manager)
        return;
    manager = WPE_PROCESS_MANAGER(g_object_new(WISP_TYPE_PROCESS_MANAGER, nullptr));
    wpe_process_manager_set_default(manager);
    wisp_log("process manager: WebKit's processes start as PS4 local processes (%s, %s)",
        wisp_rendering() != WispRendering::Cpu ? WISP_WEB_GPU_BIN : WISP_WEB_BIN, WISP_NET_BIN);
}

} // namespace wisp

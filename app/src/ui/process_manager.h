// Wisp: WebKit's web and network processes started by the app (ui/process_manager.cpp).
#pragma once

namespace wisp {

// Before the first web view: WebKit asks this manager to start its processes
void process_manager_install();

} // namespace wisp

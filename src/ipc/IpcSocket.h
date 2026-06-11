#pragma once

#include <QString>

// Shared between the app's IpcServer and the muzaitenctl client so both sides
// derive the same socket path from the same state-resolution rules (AppPaths).
namespace IpcSocket {

// Local socket path for this state root: a muzaiten instance running against a
// different state root (dev-state, agent-state, ...) gets a different socket,
// so a ctl invocation only ever talks to the instance sharing its environment.
QString serverPath();

} // namespace IpcSocket

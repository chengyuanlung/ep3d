#pragma once

#include <string>

// The client half of the script socket (M17.28).
//
// Raw sockets rather than Qt: `ep3d` is deliberately Qt-free so it runs in CI,
// in a container and over ssh, and pulling Qt in for a client that sends lines
// and reads lines would undo the one property that makes it useful.
//
// Loopback only, matching the server -- there is no host parameter, because
// there is nothing else to connect to.

namespace ep3d {

// Sends `script` line by line to 127.0.0.1:`port`, printing each reply.
//
// Returns 0 when every line was answered OK, 1 when the server answered ERR
// (the remaining lines are NOT sent -- a script that carried on past a refused
// command would build something its author did not write), and 2 when the
// connection itself failed.
int SendScript(unsigned short port, const std::string& script, bool quiet);

} // namespace ep3d

#pragma once

#include "Cli/SketchScript.h"

#include <QObject>
#include <QtGlobal>

#include <functional>
#include <map>
#include <memory>

class QTcpServer;
class QTcpSocket;

namespace paramcad {

class PartDocument;

// A socket that drives the RUNNING viewer (M17.28).
//
// The script interpreter already exists and already drives the same path the
// mouse does. What this adds is a way to reach it while the window is open, so
// a command typed elsewhere shows up as geometry a person can see -- which is
// the difference between generating a file and using the program.
//
// ------------------------------------------------------------------ safety
//
// LOOPBACK ONLY, and not configurable.
//
// This executes commands that create, modify and SAVE files. Bound to a
// reachable address it would be a remote command service with no
// authentication, which is not a thing to ship as a convenience. QHostAddress
//::LocalHost is passed at the one call site and there is no flag to widen it;
// anyone who genuinely wants it across a network can forward the port
// deliberately, which is a decision made by someone who knows they are making
// it.
//
// The port is still a choice a user can get wrong, so the server reports the
// port it actually got and refuses quietly rather than half-starting.
//
// ------------------------------------------------------------------ protocol
//
// Newline-delimited. One line in, one exchange out:
//
//   < tool line
//   > . line 1: tool line
//   > OK
//
//   < tool wobble
//   > ERR line 1: 'wobble' is not a tool; known: select, point, ...
//
// Log lines are prefixed with ". " so a client can tell them from the verdict
// without parsing them. Every exchange ends with exactly one OK or ERR line,
// so a client knows when to stop reading.
class ScriptServer : public QObject {
    Q_OBJECT

public:
    // `document` and `afterChange` must outlive the server. `afterChange` is
    // called once per exchange that ran at least one command, and is what
    // makes the window show what just happened.
    ScriptServer(PartDocument& document, std::function<void()> afterChange,
                 QObject* parent = nullptr);
    ~ScriptServer() override;

    // Starts listening on 127.0.0.1:`port`. Returns false and fills `error` if
    // the port is taken or refused.
    bool listen(quint16 port, QString* error);

    // The port actually bound, or 0 when not listening.
    quint16 port() const;

    // How many clients have connected since the server started. Exists so the
    // self-test can say a connection ARRIVED rather than only that a command
    // ran -- the two fail differently.
    int connectionCount() const;

private:
    void acceptConnection();
    void readFrom(QTcpSocket* socket);

    PartDocument& document_;
    std::function<void()> afterChange_;
    QTcpServer* server_{nullptr};
    int connections_{0};
    // ONE SESSION PER CONNECTION: the current sketch, the current tool and the
    // names given so far belong to the conversation, not to the process. Two
    // clients would otherwise finish each other's splines.
    std::map<QTcpSocket*, std::unique_ptr<SketchScriptSession>> sessions_;
    std::map<QTcpSocket*, QByteArray> buffers_;
};

} // namespace paramcad

#include "Viewer/ScriptServer.h"

#include "Core/Document/PartDocument.h"

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <utility>

namespace paramcad {

ScriptServer::ScriptServer(PartDocument& document, std::function<void()> afterChange,
                           QObject* parent)
    : QObject(parent), document_(document), afterChange_(std::move(afterChange)) {}

ScriptServer::~ScriptServer() = default;

bool ScriptServer::listen(quint16 port, QString* error) {
    if (server_ != nullptr) {
        if (error != nullptr) *error = QStringLiteral("already listening");
        return false;
    }
    auto* server = new QTcpServer(this);
    // LOOPBACK, and there is no branch for anything else. See the header: this
    // executes commands that write files.
    if (!server->listen(QHostAddress::LocalHost, port)) {
        if (error != nullptr) *error = server->errorString();
        delete server;
        return false;
    }
    server_ = server;
    connect(server_, &QTcpServer::newConnection, this, &ScriptServer::acceptConnection);
    return true;
}

quint16 ScriptServer::port() const { return server_ == nullptr ? 0 : server_->serverPort(); }

int ScriptServer::connectionCount() const { return connections_; }

void ScriptServer::acceptConnection() {
    while (server_ != nullptr && server_->hasPendingConnections()) {
        QTcpSocket* socket = server_->nextPendingConnection();
        if (socket == nullptr) continue;
        ++connections_;
        sessions_[socket] = std::make_unique<SketchScriptSession>(document_);
        buffers_[socket] = QByteArray();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { readFrom(socket); });
        // The session and the buffer go when the connection does. A map that
        // only ever grew would keep a session per connection for the life of
        // the process, and the sessions hold names for geometry that may not
        // exist any more.
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            sessions_.erase(socket);
            buffers_.erase(socket);
            socket->deleteLater();
        });
        socket->write("OK ep3d listening; one command per line, `help` for the vocabulary\n");
    }
}

void ScriptServer::readFrom(QTcpSocket* socket) {
    const auto session = sessions_.find(socket);
    if (session == sessions_.end()) return;
    QByteArray& buffer = buffers_[socket];
    buffer.append(socket->readAll());

    // ONE LINE AT A TIME, and only COMPLETE lines: TCP is a stream, so a
    // command can arrive in two pieces and two commands can arrive in one.
    // Running whatever happened to be in the buffer would execute half a line.
    for (int newline = buffer.indexOf('\n'); newline >= 0; newline = buffer.indexOf('\n')) {
        const QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);

        const std::string text = line.toStdString();
        const ScriptOutcome outcome = session->second->run(text + "\n");
        // NO LINE NUMBER: every message is its own call, so every line is
        // line 1 and saying so on each of them is noise a reader has to skip.
        for (const ScriptLogEntry& entry : outcome.log)
            socket->write((". " + entry.text + "\n").c_str());
        if (outcome.ok) {
            socket->write("OK\n");
            // THE WINDOW, updated after every exchange that could have changed
            // something. A socket that quietly edited the document while the
            // view showed the old one would be worse than no socket at all.
            if (afterChange_) afterChange_();
        } else {
            socket->write(("ERR " + outcome.message + "\n").c_str());
        }
        socket->flush();
    }
}

} // namespace paramcad

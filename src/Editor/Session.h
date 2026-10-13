#pragma once

#include <Core/Core.h>

#include <Simfile/NoteList.h>

namespace Vortex {

/// Peer-to-peer co-editing session between one host and one guest.
///
/// The host's chart is the source of truth. Guest note edits are sent to the
/// host, applied through the normal editing path, and the result is broadcast
/// back. All network I/O runs on a worker thread; received messages are
/// handled on the main thread in tick().
struct Session {
    enum Role {
        ROLE_NONE,
        ROLE_HOST,
        ROLE_GUEST,
    };

    enum State {
        STATE_IDLE,
        STATE_LISTENING,   ///< Host waiting for a guest.
        STATE_CONNECTING,  ///< Guest connecting to the host.
        STATE_HANDSHAKE,   ///< Connected, verifying the song.
        STATE_CONNECTED,   ///< Song verified, edits are synced.
    };

    static const int DEFAULT_PORT = 7654;

    static void create();
    static void destroy();

    /// Called every frame by the editor; handles received messages.
    virtual void tick() = 0;

    /// Called by the editor when changes were made to the simfile.
    virtual void onChanges(int changes) = 0;

    /// Starts hosting a session on the given port.
    virtual bool host(int port) = 0;

    /// Joins a session at the given "address:port".
    virtual bool join(const std::string& address) = 0;

    /// Ends the current session, if any.
    virtual void disconnect() = 0;

    virtual Role getRole() const = 0;
    virtual State getState() const = 0;

    /// Returns a human readable description of the session state.
    virtual std::string getStatus() const = 0;

    /// Returns the address(es) a guest can use to join, when hosting.
    virtual std::string getShareAddress() const = 0;

    /// Returns true if local note edits must be forwarded to the host instead
    /// of being applied locally.
    virtual bool isGuest() const = 0;

    /// Returns true if undo/redo is disabled (guest during a session).
    virtual bool blocksUndo() const = 0;

    /// Called by the notes manager on the guest. Sends a resolved edit to the
    /// host. The edit is never applied locally.
    virtual void forwardEdit(const NoteList& add, const NoteList& rem) = 0;

    /// Called by the notes manager on the host, after notes were applied to a
    /// chart (edit, undo or redo).
    virtual void onNotesApplied(const Chart* chart, const NoteList& add,
                                const NoteList& rem) = 0;
};

extern Session* gSession;

};  // namespace Vortex

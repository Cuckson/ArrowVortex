#include <Editor/Session.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <Core/ByteStream.h>

#include <Editor/Common.h>
#include <Editor/History.h>
#include <Editor/Music.h>
#include <Editor/NetSocket.h>
#include <Editor/Sound.h>

#include <Managers/ChartMan.h>
#include <Managers/NoteMan.h>
#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>

#include <Simfile/Chart.h>
#include <Simfile/Simfile.h>
#include <Simfile/Tempo.h>

#include <System/File.h>

namespace Vortex {
namespace {

// ================================================================================================
// Protocol.
//
// Every frame is [u32 size][u8 type][payload], where size counts the type
// byte and the payload. Values are written in native (little-endian) byte
// order using WriteStream; note lists use NoteList::encode.

const uint32_t PROTOCOL_MAGIC = 0x504D5641;  // "AVMP"
const uint16_t PROTOCOL_VERSION = 1;
const uint32_t MAX_FRAME_SIZE = 16 * 1024 * 1024;

enum MessageType : uint8_t {
    MSG_HELLO = 1,           // guest -> host: magic, version, song info.
    MSG_SONG_CHECK = 2,      // host -> guest: ok, reason.
    MSG_CHART_SNAPSHOT = 3,  // host -> guest: chart id, notes, checksum.
    MSG_NOTE_ADD = 4,        // both: chart gen, flags, notes, checksum.
    MSG_NOTE_REMOVE = 5,     // both: chart gen, flags, notes, checksum.
    MSG_DISCONNECT = 6,      // both: reason.
    MSG_RESYNC_REQUEST = 7,  // guest -> host: asks for a new snapshot.
};

enum EditFlags : uint8_t {
    // The next NoteAdd/NoteRemove belongs to the same edit.
    EDIT_MORE_FOLLOWS = 1 << 0,
};

const int CONNECT_TIMEOUT_MS = 5000;
const int POLL_INTERVAL_MS = 20;

// ================================================================================================
// Song verification.

struct SongInfo {
    uint64_t audioHash = 0;
    double duration = 0.0;
    std::string title, artist;
    double offset = 0.0;
};

static void WriteSongInfo(WriteStream& out, const SongInfo& info) {
    out.write<uint64_t>(info.audioHash);
    out.write<double>(info.duration);
    out.writeStr(info.title);
    out.writeStr(info.artist);
    out.write<double>(info.offset);
}

static void ReadSongInfo(ReadStream& in, SongInfo& info) {
    in.read(info.audioHash);
    in.read(info.duration);
    in.readStr(info.title);
    in.readStr(info.artist);
    in.read(info.offset);
}

static bool HashFile(const fs::path& path, uint64_t& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    // 64-bit FNV-1a.
    uint64_t hash = 14695981039346656037ULL;
    std::vector<char> buffer(1 << 16);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto n = static_cast<size_t>(file.gcount());
        for (size_t i = 0; i < n; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    out = hash;
    return true;
}

static bool GetLocalSongInfo(SongInfo& out, std::string& error) {
    const Simfile* sim = gSimfile->get();
    if (!sim) {
        error = "Open a simfile first.";
        return false;
    }
    if (sim->music.empty()) {
        error = "The simfile has no music file.";
        return false;
    }
    if (!gMusic->getSamples().isCompleted()) {
        error = "Wait for the music to finish loading.";
        return false;
    }
    fs::path path = utf8ToPath(sim->dir) / utf8ToPath(sim->music);
    if (!HashFile(path, out.audioHash)) {
        error = "Could not read the music file.";
        return false;
    }
    out.duration = gMusic->getSongLength();
    out.title = sim->title;
    out.artist = sim->artist;
    out.offset = sim->tempo ? sim->tempo->offset : 0.0;
    return true;
}

// Returns an empty string if the songs match, or the reason they don't.
static std::string CompareSongs(const SongInfo& host, const SongInfo& guest) {
    if (host.audioHash != guest.audioHash) {
        return "The music files are different.";
    }
    if (std::abs(host.duration - guest.duration) > 0.001) {
        return std::format(
            "The music length differs (host {:.3f}s, guest "
            "{:.3f}s).",
            host.duration, guest.duration);
    }
    if (host.title != guest.title) {
        return std::format("The title differs (host \"{}\", guest \"{}\").",
                           host.title, guest.title);
    }
    if (host.artist != guest.artist) {
        return std::format("The artist differs (host \"{}\", guest \"{}\").",
                           host.artist, guest.artist);
    }
    if (std::abs(host.offset - guest.offset) > 0.0001) {
        return std::format("The offset differs (host {:.4f}, guest {:.4f}).",
                           host.offset, guest.offset);
    }
    return std::string();
}

// ================================================================================================
// Chart identification and checksums.

struct ChartId {
    std::string style;
    int32_t difficulty = 0;
    int32_t index = 0;  // Index among charts with the same style/difficulty.
};

static bool SameKind(const Chart* a, const Chart* b) {
    return a->style == b->style && a->difficulty == b->difficulty;
}

static ChartId GetChartId(const Simfile* sim, const Chart* chart) {
    ChartId id;
    id.style = chart->style ? chart->style->id : std::string();
    id.difficulty = chart->difficulty;
    for (const Chart* c : sim->charts) {
        if (c == chart) break;
        if (SameKind(c, chart)) ++id.index;
    }
    return id;
}

static const Chart* FindChart(const Simfile* sim, const ChartId& id) {
    int index = 0;
    for (const Chart* c : sim->charts) {
        std::string style = c->style ? c->style->id : std::string();
        if (style == id.style && c->difficulty == id.difficulty) {
            if (index == id.index) return c;
            ++index;
        }
    }
    return nullptr;
}

static uint32_t Checksum(const NoteList& notes) {
    // 32-bit FNV-1a over the note fields.
    uint32_t hash = 2166136261U;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            hash ^= (v >> (i * 8)) & 0xFF;
            hash *= 16777619U;
        }
    };
    for (const Note& n : notes) {
        mix(static_cast<uint32_t>(n.row));
        mix(static_cast<uint32_t>(n.endrow));
        mix(n.col | (n.player << 8) | (n.type << 12) | (n.quant << 16));
    }
    return hash;
}

// ================================================================================================
// NetLink: owns the worker thread and the message queues.

struct NetEvent {
    enum Type {
        CONNECTED,  // A peer connected (host) / connect succeeded (guest).
        LOST,       // The peer connection ended.
        FAILED,     // Connecting failed (guest).
        FRAME,      // A complete frame was received; data = [type][payload].
    };
    Type type = FRAME;
    std::vector<uint8_t> data;
    std::string text;
};

class NetLink {
   public:
    ~NetLink() { stop(); }

    void startHost(NetSocket listener) {
        stop();
        myThread = std::jthread(
            [this, l = std::move(listener)](std::stop_token st) mutable {
                myHostLoop(st, l);
            });
    }

    void startGuest(const std::string& host, int port) {
        stop();
        myThread = std::jthread([this, host, port](std::stop_token st) {
            myGuestLoop(st, host, port);
        });
    }

    // Stops the worker thread, after trying to flush pending frames.
    void stop() {
        if (myThread.joinable()) {
            myThread.request_stop();
            myThread.join();
        }
        std::lock_guard<std::mutex> lock(myMutex);
        myInbox.clear();
        myOutbox.clear();
        myDropPeer = false;
    }

    void send(std::vector<uint8_t> frame) {
        std::lock_guard<std::mutex> lock(myMutex);
        myOutbox.push_back(std::move(frame));
    }

    // Host: closes the current peer connection once pending frames are sent.
    void dropPeer() {
        std::lock_guard<std::mutex> lock(myMutex);
        myDropPeer = true;
    }

    bool poll(NetEvent& out) {
        std::lock_guard<std::mutex> lock(myMutex);
        if (myInbox.empty()) return false;
        out = std::move(myInbox.front());
        myInbox.pop_front();
        return true;
    }

   private:
    void myPush(NetEvent::Type type, std::string text) {
        NetEvent evt;
        evt.type = type;
        evt.text = std::move(text);
        std::lock_guard<std::mutex> lock(myMutex);
        myInbox.push_back(std::move(evt));
    }

    void myHostLoop(const std::stop_token& st, NetSocket& listener) {
        while (!st.stop_requested()) {
            if (!(listener.poll(false, 50) & NetSocket::POLL_READ)) continue;
            NetSocket peer = listener.accept();
            if (!peer.isValid()) continue;
            {
                std::lock_guard<std::mutex> lock(myMutex);
                myOutbox.clear();
                myDropPeer = false;
            }
            myPush(NetEvent::CONNECTED, std::string());
            std::string reason = myServe(st, peer, &listener);
            myPush(NetEvent::LOST, reason);
        }
    }

    void myGuestLoop(const std::stop_token& st, const std::string& host,
                     int port) {
        NetSocket sock;
        std::string error;
        if (!sock.beginConnect(host, port, error)) {
            myPush(NetEvent::FAILED, error);
            return;
        }
        bool connected = false;
        for (int waited = 0; waited < CONNECT_TIMEOUT_MS && !connected;
             waited += 50) {
            if (st.stop_requested()) return;
            int flags = sock.poll(true, 50);
            if (flags & (NetSocket::POLL_WRITE | NetSocket::POLL_ERROR)) {
                if (!sock.finishConnect(error)) {
                    myPush(NetEvent::FAILED, error);
                    return;
                }
                connected = true;
            }
        }
        if (!connected) {
            myPush(NetEvent::FAILED,
                   std::format("Timed out connecting to {}:{}.", host, port));
            return;
        }
        myPush(NetEvent::CONNECTED, std::string());
        std::string reason = myServe(st, sock, nullptr);
        if (!st.stop_requested()) myPush(NetEvent::LOST, reason);
    }

    // Moves frames between the socket and the queues until the connection
    // ends. Returns the reason it ended.
    std::string myServe(const std::stop_token& st, NetSocket& peer,
                        NetSocket* listener) {
        std::vector<uint8_t> readBuf, writeBuf;
        std::vector<uint8_t> chunk(1 << 16);
        size_t writePos = 0;
        bool dropping = false;
        int flushAttempts = 0;

        while (true) {
            // Collect outgoing frames.
            {
                std::lock_guard<std::mutex> lock(myMutex);
                for (auto& frame : myOutbox) {
                    writeBuf.insert(writeBuf.end(), frame.begin(), frame.end());
                }
                myOutbox.clear();
                dropping |= myDropPeer;
                myDropPeer = false;
            }
            bool pending = writePos < writeBuf.size();

            // On stop or drop, close once everything is sent (or after a
            // short grace period).
            if (st.stop_requested() || dropping) {
                if (!pending || ++flushAttempts > 10) {
                    return "Connection closed.";
                }
            }

            int flags = peer.poll(pending, POLL_INTERVAL_MS);
            if (flags & NetSocket::POLL_ERROR) return "Connection error.";

            if (flags & NetSocket::POLL_READ) {
                int n = peer.recv(chunk.data(), static_cast<int>(chunk.size()));
                if (n < 0) return "The other side closed the connection.";
                readBuf.insert(readBuf.end(), chunk.begin(), chunk.begin() + n);
                if (!myParseFrames(readBuf)) return "Protocol error.";
            }

            if (pending && (flags & NetSocket::POLL_WRITE)) {
                int n = peer.send(writeBuf.data() + writePos,
                                  static_cast<int>(writeBuf.size() - writePos));
                if (n < 0) return "Connection error.";
                writePos += static_cast<size_t>(n);
                if (writePos == writeBuf.size()) {
                    writeBuf.clear();
                    writePos = 0;
                }
            }

            // Only one guest at a time; turn away anyone else.
            if (listener && (listener->poll(false, 0) & NetSocket::POLL_READ)) {
                listener->accept().close();
            }
        }
    }

    // Extracts complete frames from the buffer. Returns false on bad data.
    bool myParseFrames(std::vector<uint8_t>& buf) {
        size_t pos = 0;
        while (buf.size() - pos >= 4) {
            uint32_t size = static_cast<uint32_t>(buf[pos]) |
                            (static_cast<uint32_t>(buf[pos + 1]) << 8) |
                            (static_cast<uint32_t>(buf[pos + 2]) << 16) |
                            (static_cast<uint32_t>(buf[pos + 3]) << 24);
            if (size == 0 || size > MAX_FRAME_SIZE) return false;
            if (buf.size() - pos - 4 < size) break;

            NetEvent evt;
            evt.type = NetEvent::FRAME;
            auto begin = buf.begin() + static_cast<ptrdiff_t>(pos + 4);
            evt.data.assign(begin, begin + size);
            {
                std::lock_guard<std::mutex> lock(myMutex);
                myInbox.push_back(std::move(evt));
            }
            pos += 4 + size;
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(pos));
        return true;
    }

    std::jthread myThread;
    std::mutex myMutex;
    std::deque<NetEvent> myInbox;
    std::deque<std::vector<uint8_t>> myOutbox;
    bool myDropPeer = false;
};

// ================================================================================================
// SessionImpl.

const NotesMan::EditDescription GuestEditDesc = {"Guest edited %1 note",
                                                 "Guest edited %1 notes"};

struct SessionImpl : public Session {
    NetLink myLink;
    bool myNetReady = false;

    Role myRole = ROLE_NONE;
    State myState = STATE_IDLE;

    std::string myAddress;
    std::string myShareAddress;
    std::string myOtherAddresses;
    int myPort = DEFAULT_PORT;

    const Simfile* mySimfile = nullptr;
    SongInfo mySong;

    // Host: the chart last sent to the guest. Guest: the local chart that
    // matches the host's chart, or null if there is none.
    const Chart* myChart = nullptr;
    uint32_t myChartGen = 0;
    uint32_t myLastChecksum = 0;
    bool myFollowHostChart = false;

    // Notes of a NoteRemove/NoteAdd sequence that is still incomplete.
    NoteList myPendingAdd, myPendingRem;

    // ================================================================================================
    // SessionImpl :: constructor and destructor.

    ~SessionImpl() {
        disconnect();
        if (myNetReady) NetSocket::shutdown();
    }

    SessionImpl() { myNetReady = NetSocket::startup(); }

    // ================================================================================================
    // SessionImpl :: sending.

    void mySend(MessageType type, const WriteStream& payload) {
        auto size = static_cast<uint32_t>(payload.size() + 1);
        std::vector<uint8_t> frame(4 + size);
        frame[0] = static_cast<uint8_t>(size);
        frame[1] = static_cast<uint8_t>(size >> 8);
        frame[2] = static_cast<uint8_t>(size >> 16);
        frame[3] = static_cast<uint8_t>(size >> 24);
        frame[4] = type;
        if (payload.size() > 0) {
            memcpy(frame.data() + 5, payload.data(), payload.size());
        }
        myLink.send(std::move(frame));
    }

    void mySendDisconnect(const std::string& reason) {
        WriteStream out;
        out.writeStr(reason);
        mySend(MSG_DISCONNECT, out);
    }

    void mySendNotes(MessageType type, uint8_t flags, const NoteList& notes,
                     uint32_t checksum) {
        WriteStream out;
        out.write<uint32_t>(myChartGen);
        out.write<uint8_t>(flags);
        notes.encode(out, false);
        out.write<uint32_t>(checksum);
        mySend(type, out);
    }

    void mySendEdit(const NoteList& add, const NoteList& rem,
                    uint32_t checksum) {
        if (!rem.empty()) {
            uint8_t flags = add.empty() ? 0 : EDIT_MORE_FOLLOWS;
            mySendNotes(MSG_NOTE_REMOVE, flags, rem, checksum);
        }
        if (!add.empty()) {
            mySendNotes(MSG_NOTE_ADD, 0, add, checksum);
        }
    }

    void mySendSnapshot() {
        const Chart* chart = gChart->get();
        myChart = chart;
        myChartGen++;

        WriteStream out;
        out.write<uint32_t>(myChartGen);
        out.write<uint8_t>(chart ? 1 : 0);
        if (chart) {
            ChartId id = GetChartId(gSimfile->get(), chart);
            out.writeStr(id.style);
            out.write<int32_t>(id.difficulty);
            out.write<int32_t>(id.index);
            chart->notes.encode(out, false);
            myLastChecksum = Checksum(chart->notes);
        } else {
            myLastChecksum = 0;
        }
        out.write<uint32_t>(myLastChecksum);
        mySend(MSG_CHART_SNAPSHOT, out);
    }

    // ================================================================================================
    // SessionImpl :: public API.

    bool host(int port) override {
        disconnect();
        if (!myNetReady) {
            HudError("Networking is not available.");
            return false;
        }
        std::string error;
        if (!GetLocalSongInfo(mySong, error)) {
            HudError("Cannot host: %s", error.c_str());
            return false;
        }
        NetSocket listener;
        if (!listener.listen(port, error)) {
            HudError("Cannot host: %s", error.c_str());
            return false;
        }

        auto addresses = NetSocket::localAddresses();
        std::string portStr = std::to_string(port);
        myShareAddress =
            (addresses.empty() ? std::string("127.0.0.1") : addresses[0]) +
            ":" + portStr;
        myOtherAddresses.clear();
        for (size_t i = 1; i < addresses.size(); ++i) {
            myOtherAddresses += " " + addresses[i] + ":" + portStr;
        }

        myPort = port;
        myRole = ROLE_HOST;
        myState = STATE_LISTENING;
        mySimfile = gSimfile->get();
        myChart = nullptr;
        myLink.startHost(std::move(listener));
        HudInfo("Hosting session at %s", myShareAddress.c_str());
        return true;
    }

    bool join(const std::string& address) override {
        disconnect();
        if (!myNetReady) {
            HudError("Networking is not available.");
            return false;
        }

        std::string hostName = address;
        int port = DEFAULT_PORT;
        auto colon = address.rfind(':');
        if (colon != std::string::npos) {
            hostName = address.substr(0, colon);
            port = std::atoi(address.c_str() + colon + 1);
        }
        while (!hostName.empty() && hostName.front() == ' ') {
            hostName.erase(0, 1);
        }
        while (!hostName.empty() && hostName.back() == ' ') hostName.pop_back();
        if (hostName.empty() || port <= 0 || port > 65535) {
            HudError("Enter the host address as ip:port.");
            return false;
        }

        if (gHistory->hasUnsavedChanges()) {
            HudError(
                "Save your changes before joining; the host's chart will "
                "replace your notes.");
            return false;
        }
        std::string error;
        if (!GetLocalSongInfo(mySong, error)) {
            HudError("Cannot join: %s", error.c_str());
            return false;
        }

        myAddress = hostName + ":" + std::to_string(port);
        myRole = ROLE_GUEST;
        myState = STATE_CONNECTING;
        mySimfile = gSimfile->get();
        myChart = nullptr;
        myChartGen = 0;
        myLink.startGuest(hostName, port);
        HudInfo("Connecting to %s...", myAddress.c_str());
        return true;
    }

    void disconnect() override {
        if (myRole == ROLE_NONE) return;
        if (myState == STATE_HANDSHAKE || myState == STATE_CONNECTED) {
            mySendDisconnect(myRole == ROLE_HOST ? "The host ended the session."
                                                 : "The guest left.");
        }
        myEndSession();
        HudInfo("Session ended.");
    }

    void myEndSession() {
        myLink.stop();
        Role role = myRole;
        myRole = ROLE_NONE;
        myState = STATE_IDLE;
        myChart = nullptr;
        mySimfile = nullptr;
        myFollowHostChart = false;
        myPendingAdd.clear();
        myPendingRem.clear();

        // Entries the guest created during the session (e.g. tempo edits)
        // describe diffs against a state that no longer exists.
        if (role == ROLE_GUEST && gSimfile->isOpen()) gHistory->clearAll();
    }

    Role getRole() const override { return myRole; }

    State getState() const override { return myState; }

    std::string getStatus() const override {
        switch (myState) {
            case STATE_IDLE:
                return "Not connected.";
            case STATE_LISTENING:
                return "Hosting, waiting for a guest." +
                       (myOtherAddresses.empty()
                            ? std::string()
                            : " Other addresses:" + myOtherAddresses);
            case STATE_CONNECTING:
                return "Connecting to " + myAddress + "...";
            case STATE_HANDSHAKE:
                return "Verifying song...";
            case STATE_CONNECTED:
                if (myRole == ROLE_HOST) return "Guest connected.";
                if (!myChart) {
                    return "Connected. The host's chart is not in your "
                           "simfile; edits are paused.";
                }
                return "Connected to " + myAddress + ".";
        }
        return std::string();
    }

    std::string getShareAddress() const override {
        return myRole == ROLE_HOST ? myShareAddress : std::string();
    }

    bool isGuest() const override {
        return myRole == ROLE_GUEST && myState != STATE_IDLE;
    }

    bool blocksUndo() const override { return isGuest(); }

    // ================================================================================================
    // SessionImpl :: editing hooks.

    void forwardEdit(const NoteList& add, const NoteList& rem) override {
        if (add.empty() && rem.empty()) return;
        if (myState != STATE_CONNECTED) {
            HudWarning("Not connected to the host yet; edit ignored.");
            return;
        }
        if (!myChart || gChart->get() != myChart) {
            HudWarning("The host's chart is not open; edit ignored.");
            return;
        }
        mySendEdit(add, rem, 0);
    }

    void onNotesApplied(const Chart* chart, const NoteList& add,
                        const NoteList& rem) override {
        if (myRole != ROLE_HOST || myState != STATE_CONNECTED) return;
        if (chart != myChart || (add.empty() && rem.empty())) return;
        myLastChecksum = Checksum(chart->notes);
        mySendEdit(add, rem, myLastChecksum);
    }

    void onChanges(int changes) override {
        if (myRole == ROLE_NONE) return;

        // The session is bound to the simfile that was open when it started.
        if ((changes & VCM_FILE_CHANGED) || gSimfile->get() != mySimfile) {
            mySendDisconnect("The song was closed.");
            myEndSession();
            HudInfo("Session ended because the song was closed.");
            return;
        }

        if (myState != STATE_CONNECTED) return;

        if (myRole == ROLE_HOST &&
            (changes & (VCM_CHART_CHANGED | VCM_NOTES_CHANGED))) {
            // Send a snapshot if the chart changed, or if the notes changed
            // in a way that was not broadcast as an edit (e.g. insert rows).
            const Chart* chart = gChart->get();
            if (chart != myChart ||
                (chart && Checksum(chart->notes) != myLastChecksum)) {
                mySendSnapshot();
            }
        }

        if (myRole == ROLE_GUEST && (changes & VCM_CHART_CHANGED)) {
            // The guest follows the host's chart. Switching charts from
            // within onChanges would lose the change notification, so it is
            // deferred to the next tick.
            if (myChart && gChart->get() != myChart) myFollowHostChart = true;
        }
    }

    // ================================================================================================
    // SessionImpl :: receiving.

    void tick() override {
        if (myRole == ROLE_NONE) return;

        NetEvent evt;
        while (myRole != ROLE_NONE && myLink.poll(evt)) {
            if (myRole == ROLE_HOST) {
                myHandleHostEvent(evt);
            } else {
                myHandleGuestEvent(evt);
            }
        }

        if (myFollowHostChart) {
            myFollowHostChart = false;
            if (isGuest() && myChart && gChart->get() != myChart) {
                gSimfile->openChart(myChart);
                HudNote("Following the host's chart.");
            }
        }
    }

    // Reads a NoteAdd/NoteRemove message. Returns true when the edit is
    // complete and the pending lists should be applied.
    bool myReadEditPart(MessageType type, ReadStream& in, uint32_t& gen,
                        uint32_t& checksum) {
        uint8_t flags = 0;
        NoteList notes;
        in.read(gen);
        in.read(flags);
        notes.decode(in, 0);
        in.read(checksum);
        if (!in.success()) {
            myPendingAdd.clear();
            myPendingRem.clear();
            return false;
        }
        NoteList& target = (type == MSG_NOTE_ADD) ? myPendingAdd : myPendingRem;
        target.insert(notes);
        return (flags & EDIT_MORE_FOLLOWS) == 0;
    }

    void myHandleHostEvent(NetEvent& evt) {
        switch (evt.type) {
            case NetEvent::CONNECTED:
                myState = STATE_HANDSHAKE;
                HudInfo("Guest connected, verifying song...");
                return;
            case NetEvent::LOST:
                if (myState == STATE_CONNECTED) {
                    HudInfo("Guest disconnected: %s", evt.text.c_str());
                }
                myState = STATE_LISTENING;
                myChart = nullptr;
                myPendingAdd.clear();
                myPendingRem.clear();
                return;
            case NetEvent::FAILED:
                return;
            case NetEvent::FRAME:
                break;
        };

        auto type = static_cast<MessageType>(evt.data[0]);
        ReadStream in(evt.data.data() + 1,
                      static_cast<int>(evt.data.size() - 1));

        if (myState == STATE_HANDSHAKE) {
            if (type != MSG_HELLO) return;
            uint32_t magic = 0;
            uint16_t version = 0;
            SongInfo guestSong;
            in.read(magic);
            in.read(version);
            if (in.success() && magic == PROTOCOL_MAGIC &&
                version == PROTOCOL_VERSION) {
                ReadSongInfo(in, guestSong);
            }

            std::string reason;
            if (!in.success() || magic != PROTOCOL_MAGIC) {
                reason = "Not an ArrowVortex session.";
            } else if (version != PROTOCOL_VERSION) {
                reason = std::format(
                    "Protocol version mismatch (host {}, guest {}).",
                    PROTOCOL_VERSION, version);
            } else {
                reason = CompareSongs(mySong, guestSong);
            }

            WriteStream out;
            out.write<uint8_t>(reason.empty() ? 1 : 0);
            out.writeStr(reason);
            mySend(MSG_SONG_CHECK, out);

            if (reason.empty()) {
                myState = STATE_CONNECTED;
                myChart = nullptr;
                mySendSnapshot();
                HudInfo("Guest joined the session.");
            } else {
                myLink.dropPeer();
                myState = STATE_LISTENING;
                HudWarning("Rejected guest: %s", reason.c_str());
            }
            return;
        }

        if (myState != STATE_CONNECTED) return;

        switch (type) {
            case MSG_NOTE_ADD:
            case MSG_NOTE_REMOVE: {
                uint32_t gen = 0, checksum = 0;
                if (!myReadEditPart(type, in, gen, checksum)) return;
                NoteEdit edit;
                edit.add = myPendingAdd;
                edit.rem = myPendingRem;
                myPendingAdd.clear();
                myPendingRem.clear();
                // Ignore edits made against a chart the host has left.
                if (gen == myChartGen && myChart && gChart->get() == myChart) {
                    // The normal editing path: creates a host history entry,
                    // and the result is broadcast by onNotesApplied.
                    gNotes->modify(edit, false, &GuestEditDesc);
                }
                return;
            }
            case MSG_RESYNC_REQUEST:
                mySendSnapshot();
                return;
            case MSG_DISCONNECT: {
                std::string reason = in.readStr();
                HudInfo("Guest left: %s", reason.c_str());
                myLink.dropPeer();
                myState = STATE_LISTENING;
                myChart = nullptr;
                return;
            }
            default:
                return;
        }
    }

    void myHandleGuestEvent(NetEvent& evt) {
        switch (evt.type) {
            case NetEvent::CONNECTED: {
                myState = STATE_HANDSHAKE;
                WriteStream out;
                out.write<uint32_t>(PROTOCOL_MAGIC);
                out.write<uint16_t>(PROTOCOL_VERSION);
                WriteSongInfo(out, mySong);
                mySend(MSG_HELLO, out);
                return;
            }
            case NetEvent::FAILED:
                HudError("Could not join: %s", evt.text.c_str());
                myEndSession();
                return;
            case NetEvent::LOST:
                HudError("Lost connection to the host: %s", evt.text.c_str());
                myEndSession();
                return;
            case NetEvent::FRAME:
                break;
        };

        auto type = static_cast<MessageType>(evt.data[0]);
        ReadStream in(evt.data.data() + 1,
                      static_cast<int>(evt.data.size() - 1));

        switch (type) {
            case MSG_SONG_CHECK: {
                uint8_t ok = 0;
                in.read(ok);
                std::string reason = in.readStr();
                if (in.success() && ok) {
                    myState = STATE_CONNECTED;
                    // The snapshot replaces the notes; old diffs are invalid.
                    gHistory->clearAll();
                    HudInfo("Joined the session at %s.", myAddress.c_str());
                } else {
                    HudError("Join rejected: %s", reason.c_str());
                    myEndSession();
                }
                return;
            }
            case MSG_CHART_SNAPSHOT:
                myApplySnapshot(in);
                return;
            case MSG_NOTE_ADD:
            case MSG_NOTE_REMOVE: {
                uint32_t gen = 0, checksum = 0;
                if (!myReadEditPart(type, in, gen, checksum)) return;
                NoteList add = myPendingAdd;
                NoteList rem = myPendingRem;
                myPendingAdd.clear();
                myPendingRem.clear();
                if (gen != myChartGen || !myChart) return;
                if (gChart->get() != myChart) {
                    // Will be corrected once the guest is back on the chart.
                    mySend(MSG_RESYNC_REQUEST, WriteStream());
                    myFollowHostChart = true;
                    return;
                }
                gNotes->applyRemoteEdit(add, rem);
                gHistory->markExternalChange();
                if (Checksum(myChart->notes) != checksum) {
                    mySend(MSG_RESYNC_REQUEST, WriteStream());
                }
                return;
            }
            case MSG_DISCONNECT: {
                std::string reason = in.readStr();
                HudInfo("Session ended: %s", reason.c_str());
                myEndSession();
                return;
            }
            default:
                return;
        }
    }

    void myApplySnapshot(ReadStream& in) {
        uint32_t gen = 0, checksum = 0;
        uint8_t hasChart = 0;
        ChartId id;
        NoteList notes;
        in.read(gen);
        in.read(hasChart);
        if (hasChart) {
            in.readStr(id.style);
            in.read(id.difficulty);
            in.read(id.index);
            notes.decode(in, 0);
        }
        in.read(checksum);
        if (!in.success()) {
            HudError("Received an invalid chart snapshot.");
            return;
        }

        myChartGen = gen;
        myPendingAdd.clear();
        myPendingRem.clear();

        if (!hasChart) {
            myChart = nullptr;
            HudInfo("The host has no chart open.");
            return;
        }

        const Chart* chart = FindChart(gSimfile->get(), id);
        if (!chart) {
            myChart = nullptr;
            const char* diff =
                GetDifficultyName(static_cast<Difficulty>(id.difficulty));
            HudError(
                "The host's chart (%s %s #%d) is not in your simfile; edits "
                "are paused.",
                id.style.c_str(), diff, id.index + 1);
            return;
        }

        myChart = chart;
        if (gChart->get() != chart) gSimfile->openChart(chart);

        uint32_t before = Checksum(chart->notes);
        gNotes->replaceNotes(notes);
        uint32_t after = Checksum(chart->notes);
        if (before != after) gHistory->markExternalChange();
        if (after != checksum) {
            HudWarning("Chart data differs from the host after sync.");
        }
    }
};

};  // anonymous namespace

// ================================================================================================
// Session API.

Session* gSession = nullptr;

void Session::create() { gSession = new SessionImpl; }

void Session::destroy() {
    delete static_cast<SessionImpl*>(gSession);
    gSession = nullptr;
}

};  // namespace Vortex

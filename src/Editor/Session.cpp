#include <Editor/Session.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_mouse.h>

#include <Core/ByteStream.h>
#include <Core/Draw.h>
#include <Core/Text.h>
#include <Core/Utils.h>
#include <Core/Xmr.h>

#include <Editor/Common.h>
#include <Editor/Editor.h>
#include <Editor/History.h>
#include <Editor/Music.h>
#include <Editor/NetSocket.h>
#include <Editor/TextOverlay.h>
#include <Editor/View.h>

#include <Managers/ChartMan.h>
#include <Managers/NoteMan.h>
#include <Managers/SimfileMan.h>
#include <Managers/StyleMan.h>
#include <Managers/TempoMan.h>

#include <Simfile/Chart.h>
#include <Simfile/SegmentGroup.h>
#include <Simfile/Simfile.h>
#include <Simfile/Tempo.h>

#include <System/File.h>
#include <System/System.h>

namespace Vortex {
namespace {

// ================================================================================================
// Protocol.
//
// Every frame is [u32 size][u8 type][payload], where size counts the type
// byte and the payload. Values are written in native (little-endian) byte
// order using WriteStream; note lists use NoteList::encode.
//
// The host relays everything: guests only talk to the host, and the host
// forwards changes to the other guests, tagged with the id of the user that
// made them ("origin"). The host is user 0; guests get ids from 1 upwards.

const uint32_t PROTOCOL_MAGIC = 0x504D5641;  // "AVMP"
const uint16_t PROTOCOL_VERSION = 2;
const uint32_t MAX_FRAME_SIZE = 64 * 1024 * 1024;

const uint32_t HOST_ID = 0;
const int MAX_GUESTS = 16;

enum MessageType : uint8_t {
    MSG_HELLO = 1,           // G->H: magic, version, name, song info.
    MSG_WELCOME = 2,         // H->G: ok, reason, guest id.
    MSG_FULL_STATE = 3,      // H->G: song sections and all charts.
    MSG_PEER_LIST = 4,       // H->G: count, [id, name].
    MSG_NOTE_EDIT = 5,       // both: origin, chart, add, rem, checksum.
    MSG_NOTE_REPLACE = 6,    // both: origin, chart, notes, checksum.
    MSG_STATE = 7,           // both: origin, section, id, data.
    MSG_PRESENCE = 8,        // both: origin, chart, hover, times.
    MSG_RESYNC_REQUEST = 9,  // G->H: chart.
    MSG_DISCONNECT = 10,     // both: reason.
};

// Song and chart data other than notes is synced as "sections": each section
// is serialized as a whole, and the last version received by the host wins.
enum SectionType : uint8_t {
    SECTION_SONG_META = 1,    // id 0: title, artist, etc.
    SECTION_TEMPO = 2,        // id 0: song timing, otherwise chart timing.
    SECTION_CHART_PROPS = 3,  // id: chart step artist, difficulty, meter.
};

const int CONNECT_TIMEOUT_MS = 5000;
const int POLL_INTERVAL_MS = 20;
const int PRESENCE_INTERVAL_MS = 33;

typedef std::vector<uint8_t> Bytes;

static uint64_t HashBytes(const uint8_t* data, size_t size) {
    // 64-bit FNV-1a.
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t HashBytes(const Bytes& bytes) {
    return HashBytes(bytes.data(), bytes.size());
}

static Bytes ToBytes(const WriteStream& out) {
    return Bytes(out.data(), out.data() + out.size());
}

static void WriteBytes(WriteStream& out, const Bytes& bytes) {
    out.writeNum(static_cast<uint32_t>(bytes.size()));
    if (!bytes.empty()) out.write(bytes.data(), static_cast<int>(bytes.size()));
}

static Bytes ReadBytes(ReadStream& in) {
    uint32_t size = in.readNum();
    if (!in.success() || size > in.bytesleft()) {
        in.invalidate();
        return Bytes();
    }
    Bytes out(in.pos(), in.pos() + size);
    in.skip(static_cast<int>(size));
    return out;
}

// ================================================================================================
// Song verification.

struct SongInfo {
    uint64_t audioHash = 0;
    double duration = 0.0;
};

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
    return true;
}

// Returns an empty string if the songs match, or the reason they don't. The
// rest of the simfile (metadata, timing, charts) is copied from the host.
static std::string CompareSongs(const SongInfo& host, const SongInfo& guest) {
    if (host.audioHash != guest.audioHash) {
        return "The music files are different.";
    }
    if (std::abs(host.duration - guest.duration) > 0.001) {
        return std::format(
            "The music length differs (host {:.3f}s, guest {:.3f}s).",
            host.duration, guest.duration);
    }
    return std::string();
}

// ================================================================================================
// Serialization of the synced data.

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

static Bytes WriteSongMeta(const Simfile* sim) {
    WriteStream out;
    for (const std::string* s :
         {&sim->title, &sim->titleTr, &sim->subtitle, &sim->subtitleTr,
          &sim->artist, &sim->artistTr, &sim->genre, &sim->credit, &sim->banner,
          &sim->background, &sim->cdTitle, &sim->lyricsPath}) {
        out.writeStr(*s);
    }
    out.write<double>(sim->previewStart);
    out.write<double>(sim->previewLength);
    out.write<uint8_t>(sim->isSelectable ? 1 : 0);
    return ToBytes(out);
}

static bool ApplySongMeta(Simfile* sim, const Bytes& data) {
    ReadStream in(data.data(), static_cast<int>(data.size()));
    std::string s[12];
    for (auto& str : s) in.readStr(str);
    double previewStart = in.read<double>();
    double previewLength = in.read<double>();
    uint8_t selectable = in.read<uint8_t>();
    if (!in.success()) return false;

    int changes = VCM_SONG_PROPERTIES_CHANGED;
    if (sim->banner != s[8]) changes |= VCM_BANNER_PATH_CHANGED;
    if (sim->background != s[9]) changes |= VCM_BACKGROUND_PATH_CHANGED;

    std::string* fields[12] = {
        &sim->title,  &sim->titleTr,    &sim->subtitle, &sim->subtitleTr,
        &sim->artist, &sim->artistTr,   &sim->genre,    &sim->credit,
        &sim->banner, &sim->background, &sim->cdTitle,  &sim->lyricsPath};
    for (int i = 0; i < 12; ++i) *fields[i] = s[i];
    sim->previewStart = previewStart;
    sim->previewLength = previewLength;
    sim->isSelectable = selectable != 0;
    gEditor->reportChanges(changes);
    return true;
}

// Writes a tempo, or an empty marker if it is null.
static Bytes WriteTempo(const Tempo* tempo) {
    WriteStream out;
    out.write<uint8_t>(tempo ? 1 : 0);
    if (!tempo) return ToBytes(out);

    out.write<double>(tempo->offset);
    out.write<int32_t>(tempo->displayBpmType);
    out.write<double>(tempo->displayBpmRange.min);
    out.write<double>(tempo->displayBpmRange.max);
    tempo->segments->encode(out);

    out.writeNum(static_cast<uint32_t>(tempo->attacks.size()));
    for (const Attack& a : tempo->attacks) {
        out.write<double>(a.time);
        out.write<double>(a.duration);
        out.writeStr(a.mods);
        out.write<int32_t>(a.unit);
    }
    out.writeNum(static_cast<uint32_t>(tempo->keysounds.size()));
    for (const std::string& k : tempo->keysounds) out.writeStr(k);
    out.writeNum(static_cast<uint32_t>(tempo->misc.size()));
    for (const Property& p : tempo->misc) {
        out.writeStr(p.tag);
        out.writeStr(p.val);
    }
    return ToBytes(out);
}

// Reads a tempo written by WriteTempo. Returns false on bad data; otherwise
// sets hasTempo and, if it is true, fills out.
static bool ReadTempo(const Bytes& data, bool& hasTempo, Tempo& out) {
    ReadStream in(data.data(), static_cast<int>(data.size()));
    hasTempo = in.read<uint8_t>() != 0;
    if (!in.success()) return false;
    if (!hasTempo) return true;

    in.read(out.offset);
    int32_t bpmType = in.read<int32_t>();
    in.read(out.displayBpmRange.min);
    in.read(out.displayBpmRange.max);
    if (bpmType < BPM_ACTUAL || bpmType > BPM_RANDOM) in.invalidate();
    out.displayBpmType = static_cast<DisplayBpm>(bpmType);
    out.segments->clear();
    out.segments->decode(in);

    uint32_t count = in.readNum();
    for (uint32_t i = 0; i < count && in.success(); ++i) {
        Attack a;
        in.read(a.time);
        in.read(a.duration);
        in.readStr(a.mods);
        a.unit = static_cast<AttackUnit>(in.read<int32_t>());
        out.attacks.push_back(a);
    }
    count = in.readNum();
    for (uint32_t i = 0; i < count && in.success(); ++i) {
        out.keysounds.push_back(in.readStr());
    }
    count = in.readNum();
    for (uint32_t i = 0; i < count && in.success(); ++i) {
        Property p;
        in.readStr(p.tag);
        in.readStr(p.val);
        out.misc.push_back(p);
    }
    return in.success();
}

// The chart tempo is only written if the chart has split timing, so that an
// empty chart tempo and no chart tempo serialize the same.
static Bytes WriteChartTempo(const Chart* chart) {
    return WriteTempo(chart->hasTempo() ? chart->tempo : nullptr);
}

static void SetChartTempo(Chart* chart, bool hasTempo, const Tempo& tempo) {
    if (hasTempo) {
        if (!chart->tempo) chart->tempo = new Tempo;
        chart->tempo->copy(&tempo);
    } else if (chart->tempo) {
        // Clear instead of deleting; history entries can point to it.
        Tempo empty;
        chart->tempo->copy(&empty);
    }
}

struct ChartProps {
    std::string artist;
    int32_t difficulty = DIFF_CHALLENGE;
    int32_t meter = 1;
};

static Bytes WriteChartProps(const Chart* chart) {
    WriteStream out;
    out.writeStr(chart->artist);
    out.write<int32_t>(chart->difficulty);
    out.write<int32_t>(chart->meter);
    return ToBytes(out);
}

static bool ReadChartProps(const Bytes& data, ChartProps& out) {
    ReadStream in(data.data(), static_cast<int>(data.size()));
    in.readStr(out.artist);
    in.read(out.difficulty);
    in.read(out.meter);
    if (out.difficulty < 0 || out.difficulty >= NUM_DIFFICULTIES) {
        in.invalidate();
    }
    return in.success();
}

static void SetChartProps(Chart* chart, const ChartProps& props) {
    chart->artist = props.artist;
    chart->difficulty = static_cast<Difficulty>(props.difficulty);
    chart->meter = props.meter;
}

static uint64_t SectionKey(SectionType type, uint32_t id) {
    return (static_cast<uint64_t>(type) << 32) | id;
}

// ================================================================================================
// Drawing helpers.

static const uint32_t PeerColors[] = {
    RGBAtoColor32(255, 170, 60, 255),  RGBAtoColor32(80, 200, 255, 255),
    RGBAtoColor32(140, 230, 90, 255),  RGBAtoColor32(255, 110, 160, 255),
    RGBAtoColor32(190, 140, 255, 255), RGBAtoColor32(255, 230, 80, 255),
    RGBAtoColor32(90, 230, 200, 255),  RGBAtoColor32(255, 120, 90, 255),
};

static uint32_t PeerColor(uint32_t id) {
    return PeerColors[id % (sizeof(PeerColors) / sizeof(PeerColors[0]))];
}

// A mouse pointer; 'X' is the outline, '.' the fill.
static const char* const CursorShape[] = {
    "X",        "XX",       "X.X",       "X..X",       "X...X",       "X....X",
    "X.....X",  "X......X", "X.......X", "X........X", "X.....XXXXX", "X..X..X",
    "X.X X..X", "XX  X..X", "X    X..X", "     X..X",  "      XX",
};

static void DrawPointer(int x, int y, uint32_t color) {
    int s = std::max(1, gSystem->applyScaleFactor(1));
    uint32_t outline = RGBAtoColor32(0, 0, 0, 150);
    uint32_t fill = Color32a(color, 140);
    int row = 0;
    for (const char* line : CursorShape) {
        int len = static_cast<int>(strlen(line));
        for (int col = 0; col < len;) {
            char c = line[col];
            int end = col;
            while (end < len && line[end] == c) ++end;
            if (c != ' ') {
                Draw::fill({x + col * s, y + row * s, (end - col) * s, s},
                           c == 'X' ? outline : fill);
            }
            col = end;
        }
        ++row;
    }
}

static void DrawNameTag(int x, int y, const std::string& name, uint32_t color,
                        Text::Align align) {
    TextStyle style;
    style.textColor = Colors::white;
    style.textFlags = Text::SINGLE_LINE;
    Text::arrange(align, style, name.c_str());
    vec2i size = Text::getSize();
    int pad = gSystem->applyScaleFactor(3);
    int w = size.x + pad * 2, h = size.y + pad;
    int bx = x;
    if (align == Text::MR) bx = x - w;
    recti box = {bx, y - h / 2, w, h};
    Draw::fill(box, Color32a(color, 110));
    Text::draw(recti{box.x + pad, box.y, size.x, box.h});
}

// Noteskin units are scaled by the zoom level and the UI scale.
static double PixelsPerUnit() {
    return gSystem->applyScaleFactor(gView->applyZoom(256)) / 256.0;
}

// ================================================================================================
// NetLink: owns the worker thread and the message queues.

struct NetEvent {
    enum Type {
        CONNECTED,  // A peer connected (host) / connect succeeded (guest).
        LOST,       // A peer connection ended.
        FAILED,     // Connecting failed (guest).
        FRAME,      // A complete frame was received; data = [type][payload].
    };
    Type type = FRAME;
    uint32_t peer = 0;
    Bytes data;
    std::string text;
};

class NetLink {
   public:
    ~NetLink() { stop(); }

    // Host: accepts any number of peers, with ids starting at 1.
    bool startHost(NetSocket listener, std::string& error) {
        stop();
        try {
            myThread = std::jthread(
                [this, l = std::move(listener)](std::stop_token st) mutable {
                    myRun(st, &l, nullptr);
                });
        } catch (const std::system_error& e) {
            error = e.what();
            return false;
        }
        return true;
    }

    // Guest: connects to the host, which gets peer id HOST_ID.
    bool startGuest(const std::string& host, int port, std::string& error) {
        stop();
        try {
            myThread = std::jthread([this, host, port](std::stop_token st) {
                myGuestLoop(st, host, port);
            });
        } catch (const std::system_error& e) {
            error = e.what();
            return false;
        }
        return true;
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
        myDrops.clear();
    }

    void send(uint32_t peer, Bytes frame) {
        std::lock_guard<std::mutex> lock(myMutex);
        myOutbox.push_back({peer, std::move(frame)});
    }

    // Closes a peer connection once its pending frames are sent.
    void dropPeer(uint32_t peer) {
        std::lock_guard<std::mutex> lock(myMutex);
        myDrops.push_back(peer);
    }

    bool poll(NetEvent& out) {
        std::lock_guard<std::mutex> lock(myMutex);
        if (myInbox.empty()) return false;
        out = std::move(myInbox.front());
        myInbox.pop_front();
        return true;
    }

   private:
    struct Outgoing {
        uint32_t peer;
        Bytes frame;
    };

    struct Conn {
        uint32_t id = 0;
        NetSocket socket;
        Bytes readBuf, writeBuf;
        size_t writePos = 0;
        bool dropping = false;
        int dropTicks = 0;

        bool hasPendingWrites() const { return writePos < writeBuf.size(); }
    };

    void myPush(NetEvent::Type type, uint32_t peer, std::string text) {
        NetEvent evt;
        evt.type = type;
        evt.peer = peer;
        evt.text = std::move(text);
        std::lock_guard<std::mutex> lock(myMutex);
        myInbox.push_back(std::move(evt));
    }

    void myGuestLoop(const std::stop_token& st, const std::string& host,
                     int port) {
        NetSocket sock;
        std::string error;
        if (!sock.beginConnect(host, port, error)) {
            myPush(NetEvent::FAILED, HOST_ID, error);
            return;
        }
        bool connected = false;
        for (int waited = 0; waited < CONNECT_TIMEOUT_MS && !connected;
             waited += 50) {
            if (st.stop_requested()) return;
            int flags = sock.poll(true, 50);
            if (flags & (NetSocket::POLL_WRITE | NetSocket::POLL_ERROR)) {
                if (!sock.finishConnect(error)) {
                    myPush(NetEvent::FAILED, HOST_ID, error);
                    return;
                }
                connected = true;
            }
        }
        if (!connected) {
            myPush(NetEvent::FAILED, HOST_ID,
                   std::format("Timed out connecting to {}:{}.", host, port));
            return;
        }
        myPush(NetEvent::CONNECTED, HOST_ID, std::string());
        myRun(st, nullptr, &sock);
    }

    static Conn* myFind(std::vector<Conn>& conns, uint32_t id) {
        for (auto& c : conns) {
            if (c.id == id) return &c;
        }
        return nullptr;
    }

    // Moves frames between the sockets and the queues. With a listener, new
    // connections are accepted until stopped; otherwise the loop ends when the
    // single connection ends.
    void myRun(const std::stop_token& st, NetSocket* listener,
               NetSocket* single) {
        std::vector<Conn> conns;
        if (single) {
            Conn c;
            c.id = HOST_ID;
            c.socket = std::move(*single);
            conns.push_back(std::move(c));
        }
        uint32_t nextId = HOST_ID + 1;
        Bytes chunk(1 << 16);
        int stopTicks = 0;

        while (true) {
            // Collect outgoing frames and drop requests.
            {
                std::lock_guard<std::mutex> lock(myMutex);
                for (auto& out : myOutbox) {
                    Conn* c = myFind(conns, out.peer);
                    if (c && !c->dropping) {
                        c->writeBuf.insert(c->writeBuf.end(), out.frame.begin(),
                                           out.frame.end());
                    }
                }
                myOutbox.clear();
                for (uint32_t id : myDrops) {
                    if (Conn* c = myFind(conns, id)) c->dropping = true;
                }
                myDrops.clear();
            }

            // On stop, close once everything is sent (or after a short grace
            // period).
            if (st.stop_requested()) {
                bool pending = std::any_of(
                    conns.begin(), conns.end(),
                    [](const Conn& c) { return c.hasPendingWrites(); });
                if (!pending || ++stopTicks > 10) return;
            }

            // Close dropped connections once their frames are sent.
            for (auto it = conns.begin(); it != conns.end();) {
                if (it->dropping &&
                    (!it->hasPendingWrites() || ++it->dropTicks > 10)) {
                    myPush(NetEvent::LOST, it->id, "Connection closed.");
                    it = conns.erase(it);
                } else {
                    ++it;
                }
            }
            if (!listener && conns.empty()) return;

            std::vector<NetSocket::PollItem> items;
            if (listener) items.push_back({listener, false, 0});
            for (auto& c : conns) {
                items.push_back({&c.socket, c.hasPendingWrites(), 0});
            }
            if (!NetSocket::pollAll(items, POLL_INTERVAL_MS)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(POLL_INTERVAL_MS));
                continue;
            }

            size_t first = listener ? 1 : 0;
            std::vector<std::pair<uint32_t, std::string>> lost;
            for (size_t i = 0; i < conns.size(); ++i) {
                std::string reason =
                    myService(conns[i], items[first + i].result, chunk);
                if (!reason.empty()) lost.emplace_back(conns[i].id, reason);
            }
            for (auto& [id, reason] : lost) {
                conns.erase(std::find_if(
                    conns.begin(), conns.end(),
                    [id = id](const Conn& c) { return c.id == id; }));
                myPush(NetEvent::LOST, id, reason);
            }

            if (listener && (items[0].result & NetSocket::POLL_READ)) {
                NetSocket sock = listener->accept();
                if (sock.isValid()) {
                    Conn c;
                    c.id = nextId++;
                    c.socket = std::move(sock);
                    conns.push_back(std::move(c));
                    myPush(NetEvent::CONNECTED, conns.back().id, std::string());
                }
            }
        }
    }

    // Reads and writes one connection. Returns the reason it ended, or an
    // empty string if it is still alive.
    std::string myService(Conn& c, int flags, Bytes& chunk) {
        if (flags & NetSocket::POLL_ERROR) return "Connection error.";

        if (flags & NetSocket::POLL_READ) {
            int n = c.socket.recv(chunk.data(), static_cast<int>(chunk.size()));
            if (n < 0) return "The other side closed the connection.";
            c.readBuf.insert(c.readBuf.end(), chunk.begin(), chunk.begin() + n);
            if (!myParseFrames(c.id, c.readBuf)) return "Protocol error.";
        }

        if (c.hasPendingWrites() && (flags & NetSocket::POLL_WRITE)) {
            int n =
                c.socket.send(c.writeBuf.data() + c.writePos,
                              static_cast<int>(c.writeBuf.size() - c.writePos));
            if (n < 0) return "Connection error.";
            c.writePos += static_cast<size_t>(n);
            if (c.writePos == c.writeBuf.size()) {
                c.writeBuf.clear();
                c.writePos = 0;
            }
        }
        return std::string();
    }

    // Extracts complete frames from the buffer. Returns false on bad data.
    bool myParseFrames(uint32_t peer, Bytes& buf) {
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
            evt.peer = peer;
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
    std::deque<Outgoing> myOutbox;
    std::vector<uint32_t> myDrops;
};

// ================================================================================================
// SessionImpl.

typedef std::chrono::steady_clock Clock;

struct Presence {
    uint32_t chart = 0;
    bool hasHover = false;
    double hoverTime = 0.0;
    double hoverX = 0.0;
    double viewTime = 0.0;

    bool operator==(const Presence& o) const {
        return chart == o.chart && hasHover == o.hasHover &&
               hoverTime == o.hoverTime && hoverX == o.hoverX &&
               viewTime == o.viewTime;
    }
};

struct PeerInfo {
    uint32_t id = 0;
    std::string name;
    bool joined = false;  // Host: passed the song check.
    Presence presence;
};

// Sync state of a chart that exists for everyone in the session.
struct ChartSync {
    uint32_t knownChecksum = 0;  // Checksum after the last sent/applied edit.
    int pendingEdits = 0;        // Guest: own edits not yet echoed by host.
    bool resyncRequested = false;
};

// A chart received in a full state, before it is applied.
struct ChartData {
    uint32_t id = 0;
    const Style* style = nullptr;
    ChartProps props;
    bool hasTempo = false;
    std::unique_ptr<Tempo> tempo;
    NoteList notes;
};

struct SessionImpl : public Session {
    NetLink myLink;
    bool myNetReady = false;

    Role myRole = ROLE_NONE;
    State myState = STATE_IDLE;

    std::string myName;
    uint32_t myId = HOST_ID;

    std::string myAddress;
    std::string myShareAddress;
    std::string myOtherAddresses;

    const Simfile* mySimfile = nullptr;
    SongInfo mySong;

    // Guest: true once the host's full state has been applied.
    bool myHasState = false;

    std::vector<PeerInfo> myPeerInfo;
    std::vector<Peer> myPeers;

    // Charts that exist for everyone in the session, by session id.
    std::unordered_map<const Chart*, uint32_t> myChartIds;
    std::unordered_map<uint32_t, ChartSync> myChartSync;
    uint32_t myNextChartId = 1;

    // Hashes of the last sent/applied version of each section.
    std::unordered_map<uint64_t, uint64_t> mySectionHashes;
    // Guest: sections sent to the host that were not echoed back yet.
    std::unordered_map<uint64_t, int> myPendingSections;
    bool mySectionsDirty = false;

    Presence myPresence;
    Clock::time_point myPresenceTime;
    bool myForcePresence = false;

    // ================================================================================================
    // SessionImpl :: constructor and destructor.

    ~SessionImpl() {
        disconnect();
        if (myNetReady) NetSocket::shutdown();
    }

    explicit SessionImpl(XmrNode& settings) {
        XmrNode* session = settings.child("session");
        if (session) setUserName(session->get("name", ""));
    }

    void saveSettings(XmrNode& settings) override {
        XmrNode* session = settings.addChild("session");
        session->addAttrib("name", myName.c_str());
    }

    static std::string SanitizeName(const std::string& name) {
        std::string clean;
        for (char c : name) {
            if (static_cast<unsigned char>(c) >= 32) clean.push_back(c);
        }
        while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        if (clean.size() > MAX_NAME_LENGTH) {
            // Cut before the start of the character that does not fit.
            size_t end = MAX_NAME_LENGTH;
            while (end > 0 &&
                   (static_cast<uint8_t>(clean[end]) & 0xC0) == 0x80) {
                --end;
            }
            clean.resize(end);
        }
        return clean;
    }

    void setUserName(const std::string& name) override {
        myName = SanitizeName(name);
    }

    const std::string& getUserName() const override { return myName; }

    // Starts the socket library on first use.
    bool myStartNetworking() {
        if (!myNetReady) myNetReady = NetSocket::startup();
        if (!myNetReady) HudError("Networking is not available.");
        return myNetReady;
    }

    // ================================================================================================
    // SessionImpl :: sending.

    static Bytes MakeFrame(MessageType type, const WriteStream& payload) {
        auto size = static_cast<uint32_t>(payload.size() + 1);
        Bytes frame(4 + size);
        frame[0] = static_cast<uint8_t>(size);
        frame[1] = static_cast<uint8_t>(size >> 8);
        frame[2] = static_cast<uint8_t>(size >> 16);
        frame[3] = static_cast<uint8_t>(size >> 24);
        frame[4] = type;
        if (payload.size() > 0) {
            memcpy(frame.data() + 5, payload.data(), payload.size());
        }
        return frame;
    }

    void mySendTo(uint32_t peer, MessageType type, const WriteStream& payload) {
        myLink.send(peer, MakeFrame(type, payload));
    }

    // Host: sends to every guest that joined, except one.
    void myBroadcast(MessageType type, const WriteStream& payload,
                     uint32_t except = UINT32_MAX) {
        Bytes frame = MakeFrame(type, payload);
        for (auto& peer : myPeerInfo) {
            if (peer.joined && peer.id != except) myLink.send(peer.id, frame);
        }
    }

    // Sends a change made by the local user: guests send it to the host, the
    // host sends it to all guests.
    void mySendChange(MessageType type, const WriteStream& payload) {
        if (myRole == ROLE_HOST) {
            myBroadcast(type, payload);
        } else {
            mySendTo(HOST_ID, type, payload);
        }
    }

    void mySendDisconnect(uint32_t peer, const std::string& reason) {
        WriteStream out;
        out.writeStr(reason);
        mySendTo(peer, MSG_DISCONNECT, out);
    }

    // ================================================================================================
    // SessionImpl :: public API.

    bool host(int port) override {
        disconnect();
        if (!myStartNetworking()) return false;

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

        if (!myLink.startHost(std::move(listener), error)) {
            HudError("Cannot host: %s", error.c_str());
            return false;
        }

        myRole = ROLE_HOST;
        myState = STATE_LISTENING;
        myId = HOST_ID;
        mySimfile = gSimfile->get();
        myHasState = true;
        myResetSyncState();
        myAssignChartIds();
        myStoreSectionHashes();
        HudInfo("Hosting session at %s", myShareAddress.c_str());
        return true;
    }

    bool join(const std::string& address) override {
        disconnect();
        if (!myStartNetworking()) return false;

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
                "Save your changes before joining; the host's simfile will "
                "replace yours.");
            return false;
        }
        std::string error;
        if (!GetLocalSongInfo(mySong, error)) {
            HudError("Cannot join: %s", error.c_str());
            return false;
        }
        if (!myLink.startGuest(hostName, port, error)) {
            HudError("Cannot join: %s", error.c_str());
            return false;
        }

        myAddress = hostName + ":" + std::to_string(port);
        myRole = ROLE_GUEST;
        myState = STATE_CONNECTING;
        mySimfile = gSimfile->get();
        myHasState = false;
        myResetSyncState();
        HudInfo("Connecting to %s...", myAddress.c_str());
        return true;
    }

    void disconnect() override {
        if (myRole == ROLE_NONE) return;
        if (myRole == ROLE_HOST) {
            for (auto& peer : myPeerInfo) {
                mySendDisconnect(peer.id, "The host ended the session.");
            }
        } else if (myState == STATE_HANDSHAKE || myState == STATE_CONNECTED) {
            mySendDisconnect(HOST_ID, "Left the session.");
        }
        myEndSession();
        HudInfo("Session ended.");
    }

    void myResetSyncState() {
        myPeerInfo.clear();
        myPeers.clear();
        myChartIds.clear();
        myChartSync.clear();
        myNextChartId = 1;
        mySectionHashes.clear();
        myPendingSections.clear();
        mySectionsDirty = false;
        myPresence = Presence();
        myForcePresence = true;
    }

    void myEndSession() {
        myLink.stop();
        myRole = ROLE_NONE;
        myState = STATE_IDLE;
        mySimfile = nullptr;
        myHasState = false;
        myResetSyncState();
    }

    Role getRole() const override { return myRole; }

    State getState() const override { return myState; }

    std::string myUserList() const {
        std::string names;
        for (auto& peer : myPeers) {
            if (!names.empty()) names += ", ";
            names += peer.name;
        }
        return names;
    }

    std::string getStatus() const override {
        switch (myState) {
            case STATE_IDLE:
                return "Not connected.";
            case STATE_LISTENING: {
                if (myPeers.empty()) {
                    return "Hosting, waiting for guests." +
                           (myOtherAddresses.empty()
                                ? std::string()
                                : " Other addresses:" + myOtherAddresses);
                }
                return std::format("Hosting, {} guest{}: {}", myPeers.size(),
                                   myPeers.size() == 1 ? "" : "s",
                                   myUserList());
            }
            case STATE_CONNECTING:
                return "Connecting to " + myAddress + "...";
            case STATE_HANDSHAKE:
                return "Verifying song...";
            case STATE_CONNECTED:
                if (!myHasState) return "Receiving the host's simfile...";
                return "Connected to " + myAddress + ". Users: " + myUserList();
        }
        return std::string();
    }

    std::string getShareAddress() const override {
        return myRole == ROLE_HOST ? myShareAddress : std::string();
    }

    const std::vector<Peer>& getPeers() const override { return myPeers; }

    // Returns true if changes can be exchanged with the other users.
    bool myIsSyncing() const {
        if (myRole == ROLE_HOST) return true;
        return myRole == ROLE_GUEST && myState == STATE_CONNECTED && myHasState;
    }

    // ================================================================================================
    // SessionImpl :: chart ids.

    Simfile* mySim() { return gSimfile->getForRemoteEdit(); }

    uint32_t myChartId(const Chart* chart) const {
        if (!chart) return 0;
        auto it = myChartIds.find(chart);
        return it == myChartIds.end() ? 0 : it->second;
    }

    // Returns the chart with the given id, if it is still in the simfile.
    Chart* myFindChart(uint32_t id) {
        Simfile* sim = mySim();
        if (!sim || id == 0) return nullptr;
        for (Chart* chart : sim->charts) {
            if (myChartId(chart) == id) return chart;
        }
        return nullptr;
    }

    void mySetChartId(const Chart* chart, uint32_t id) {
        myChartIds[chart] = id;
        ChartSync& sync = myChartSync[id];
        sync = ChartSync();
        sync.knownChecksum = Checksum(chart->notes);
    }

    // Host: gives every chart in the simfile an id. Charts added later get an
    // id when the next guest joins, and only exist for guests that joined
    // after that.
    void myAssignChartIds() {
        Simfile* sim = mySim();
        if (!sim) return;
        for (const Chart* chart : sim->charts) {
            if (myChartId(chart) == 0) mySetChartId(chart, myNextChartId++);
        }
    }

    // ================================================================================================
    // SessionImpl :: sections.

    // Returns the local version of a section, or false if it does not exist.
    bool myWriteSection(SectionType type, uint32_t id, Bytes& out) {
        Simfile* sim = mySim();
        if (!sim) return false;
        switch (type) {
            case SECTION_SONG_META:
                out = WriteSongMeta(sim);
                return true;
            case SECTION_TEMPO: {
                if (id == 0) {
                    out = WriteTempo(sim->tempo);
                    return true;
                }
                Chart* chart = myFindChart(id);
                if (!chart) return false;
                out = WriteChartTempo(chart);
                return true;
            }
            case SECTION_CHART_PROPS: {
                Chart* chart = myFindChart(id);
                if (!chart) return false;
                out = WriteChartProps(chart);
                return true;
            }
        }
        return false;
    }

    // Applies a section received from another user. Returns false if the
    // data is invalid or refers to an unknown chart.
    bool myApplySection(SectionType type, uint32_t id, const Bytes& data) {
        Simfile* sim = mySim();
        if (!sim) return false;
        switch (type) {
            case SECTION_SONG_META:
                return ApplySongMeta(sim, data);
            case SECTION_TEMPO: {
                Tempo tempo;
                bool hasTempo = false;
                if (!ReadTempo(data, hasTempo, tempo)) return false;
                if (id == 0) {
                    if (!hasTempo) return false;
                    sim->tempo->copy(&tempo);
                    gTempo->onRemoteTempoChanged(sim->tempo);
                    return true;
                }
                Chart* chart = myFindChart(id);
                if (!chart) return false;
                SetChartTempo(chart, hasTempo, tempo);
                gTempo->onRemoteTempoChanged(chart->tempo);
                return true;
            }
            case SECTION_CHART_PROPS: {
                ChartProps props;
                Chart* chart = myFindChart(id);
                if (!chart || !ReadChartProps(data, props)) return false;
                SetChartProps(chart, props);
                gSimfile->onRemoteChartsChanged(nullptr, false);
                return true;
            }
        }
        return false;
    }

    template <typename F>
    void myForEachSection(F&& func) {
        func(SECTION_SONG_META, 0);
        func(SECTION_TEMPO, 0);
        Simfile* sim = mySim();
        if (!sim) return;
        for (const Chart* chart : sim->charts) {
            uint32_t id = myChartId(chart);
            if (id == 0) continue;
            func(SECTION_TEMPO, id);
            func(SECTION_CHART_PROPS, id);
        }
    }

    // Remembers the current local version of every section as synced.
    void myStoreSectionHashes() {
        myForEachSection([this](SectionType type, uint32_t id) {
            Bytes data;
            if (myWriteSection(type, id, data)) {
                mySectionHashes[SectionKey(type, id)] = HashBytes(data);
            }
        });
    }

    static void WriteStateMessage(WriteStream& out, uint32_t origin,
                                  SectionType type, uint32_t id,
                                  const Bytes& data) {
        out.write<uint32_t>(origin);
        out.write<uint8_t>(type);
        out.write<uint32_t>(id);
        WriteBytes(out, data);
    }

    // Sends the sections that were changed locally.
    void myFlushSections() {
        if (!mySectionsDirty || !myIsSyncing()) return;
        mySectionsDirty = false;
        myForEachSection([this](SectionType type, uint32_t id) {
            Bytes data;
            if (!myWriteSection(type, id, data)) return;
            uint64_t key = SectionKey(type, id);
            uint64_t hash = HashBytes(data);
            auto it = mySectionHashes.find(key);
            if (it != mySectionHashes.end() && it->second == hash) return;
            mySectionHashes[key] = hash;

            WriteStream out;
            WriteStateMessage(out, myId, type, id, data);
            mySendChange(MSG_STATE, out);
            if (myRole == ROLE_GUEST) ++myPendingSections[key];
        });
    }

    // Applies a section and marks it as synced.
    void myApplyAndStoreSection(SectionType type, uint32_t id,
                                const Bytes& data) {
        if (!myApplySection(type, id, data)) return;
        gHistory->markExternalChange();
        Bytes local;
        if (myWriteSection(type, id, local)) {
            mySectionHashes[SectionKey(type, id)] = HashBytes(local);
        }
    }

    void myHandleState(uint32_t sender, ReadStream& in) {
        uint32_t origin = in.read<uint32_t>();
        auto type = static_cast<SectionType>(in.read<uint8_t>());
        uint32_t id = in.read<uint32_t>();
        Bytes data = ReadBytes(in);
        if (!in.success()) return;
        uint64_t key = SectionKey(type, id);

        if (myRole == ROLE_HOST) {
            // The last version received by the host wins. The host's version
            // is sent back to everyone, including the sender, so that it
            // always gets an echo.
            auto it = mySectionHashes.find(key);
            if (it == mySectionHashes.end() || it->second != HashBytes(data)) {
                myApplyAndStoreSection(type, id, data);
            }
            Bytes current;
            if (!myWriteSection(type, id, current)) current = data;
            WriteStream out;
            WriteStateMessage(out, sender, type, id, current);
            myBroadcast(MSG_STATE, out);
            return;
        }

        int& pending = myPendingSections[key];
        if (origin == myId) {
            // The host's echo of our own change.
            if (pending > 0) --pending;
            if (pending > 0) return;
        } else if (pending > 0) {
            // Our own pending change will overwrite this on the host.
            return;
        }
        auto it = mySectionHashes.find(key);
        if (it != mySectionHashes.end() && it->second == HashBytes(data)) {
            return;
        }
        myApplyAndStoreSection(type, id, data);
    }

    // ================================================================================================
    // SessionImpl :: notes.

    void onNotesApplied(const Chart* chart, const NoteList& add,
                        const NoteList& rem) override {
        if (!myIsSyncing() || (add.empty() && rem.empty())) return;
        uint32_t id = myChartId(chart);
        if (id == 0) return;

        ChartSync& sync = myChartSync[id];
        sync.knownChecksum = Checksum(chart->notes);

        WriteStream out;
        out.write<uint32_t>(myId);
        out.write<uint32_t>(id);
        add.encode(out, false);
        rem.encode(out, false);
        out.write<uint32_t>(sync.knownChecksum);
        mySendChange(MSG_NOTE_EDIT, out);
        if (myRole == ROLE_GUEST) ++sync.pendingEdits;
    }

    void mySendReplace(uint32_t origin, uint32_t id, const Chart* chart,
                       uint32_t to) {
        WriteStream out;
        out.write<uint32_t>(origin);
        out.write<uint32_t>(id);
        chart->notes.encode(out, false);
        out.write<uint32_t>(Checksum(chart->notes));
        if (to == UINT32_MAX) {
            mySendChange(MSG_NOTE_REPLACE, out);
        } else {
            mySendTo(to, MSG_NOTE_REPLACE, out);
        }
    }

    // Sends the full notes of charts that were changed without going through
    // onNotesApplied (e.g. inserting rows).
    void myCheckNoteChanges() {
        Simfile* sim = mySim();
        if (!sim || !myIsSyncing()) return;
        for (const Chart* chart : sim->charts) {
            uint32_t id = myChartId(chart);
            if (id == 0) continue;
            ChartSync& sync = myChartSync[id];
            uint32_t checksum = Checksum(chart->notes);
            if (checksum == sync.knownChecksum) continue;
            sync.knownChecksum = checksum;
            mySendReplace(myId, id, chart, UINT32_MAX);
            if (myRole == ROLE_GUEST) ++sync.pendingEdits;
        }
    }

    void myHandleNotes(MessageType type, uint32_t sender, ReadStream& in) {
        uint32_t origin = in.read<uint32_t>();
        uint32_t id = in.read<uint32_t>();
        NoteList add, rem;
        add.decode(in, 0);
        if (type == MSG_NOTE_EDIT) rem.decode(in, 0);
        uint32_t checksum = in.read<uint32_t>();
        if (!in.success()) return;
        if (myRole == ROLE_HOST) origin = sender;

        Chart* chart = myFindChart(id);
        ChartSync& sync = myChartSync[id];
        bool own = myRole == ROLE_GUEST && origin == myId;
        if (own) {
            if (sync.pendingEdits > 0) --sync.pendingEdits;
        } else if (chart) {
            // Changes from others are applied without history entries, so
            // undo/redo never reverts them.
            if (type == MSG_NOTE_EDIT) {
                gNotes->applyRemoteEdit(chart, add, rem);
            } else {
                gNotes->replaceNotes(chart, add);
                sync.resyncRequested = false;
            }
            gHistory->markExternalChange();
            sync.knownChecksum = Checksum(chart->notes);
        }

        if (myRole == ROLE_HOST) {
            if (!chart) return;
            // Relay the change to everyone, as acknowledgement for the sender.
            WriteStream out;
            out.write<uint32_t>(origin);
            out.write<uint32_t>(id);
            add.encode(out, false);
            if (type == MSG_NOTE_EDIT) rem.encode(out, false);
            out.write<uint32_t>(sync.knownChecksum);
            myBroadcast(type, out);
            return;
        }

        // Concurrent edits can end up applied in a different order than on
        // the host; ask for the host's notes when they differ.
        if (chart && sync.pendingEdits == 0 && !sync.resyncRequested &&
            Checksum(chart->notes) != checksum) {
            sync.resyncRequested = true;
            WriteStream out;
            out.write<uint32_t>(id);
            mySendTo(HOST_ID, MSG_RESYNC_REQUEST, out);
        }
    }

    // ================================================================================================
    // SessionImpl :: change notifications.

    void onChanges(int changes) override {
        if (myRole == ROLE_NONE) return;

        // The session is bound to the simfile that was open when it started.
        if ((changes & VCM_FILE_CHANGED) || gSimfile->get() != mySimfile) {
            if (myRole == ROLE_HOST) {
                for (auto& peer : myPeerInfo) {
                    mySendDisconnect(peer.id, "The host closed the song.");
                }
            } else {
                mySendDisconnect(HOST_ID, "Closed the song.");
            }
            myEndSession();
            HudInfo("Session ended because the song was closed.");
            return;
        }

        if (!myIsSyncing()) return;

        if (changes & (VCM_TEMPO_CHANGED | VCM_SONG_PROPERTIES_CHANGED |
                       VCM_CHART_PROPERTIES_CHANGED | VCM_CHART_LIST_CHANGED |
                       VCM_BANNER_PATH_CHANGED | VCM_BACKGROUND_PATH_CHANGED)) {
            mySectionsDirty = true;
        }
        if (changes & (VCM_NOTES_CHANGED | VCM_CHART_LIST_CHANGED)) {
            myCheckNoteChanges();
        }
        if (changes & VCM_CHART_CHANGED) myForcePresence = true;
    }

    // ================================================================================================
    // SessionImpl :: presence.

    Presence myLocalPresence() {
        Presence p;
        if (!gSimfile->isOpen()) return p;
        p.chart = myChartId(gChart->get());
        p.viewTime = gView->getCursorTime();

        // The last mouse position is kept when the mouse leaves the window.
        vec2i mouse = gSystem->getMousePos();
        recti view = gView->getRect();
        if (SDL_GetMouseFocus() && IsInside(view, mouse.x, mouse.y) &&
            !gTextOverlay->isOpen()) {
            p.hasHover = true;
            p.hoverTime = gView->offsetToTime(gView->yToOffset(mouse.y));
            p.hoverX = (mouse.x - gView->getReceptorCoords().xc) /
                       std::max(PixelsPerUnit(), 0.001);
        }
        return p;
    }

    static void WritePresence(WriteStream& out, uint32_t origin,
                              const Presence& p) {
        out.write<uint32_t>(origin);
        out.write<uint32_t>(p.chart);
        out.write<uint8_t>(p.hasHover ? 1 : 0);
        out.write<double>(p.hoverTime);
        out.write<double>(p.hoverX);
        out.write<double>(p.viewTime);
    }

    void mySendPresence() {
        if (!myIsSyncing()) return;
        Presence p = myLocalPresence();
        auto now = Clock::now();
        if (!myForcePresence) {
            if (p == myPresence) return;
            if (now - myPresenceTime <
                std::chrono::milliseconds(PRESENCE_INTERVAL_MS)) {
                return;
            }
        }
        myForcePresence = false;
        myPresence = p;
        myPresenceTime = now;
        WriteStream out;
        WritePresence(out, myId, p);
        mySendChange(MSG_PRESENCE, out);
    }

    void myHandlePresence(uint32_t sender, ReadStream& in) {
        uint32_t origin = in.read<uint32_t>();
        Presence p;
        in.read(p.chart);
        p.hasHover = in.read<uint8_t>() != 0;
        in.read(p.hoverTime);
        in.read(p.hoverX);
        in.read(p.viewTime);
        if (!in.success()) return;
        if (myRole == ROLE_HOST) origin = sender;

        PeerInfo* peer = myFindPeer(origin);
        if (!peer) return;
        peer->presence = p;

        if (myRole == ROLE_HOST) {
            WriteStream out;
            WritePresence(out, origin, p);
            myBroadcast(MSG_PRESENCE, out, origin);
        }
    }

    // Rebuilds the list returned by getPeers().
    void myUpdatePeers() {
        myPeers.clear();
        uint32_t chart = myChartId(gChart->get());
        for (auto& info : myPeerInfo) {
            if (myRole == ROLE_HOST && !info.joined) continue;
            Peer peer;
            peer.id = info.id;
            peer.name = info.name;
            peer.color = PeerColor(info.id);
            peer.onChart = chart != 0 && info.presence.chart == chart;
            peer.hasHover = info.presence.hasHover;
            peer.hoverTime = info.presence.hoverTime;
            peer.hoverX = info.presence.hoverX;
            peer.viewTime = info.presence.viewTime;
            myPeers.push_back(peer);
        }
    }

    PeerInfo* myFindPeer(uint32_t id) {
        for (auto& peer : myPeerInfo) {
            if (peer.id == id) return &peer;
        }
        return nullptr;
    }

    // ================================================================================================
    // SessionImpl :: receiving.

    void tick() override {
        if (myRole == ROLE_NONE) return;

        // Send local changes before applying remote ones, so that pending
        // changes are known when the host's echoes arrive.
        myFlushSections();

        NetEvent evt;
        while (myRole != ROLE_NONE && myLink.poll(evt)) {
            if (myRole == ROLE_HOST) {
                myHandleHostEvent(evt);
            } else {
                myHandleGuestEvent(evt);
            }
        }
        if (myRole == ROLE_NONE) return;

        mySendPresence();
        myUpdatePeers();
    }

    // ================================================================================================
    // SessionImpl :: host.

    std::string myUniqueName(std::string name, uint32_t id) const {
        if (name.empty()) name = "Guest " + std::to_string(id);
        std::string base = name;
        for (int n = 2;; ++n) {
            bool taken = (name == myHostName());
            for (auto& peer : myPeerInfo) {
                taken |= peer.joined && peer.id != id && peer.name == name;
            }
            if (!taken) return name;
            name = base + " (" + std::to_string(n) + ")";
        }
    }

    std::string myHostName() const {
        return myName.empty() ? std::string("Host") : myName;
    }

    void myBroadcastPeerList() {
        WriteStream out;
        uint32_t count = 1;
        for (auto& peer : myPeerInfo) count += peer.joined ? 1 : 0;
        out.write<uint32_t>(count);
        out.write<uint32_t>(HOST_ID);
        out.writeStr(myHostName());
        for (auto& peer : myPeerInfo) {
            if (!peer.joined) continue;
            out.write<uint32_t>(peer.id);
            out.writeStr(peer.name);
        }
        myBroadcast(MSG_PEER_LIST, out);
    }

    void myRemovePeer(uint32_t id, const char* message) {
        auto it = std::find_if(myPeerInfo.begin(), myPeerInfo.end(),
                               [id](const PeerInfo& p) { return p.id == id; });
        if (it == myPeerInfo.end()) return;
        bool joined = it->joined;
        std::string name = it->name;
        myPeerInfo.erase(it);
        if (joined) {
            HudInfo("%s %s", name.c_str(), message);
            myBroadcastPeerList();
        }
    }

    void mySendFullState(uint32_t peer) {
        myAssignChartIds();
        Simfile* sim = mySim();
        WriteStream out;
        WriteBytes(out, WriteSongMeta(sim));
        WriteBytes(out, WriteTempo(sim->tempo));
        out.write<uint32_t>(static_cast<uint32_t>(sim->charts.size()));
        for (const Chart* chart : sim->charts) {
            out.write<uint32_t>(myChartId(chart));
            out.writeStr(chart->style ? chart->style->id : std::string());
            WriteBytes(out, WriteChartProps(chart));
            WriteBytes(out, WriteChartTempo(chart));
            chart->notes.encode(out, false);
        }
        mySendTo(peer, MSG_FULL_STATE, out);
    }

    void myHandleHello(PeerInfo& peer, ReadStream& in) {
        uint32_t magic = in.read<uint32_t>();
        uint16_t version = in.read<uint16_t>();
        std::string name;
        SongInfo guestSong;
        if (in.success() && magic == PROTOCOL_MAGIC &&
            version == PROTOCOL_VERSION) {
            in.readStr(name);
            in.read(guestSong.audioHash);
            in.read(guestSong.duration);
        }

        std::string reason;
        int numJoined = 0;
        for (auto& p : myPeerInfo) numJoined += p.joined ? 1 : 0;
        if (!in.success() || magic != PROTOCOL_MAGIC) {
            reason = "Not an ArrowVortex session.";
        } else if (version != PROTOCOL_VERSION) {
            reason = std::format(
                "Protocol version mismatch (host {}, guest {}). Use the same "
                "ArrowVortex version.",
                PROTOCOL_VERSION, version);
        } else if (numJoined >= MAX_GUESTS) {
            reason = "The session is full.";
        } else {
            reason = CompareSongs(mySong, guestSong);
        }

        WriteStream out;
        out.write<uint8_t>(reason.empty() ? 1 : 0);
        out.writeStr(reason);
        out.write<uint32_t>(peer.id);
        mySendTo(peer.id, MSG_WELCOME, out);

        if (!reason.empty()) {
            uint32_t id = peer.id;
            myLink.dropPeer(id);
            myRemovePeer(id, "was rejected.");
            HudWarning("Rejected a guest: %s", reason.c_str());
            return;
        }

        peer.name = myUniqueName(SanitizeName(name), peer.id);
        peer.joined = true;

        mySendFullState(peer.id);
        myBroadcastPeerList();

        // Let the new guest know where everyone is.
        {
            WriteStream presence;
            WritePresence(presence, HOST_ID, myLocalPresence());
            mySendTo(peer.id, MSG_PRESENCE, presence);
        }
        for (auto& other : myPeerInfo) {
            if (!other.joined || other.id == peer.id) continue;
            WriteStream presence;
            WritePresence(presence, other.id, other.presence);
            mySendTo(peer.id, MSG_PRESENCE, presence);
        }
        HudInfo("%s joined the session.", peer.name.c_str());
    }

    void myHandleHostEvent(NetEvent& evt) {
        switch (evt.type) {
            case NetEvent::CONNECTED: {
                PeerInfo peer;
                peer.id = evt.peer;
                myPeerInfo.push_back(peer);
                return;
            }
            case NetEvent::LOST:
                myRemovePeer(evt.peer, "left the session.");
                return;
            case NetEvent::FAILED:
                return;
            case NetEvent::FRAME:
                break;
        };

        PeerInfo* peer = myFindPeer(evt.peer);
        if (!peer || evt.data.empty()) return;

        auto type = static_cast<MessageType>(evt.data[0]);
        ReadStream in(evt.data.data() + 1,
                      static_cast<int>(evt.data.size() - 1));

        if (!peer->joined) {
            if (type == MSG_HELLO) myHandleHello(*peer, in);
            return;
        }

        switch (type) {
            case MSG_NOTE_EDIT:
            case MSG_NOTE_REPLACE:
                myHandleNotes(type, evt.peer, in);
                return;
            case MSG_STATE:
                myHandleState(evt.peer, in);
                return;
            case MSG_PRESENCE:
                myHandlePresence(evt.peer, in);
                return;
            case MSG_RESYNC_REQUEST: {
                uint32_t id = in.read<uint32_t>();
                const Chart* chart = myFindChart(id);
                if (in.success() && chart) {
                    mySendReplace(HOST_ID, id, chart, evt.peer);
                }
                return;
            }
            case MSG_DISCONNECT: {
                uint32_t id = evt.peer;
                myLink.dropPeer(id);
                myRemovePeer(id, "left the session.");
                return;
            }
            default:
                return;
        }
    }

    // ================================================================================================
    // SessionImpl :: guest.

    void myHandleGuestEvent(NetEvent& evt) {
        switch (evt.type) {
            case NetEvent::CONNECTED: {
                myState = STATE_HANDSHAKE;
                WriteStream out;
                out.write<uint32_t>(PROTOCOL_MAGIC);
                out.write<uint16_t>(PROTOCOL_VERSION);
                out.writeStr(myName);
                out.write<uint64_t>(mySong.audioHash);
                out.write<double>(mySong.duration);
                mySendTo(HOST_ID, MSG_HELLO, out);
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
        if (evt.data.empty()) return;

        auto type = static_cast<MessageType>(evt.data[0]);
        ReadStream in(evt.data.data() + 1,
                      static_cast<int>(evt.data.size() - 1));

        switch (type) {
            case MSG_WELCOME: {
                uint8_t ok = in.read<uint8_t>();
                std::string reason = in.readStr();
                uint32_t id = in.read<uint32_t>();
                if (in.success() && ok) {
                    myId = id;
                    myState = STATE_CONNECTED;
                } else {
                    HudError("Join rejected: %s", reason.c_str());
                    myEndSession();
                }
                return;
            }
            case MSG_FULL_STATE:
                myApplyFullState(in);
                return;
            case MSG_PEER_LIST:
                myHandlePeerList(in);
                return;
            case MSG_DISCONNECT: {
                std::string reason = in.readStr();
                HudInfo("Session ended: %s", reason.c_str());
                myEndSession();
                return;
            }
            default:
                break;
        }

        if (!myHasState) return;
        switch (type) {
            case MSG_NOTE_EDIT:
            case MSG_NOTE_REPLACE:
                myHandleNotes(type, HOST_ID, in);
                return;
            case MSG_STATE:
                myHandleState(HOST_ID, in);
                return;
            case MSG_PRESENCE:
                myHandlePresence(HOST_ID, in);
                return;
            default:
                return;
        }
    }

    void myHandlePeerList(ReadStream& in) {
        uint32_t count = in.read<uint32_t>();
        std::vector<PeerInfo> list;
        for (uint32_t i = 0; i < count && in.success(); ++i) {
            PeerInfo info;
            in.read(info.id);
            in.readStr(info.name);
            info.joined = true;
            if (info.id == myId) continue;
            if (PeerInfo* old = myFindPeer(info.id)) {
                info.presence = old->presence;
            }
            list.push_back(info);
        }
        if (!in.success()) return;

        for (auto& info : list) {
            if (!myFindPeer(info.id) && myHasState) {
                HudInfo("%s joined the session.", info.name.c_str());
            }
        }
        for (auto& old : myPeerInfo) {
            bool kept =
                std::any_of(list.begin(), list.end(),
                            [&](const PeerInfo& p) { return p.id == old.id; });
            if (!kept) HudInfo("%s left the session.", old.name.c_str());
        }
        myPeerInfo = std::move(list);
    }

    // Replaces the local simfile data with the host's.
    void myApplyFullState(ReadStream& in) {
        Bytes meta = ReadBytes(in);
        Bytes songTempoData = ReadBytes(in);
        uint32_t count = in.read<uint32_t>();
        std::vector<ChartData> charts;
        std::string missingStyle;
        for (uint32_t i = 0; i < count && in.success(); ++i) {
            ChartData data;
            in.read(data.id);
            std::string styleId = in.readStr();
            Bytes props = ReadBytes(in);
            Bytes tempo = ReadBytes(in);
            data.notes.decode(in, 0);
            if (!in.success()) break;
            data.style = gStyle->findStyle(styleId);
            if (!data.style) missingStyle = styleId;
            data.tempo = std::make_unique<Tempo>();
            if (!ReadChartProps(props, data.props) ||
                !ReadTempo(tempo, data.hasTempo, *data.tempo)) {
                in.invalidate();
            }
            charts.push_back(std::move(data));
        }
        Tempo songTempo;
        bool hasSongTempo = false;
        if (!in.success() ||
            !ReadTempo(songTempoData, hasSongTempo, songTempo) ||
            !hasSongTempo) {
            HudError("Received invalid simfile data from the host.");
            disconnect();
            return;
        }
        if (!missingStyle.empty()) {
            HudError("The host's simfile uses a style you don't have (%s).",
                     missingStyle.c_str());
            disconnect();
            return;
        }

        uint64_t before = myHashEverything();

        // History entries describe changes to the old data.
        gHistory->clearAll();

        Simfile* sim = mySim();
        ApplySongMeta(sim, meta);
        sim->tempo->copy(&songTempo);

        // Reuse the existing chart objects where possible.
        std::vector<Chart*> old = sim->charts;
        std::vector<Chart*> list;
        const Chart* open = gChart->get();
        int openIndex = 0;
        for (size_t i = 0; i < old.size(); ++i) {
            if (old[i] == open) openIndex = static_cast<int>(i);
        }
        for (size_t i = 0; i < charts.size(); ++i) {
            Chart* chart = i < old.size() ? old[i] : new Chart;
            ChartData& data = charts[i];
            chart->style = data.style;
            SetChartProps(chart, data.props);
            SetChartTempo(chart, data.hasTempo, *data.tempo);
            chart->notes = data.notes;
            list.push_back(chart);
            mySetChartId(chart, data.id);
        }
        sim->charts = list;

        const Chart* reopen = nullptr;
        if (!list.empty()) {
            reopen =
                list[std::min(openIndex, static_cast<int>(list.size()) - 1)];
        }
        gSimfile->onRemoteChartsChanged(reopen, true);
        for (size_t i = charts.size(); i < old.size(); ++i) delete old[i];

        gTempo->onRemoteTempoChanged(nullptr);
        gEditor->reportChanges(
            VCM_SONG_PROPERTIES_CHANGED | VCM_BANNER_PATH_CHANGED |
            VCM_BACKGROUND_PATH_CHANGED | VCM_CHART_PROPERTIES_CHANGED);

        if (myHashEverything() != before) gHistory->markExternalChange();

        myStoreSectionHashes();
        myHasState = true;
        myForcePresence = true;
        HudInfo("Joined the session at %s.", myAddress.c_str());
    }

    // Returns a hash of all synced data, to detect if anything changed.
    uint64_t myHashEverything() {
        Simfile* sim = mySim();
        if (!sim) return 0;
        WriteStream out;
        WriteBytes(out, WriteSongMeta(sim));
        WriteBytes(out, WriteTempo(sim->tempo));
        for (const Chart* chart : sim->charts) {
            out.writeStr(chart->style ? chart->style->id : std::string());
            WriteBytes(out, WriteChartProps(chart));
            WriteBytes(out, WriteChartTempo(chart));
            out.write<uint32_t>(Checksum(chart->notes));
        }
        return HashBytes(out.data(), static_cast<size_t>(out.size()));
    }

    // ================================================================================================
    // SessionImpl :: drawing.

    void drawCursors() override {
        if (myPeers.empty() || gTextOverlay->isOpen()) return;
        recti view = gView->getRect();
        int xc = gView->getReceptorCoords().xc;
        double unit = PixelsPerUnit();
        Renderer::pushScissorRect(view);
        for (const Peer& peer : myPeers) {
            if (!peer.onChart || !peer.hasHover) continue;
            int x = xc + static_cast<int>(std::lround(peer.hoverX * unit));
            int y = gView->timeToY(peer.hoverTime);
            if (y < view.y - 32 || y > view.y + view.h + 32) continue;
            DrawPointer(x, y, peer.color);
            int s = std::max(1, gSystem->applyScaleFactor(1));
            DrawNameTag(x + 12 * s, y + 18 * s, peer.name, peer.color,
                        Text::ML);
        }
        Renderer::popScissorRect();
    }
};

};  // anonymous namespace

// ================================================================================================
// Session API.

Session* gSession = nullptr;

void Session::create(XmrNode& settings) {
    gSession = new SessionImpl(settings);
}

void Session::destroy() {
    delete static_cast<SessionImpl*>(gSession);
    gSession = nullptr;
}

};  // namespace Vortex

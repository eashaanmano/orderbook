#pragma once
// On-disk formats (little-endian, packed, fixed-size records).
//
//  .obmd   recorded market data: absolute L2 level updates + trades
//          (written by replay/recorder.py or orderbook_gen)
//  .obcmd  engine command stream: New/Cancel/Modify
//          (written by orderbook_convert, replayed by orderbook_replay / orderbook_pipeline)
//
// Keep the Python struct strings in replay/recorder.py in sync with these.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "orderbook/types.hpp"

namespace ob {

// ------------------------------------------------------------------ .obmd

struct MdFileHeader {             // python: '<8sdd16s' (40 bytes)
    char   magic[8];              // "OBMD0001"
    double tickSize;              // price = ticks * tickSize
    double lotSize;               // qty   = lots  * lotSize
    char   symbol[16];
};
static_assert(sizeof(MdFileHeader) == 40);

enum class MdKind : std::uint8_t {
    Level = 0,  // absolute qty now resting at (side, price); qty 0 = level gone
    Trade = 1,  // a trade printed at price for qty; side = AGGRESSOR side
};

struct MdRecord {                 // python: '<QqqBB6x' (32 bytes)
    std::uint64_t ts;             // exchange event time, ns since epoch
    Price         price;          // ticks
    Qty           qty;            // lots
    MdKind        kind;
    Side          side;
    std::uint8_t  pad[6];
};
static_assert(sizeof(MdRecord) == 32);

// ----------------------------------------------------------------- .obcmd

enum class CmdType : std::uint8_t { New = 0, Cancel = 1, Modify = 2 };

struct Command {                  // 40 bytes
    std::uint64_t ts;             // source event time, ns
    OrderId       id;
    Price         price;
    Qty           qty;
    CmdType       type;
    Side          side;
    OrderType     otype;
    std::uint8_t  pad[5];
};
static_assert(sizeof(Command) == 40);

struct CmdFileHeader {
    char          magic[8];       // "OBCMD001"
    std::uint64_t count;
    double        tickSize;
    double        lotSize;
    Price         minPrice;       // engine window, chosen by the converter
    std::uint32_t numLevels;
    std::uint32_t pad;
};
static_assert(sizeof(CmdFileHeader) == 48);

// ------------------------------------------------------------------ IO helpers

inline std::vector<char> readWholeFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> buf(static_cast<std::size_t>(n));
    if (n > 0 && std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        throw std::runtime_error("short read on " + path);
    }
    std::fclose(f);
    return buf;
}

struct MdFile {
    MdFileHeader          header{};
    std::vector<MdRecord> records;
};

inline MdFile loadMd(const std::string& path) {
    auto   buf = readWholeFile(path);
    MdFile f;
    if (buf.size() < sizeof(MdFileHeader)) throw std::runtime_error("truncated .obmd: " + path);
    std::memcpy(&f.header, buf.data(), sizeof(MdFileHeader));
    if (std::memcmp(f.header.magic, "OBMD0001", 8) != 0) throw std::runtime_error("bad .obmd magic: " + path);
    const std::size_t n = (buf.size() - sizeof(MdFileHeader)) / sizeof(MdRecord);
    f.records.resize(n);
    if (n) std::memcpy(f.records.data(), buf.data() + sizeof(MdFileHeader), n * sizeof(MdRecord));
    return f;
}

class MdWriter {
public:
    MdWriter(const std::string& path, double tick, double lot, const std::string& symbol) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) throw std::runtime_error("cannot write " + path);
        MdFileHeader h{};
        std::memcpy(h.magic, "OBMD0001", 8);
        h.tickSize = tick;
        h.lotSize  = lot;
        std::strncpy(h.symbol, symbol.c_str(), sizeof(h.symbol) - 1);
        std::fwrite(&h, sizeof h, 1, f_);
    }
    ~MdWriter() { if (f_) std::fclose(f_); }
    MdWriter(const MdWriter&) = delete;
    MdWriter& operator=(const MdWriter&) = delete;
    void write(const MdRecord& r) { std::fwrite(&r, sizeof r, 1, f_); }

private:
    std::FILE* f_ = nullptr;
};

struct CmdFile {
    CmdFileHeader        header{};
    std::vector<Command> cmds;
};

inline CmdFile loadCmds(const std::string& path) {
    auto    buf = readWholeFile(path);
    CmdFile f;
    if (buf.size() < sizeof(CmdFileHeader)) throw std::runtime_error("truncated .obcmd: " + path);
    std::memcpy(&f.header, buf.data(), sizeof(CmdFileHeader));
    if (std::memcmp(f.header.magic, "OBCMD001", 8) != 0) throw std::runtime_error("bad .obcmd magic: " + path);
    const std::size_t n = (buf.size() - sizeof(CmdFileHeader)) / sizeof(Command);
    f.cmds.resize(n);
    if (n) std::memcpy(f.cmds.data(), buf.data() + sizeof(CmdFileHeader), n * sizeof(Command));
    return f;
}

inline void saveCmds(const std::string& path, CmdFileHeader h, const std::vector<Command>& cmds) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::memcpy(h.magic, "OBCMD001", 8);
    h.count = cmds.size();
    std::fwrite(&h, sizeof h, 1, f);
    if (!cmds.empty()) std::fwrite(cmds.data(), sizeof(Command), cmds.size(), f);
    std::fclose(f);
}

}  // namespace ob

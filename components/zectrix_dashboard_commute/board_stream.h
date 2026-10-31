#ifndef ZECTRIX_BOARD_STREAM_H_
#define ZECTRIX_BOARD_STREAM_H_

// Streaming reader for Rail Data Marketplace GetDepBoardWithDetails JSON.
//
// A board with details lists every calling point and coach of every train
// (100+ KB for King's Cross), but the commute only needs a few fields per
// train. BoardStream is fed the response in chunks as it arrives and keeps
// just those, so the body is never stored or parsed into a tree. Plain
// C++ without ESP-IDF, so it can be tested on the host.

#include <cstddef>
#include <cstdint>
#include <vector>

// One leg from a departure board: times in minutes since 1970 (local).
struct BoardLeg {
    char from[4];
    char to[4];
    int64_t dep;
    int64_t dep_expected;
    int64_t arr;
    int64_t arr_expected;
};

// "2026-11-02T07:31:30" -> minutes since 1970 (fields taken as given).
bool ParseBoardMinutes(const char* text, int64_t* out);

class BoardStream {
public:
    // from: the board's station CRS; targets: space-separated CRS codes of
    // the stops wanted (the first one each train calls at is used).
    BoardStream(const char* from, const char* targets);

    void Feed(const char* data, size_t length);

    // True once the whole top-level JSON object has been read.
    bool complete() const { return complete_; }
    // Departures that call at a target, skipping cancelled and departed.
    const std::vector<BoardLeg>& legs() const { return legs_; }

private:
    static constexpr int kMaxDepth = 16;
    static constexpr int kTextSize = 40;

    struct Frame {
        bool object;
        char name[24];   // key this container has in its parent
        char key[24];    // current key inside this object
        bool expect_key;
    };

    // Fields of the train and calling point being read.
    struct Service {
        char std[kTextSize];
        char etd[kTextSize];
        char departure_type[16];
        bool departed;
        bool cancelled;
        bool found;
        char to[4];
        char sta[kTextSize];
        char eta[kTextSize];
        char ata[kTextSize];
    };
    struct Location {
        char crs[8];
        char sta[kTextSize];
        char eta[kTextSize];
        char ata[kTextSize];
        bool cancelled;
    };

    void Process(char c);
    void Push(bool object);
    void Pop();
    void OnText(bool is_string);
    void OnValue(const char* key, const char* value, bool is_string);
    bool InService() const;
    bool InLocation() const;
    void FinishLocation();
    void FinishService();

    char from_[4];
    char targets_[32];

    Frame frames_[kMaxDepth];
    int depth_ = 0;
    bool started_ = false;
    bool complete_ = false;

    enum class State { kIdle, kString, kLiteral } state_ = State::kIdle;
    char text_[kTextSize];
    int text_length_ = 0;
    bool escape_ = false;
    int unicode_left_ = 0;

    Service service_ = {};
    Location location_ = {};
    std::vector<BoardLeg> legs_;
};

#endif  // ZECTRIX_BOARD_STREAM_H_

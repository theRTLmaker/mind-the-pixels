#include "board_stream.h"

#include <cstdio>
#include <cstring>

namespace {

void Copy(char* out, size_t size, const char* text) {
    std::snprintf(out, size, "%s", text);
}

}  // namespace

bool ParseBoardMinutes(const char* text, int64_t* out) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (std::sscanf(text, "%d-%d-%dT%d:%d", &year, &month, &day, &hour,
                    &minute) != 5) {
        return false;
    }
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int year_of_era = year - era * 400;
    const int day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int day_of_era = year_of_era * 365 + year_of_era / 4 -
                           year_of_era / 100 + day_of_year;
    const int64_t days = era * 146097LL + day_of_era - 719468;
    *out = days * 1440 + hour * 60 + minute;
    return true;
}

BoardStream::BoardStream(const char* from, const char* targets) {
    Copy(from_, sizeof(from_), from);
    Copy(targets_, sizeof(targets_), targets);
}

void BoardStream::Feed(const char* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        Process(data[i]);
    }
}

void BoardStream::Process(char c) {
    if (state_ == State::kString) {
        if (unicode_left_ > 0) {
            // \uXXXX: none of the fields kept need it; keep a placeholder.
            if (--unicode_left_ == 0 && text_length_ < kTextSize - 1) {
                text_[text_length_++] = '?';
            }
            return;
        }
        if (escape_) {
            escape_ = false;
            if (c == 'u') {
                unicode_left_ = 4;
                return;
            }
        } else if (c == '\\') {
            escape_ = true;
            return;
        } else if (c == '"') {
            state_ = State::kIdle;
            text_[text_length_] = '\0';
            OnText(true);
            return;
        }
        if (text_length_ < kTextSize - 1) {
            text_[text_length_++] = c;
        }
        return;
    }

    if (state_ == State::kLiteral) {
        const bool literal_char = (c >= '0' && c <= '9') ||
                                  (c >= 'a' && c <= 'z') ||
                                  (c >= 'A' && c <= 'Z') || c == '.' ||
                                  c == '-' || c == '+';
        if (literal_char) {
            if (text_length_ < kTextSize - 1) {
                text_[text_length_++] = c;
            }
            return;
        }
        state_ = State::kIdle;
        text_[text_length_] = '\0';
        OnText(false);
        // Fall through: c ends the literal and still needs handling.
    }

    switch (c) {
        case ' ': case '\t': case '\r': case '\n':
            return;
        case '"':
            state_ = State::kString;
            text_length_ = 0;
            escape_ = false;
            unicode_left_ = 0;
            return;
        case '{':
            Push(true);
            return;
        case '[':
            Push(false);
            return;
        case '}':
        case ']':
            Pop();
            return;
        case ':':
            if (depth_ > 0 && depth_ <= kMaxDepth &&
                frames_[depth_ - 1].object) {
                frames_[depth_ - 1].expect_key = false;
            }
            return;
        case ',':
            if (depth_ > 0 && depth_ <= kMaxDepth &&
                frames_[depth_ - 1].object) {
                frames_[depth_ - 1].expect_key = true;
            }
            return;
        default:
            state_ = State::kLiteral;
            text_length_ = 0;
            text_[text_length_++] = c;
            return;
    }
}

void BoardStream::Push(bool object) {
    if (depth_ < kMaxDepth) {
        Frame& frame = frames_[depth_];
        frame.object = object;
        frame.key[0] = '\0';
        frame.expect_key = object;
        frame.name[0] = '\0';
        if (depth_ > 0 && frames_[depth_ - 1].object) {
            Copy(frame.name, sizeof(frame.name), frames_[depth_ - 1].key);
        }
    }
    ++depth_;
    started_ = true;
    if (InService() && object) {
        service_ = {};
    } else if (InLocation() && object) {
        location_ = {};
    }
}

void BoardStream::Pop() {
    if (depth_ == 0) {
        return;
    }
    if (InLocation() && frames_[depth_ - 1].object) {
        FinishLocation();
    } else if (InService() && frames_[depth_ - 1].object) {
        FinishService();
    }
    --depth_;
    if (depth_ == 0 && started_) {
        complete_ = true;
    }
}

// A train: root object > "trainServices" array > service object.
bool BoardStream::InService() const {
    return depth_ == 3 && std::strcmp(frames_[1].name, "trainServices") == 0;
}

// A calling point: ... service > "subsequentLocations" array > object.
bool BoardStream::InLocation() const {
    return depth_ == 5 && std::strcmp(frames_[1].name, "trainServices") == 0 &&
           std::strcmp(frames_[3].name, "subsequentLocations") == 0;
}

void BoardStream::OnText(bool is_string) {
    if (depth_ == 0 || depth_ > kMaxDepth) {
        return;
    }
    Frame& top = frames_[depth_ - 1];
    if (top.object && top.expect_key) {
        Copy(top.key, sizeof(top.key), text_);
        return;
    }
    OnValue(top.object ? top.key : "", text_, is_string);
}

void BoardStream::OnValue(const char* key, const char* value,
                          bool is_string) {
    if (InService()) {
        if (std::strcmp(key, "std") == 0 && is_string) {
            Copy(service_.std, sizeof(service_.std), value);
        } else if (std::strcmp(key, "etd") == 0 && is_string) {
            Copy(service_.etd, sizeof(service_.etd), value);
        } else if (std::strcmp(key, "atd") == 0 && is_string) {
            service_.departed = true;
        } else if (std::strcmp(key, "departureType") == 0 && is_string) {
            Copy(service_.departure_type, sizeof(service_.departure_type),
                 value);
        } else if (std::strcmp(key, "isCancelled") == 0) {
            service_.cancelled = std::strcmp(value, "true") == 0;
        }
    } else if (InLocation()) {
        if (std::strcmp(key, "crs") == 0 && is_string) {
            Copy(location_.crs, sizeof(location_.crs), value);
        } else if (std::strcmp(key, "sta") == 0 && is_string) {
            Copy(location_.sta, sizeof(location_.sta), value);
        } else if (std::strcmp(key, "eta") == 0 && is_string) {
            Copy(location_.eta, sizeof(location_.eta), value);
        } else if (std::strcmp(key, "ata") == 0 && is_string) {
            Copy(location_.ata, sizeof(location_.ata), value);
        } else if (std::strcmp(key, "isCancelled") == 0) {
            location_.cancelled = std::strcmp(value, "true") == 0;
        }
    }
}

void BoardStream::FinishLocation() {
    if (service_.found || location_.cancelled ||
        std::strlen(location_.crs) != 3 ||
        std::strstr(targets_, location_.crs) == nullptr) {
        return;
    }
    service_.found = true;
    Copy(service_.to, sizeof(service_.to), location_.crs);
    Copy(service_.sta, sizeof(service_.sta), location_.sta);
    Copy(service_.eta, sizeof(service_.eta), location_.eta);
    Copy(service_.ata, sizeof(service_.ata), location_.ata);
}

void BoardStream::FinishService() {
    if (!service_.found || service_.cancelled || service_.departed ||
        std::strcmp(service_.departure_type, "Actual") == 0) {
        return;
    }
    BoardLeg leg = {};
    Copy(leg.from, sizeof(leg.from), from_);
    Copy(leg.to, sizeof(leg.to), service_.to);
    if (!ParseBoardMinutes(service_.std, &leg.dep) ||
        !ParseBoardMinutes(service_.sta, &leg.arr)) {
        return;
    }
    if (!ParseBoardMinutes(service_.etd, &leg.dep_expected)) {
        leg.dep_expected = leg.dep;
    }
    if (!ParseBoardMinutes(service_.eta, &leg.arr_expected) &&
        !ParseBoardMinutes(service_.ata, &leg.arr_expected)) {
        leg.arr_expected = leg.arr + (leg.dep_expected - leg.dep);
    }
    legs_.push_back(leg);
}

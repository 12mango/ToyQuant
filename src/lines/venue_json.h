#pragma once
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A small JSON reader, shared by the relay and the order translator so the tree has one parser rather than two.
//
// The venue publishes nested JSON, and the fields this line needs sit inside arrays of arrays
// (`asks: [[price, size, orders, count], ...]`, and an order's state inside `data: [{...}]`). A recursive parser is
// less code than field scraping through that nesting, and it refuses a shape it does not understand instead of
// inventing one. The numbering is deliberately simple: this reads the venue's replies, it does not round-trip
// arbitrary documents, and a `\u` escape is skipped rather than half-decoded -- the fields read here are ASCII.
struct Json
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Type type{Type::Null};
    bool boolean{false};
    double number{0.0};
    std::string text;
    std::vector<Json> items;
    std::unordered_map<std::string, Json> fields;

    const Json* find(std::string_view key) const
    {
        if (type != Type::Object) return nullptr;
        const auto found = fields.find(std::string(key));
        return found == fields.end() ? nullptr : &found->second;
    }

    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }

    // The venues quote numbers as strings ("82915.7"), so both spellings are read as text.
    std::string as_text() const
    {
        if (type == Type::String) return text;
        if (type == Type::Number)
        {
            char buffer[32];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
            return std::string(buffer, result.ptr);
        }
        return {};
    }

    double as_double() const
    {
        if (type == Type::Number) return number;
        if (type == Type::String) return std::stod(text);
        return 0.0;
    }
};

class JsonParser
{
   public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(Json& out, std::string& error)
    {
        skip_space();
        if (!parse_value(out))
        {
            error = "invalid JSON at offset " + std::to_string(position_);
            return false;
        }
        skip_space();
        if (position_ != text_.size())
        {
            error = "trailing text after JSON at offset " + std::to_string(position_);
            return false;
        }
        return true;
    }

   private:
    void skip_space()
    {
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++position_;
            else
                break;
        }
    }

    bool consume(std::string_view token)
    {
        if (text_.substr(position_, token.size()) != token) return false;
        position_ += token.size();
        return true;
    }

    bool parse_value(Json& out)
    {
        if (position_ >= text_.size()) return false;
        const char c = text_[position_];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"')
        {
            out.type = Json::Type::String;
            return parse_string(out.text);
        }
        if (consume("true"))
        {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (consume("false"))
        {
            out.type = Json::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (consume("null"))
        {
            out.type = Json::Type::Null;
            return true;
        }
        return parse_number(out);
    }

    bool parse_number(Json& out)
    {
        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+'))
            ++position_;
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+')
                ++position_;
            else
                break;
        }
        if (position_ == start) return false;
        out.type = Json::Type::Number;
        out.number = std::stod(std::string(text_.substr(start, position_ - start)));
        return true;
    }

    bool parse_string(std::string& out)
    {
        if (position_ >= text_.size() || text_[position_] != '"') return false;
        ++position_;
        out.clear();
        while (position_ < text_.size())
        {
            const char c = text_[position_++];
            if (c == '"') return true;
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escaped = text_[position_++];
            switch (escaped)
            {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    // A field this line reads is ASCII; a \u escape is skipped rather than half-decoded, so a byte it
                    // cannot represent is empty instead of wrong.
                    if (position_ + 4 > text_.size()) return false;
                    position_ += 4;
                    break;
                default: return false;
            }
        }
        return false;
    }

    bool parse_array(Json& out)
    {
        out.type = Json::Type::Array;
        ++position_;  // '['
        skip_space();
        if (position_ < text_.size() && text_[position_] == ']')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            out.items.emplace_back();
            skip_space();
            if (!parse_value(out.items.back())) return false;
            skip_space();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == ',')
            {
                ++position_;
                continue;
            }
            if (text_[position_] == ']')
            {
                ++position_;
                return true;
            }
            return false;
        }
    }

    bool parse_object(Json& out)
    {
        out.type = Json::Type::Object;
        ++position_;  // '{'
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}')
        {
            ++position_;
            return true;
        }
        while (true)
        {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (position_ >= text_.size() || text_[position_] != ':') return false;
            ++position_;
            skip_space();
            Json value;
            if (!parse_value(value)) return false;
            out.fields.emplace(std::move(key), std::move(value));
            skip_space();
            if (position_ >= text_.size()) return false;
            if (text_[position_] == ',')
            {
                ++position_;
                continue;
            }
            if (text_[position_] == '}')
            {
                ++position_;
                return true;
            }
            return false;
        }
    }

    std::string_view text_;
    std::size_t position_{0};
};

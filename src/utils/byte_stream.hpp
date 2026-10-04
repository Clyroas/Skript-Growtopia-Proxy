#pragma once
#include <vector>
#include <string>

template <typename LengthType = std::uint16_t>
class ByteStream {
public:
    ByteStream()
        : data_{ std::vector<std::byte>() }
        , read_offset_{ 0 }
    {

    }

    ByteStream(std::byte* data, const std::size_t length)
        : data_{ std::vector(data, data + length) }
        , read_offset_{ 0 }
    {

    }

    // Added so callers can construct from get_data().data(), which is const. The
    // buffer is copied, never written through, so a const source is sufficient.
    ByteStream(const std::byte* data, const std::size_t length)
        : data_{ std::vector(data, data + length) }
        , read_offset_{ 0 }
    {

    }

    void write_data(const void* ptr, const std::size_t size)
    {
        const auto begin{ static_cast<const std::byte*>(ptr) };
        const std::byte* end{ begin + size };
        data_.insert(data_.end(), begin, end);
    }

    void write_vector(const std::vector<std::byte>& vec, const bool write_length_info = true)
    {
        if (write_length_info) {
            write(static_cast<LengthType>(vec.size()));
        }

        write_data(vec.data(), vec.size());
    }

    template <typename T>
    void write(const T& value)
    {
        write_data(&value, sizeof(T));
    }

    void write(const char* c_str, const bool write_length_info = true)
    {
        write(std::string{ c_str }, write_length_info);
    }

    void write(const std::string& str, const bool write_length_info = true)
    {
        if (write_length_info) {
            write(static_cast<LengthType>(str.size()));
        }

        write_data(str.c_str(), str.size());
    }

    template <typename T>
    ByteStream& operator<<(const T& value)
    {
        write(value);
        return *this;
    }

    bool read_data(void* ptr, const std::size_t size)
    {
        // get_remaining() avoids the unsigned underflow that a bare
        // `data_.size() - read_offset_` would produce if the offset ever passed the end.
        if (get_remaining() < size) {
            return false;
        }

        std::memcpy(ptr, data_.data() + read_offset_, size);
        read_offset_ += size;
        return true;
    }

    
    
    bool read_vector(std::vector<std::byte>& vec, LengthType length_override = 0)
    {
        LengthType length = length_override;
        if (length == 0) {
            if (!read<LengthType>(length)) {
                return false;
            }
        }

        if (get_remaining() < static_cast<std::size_t>(length)) {
            return false;
        }

        vec.resize(length);
        return read_data(vec.data(), length);
    }

    
    bool read_vector(std::vector<std::byte>& vec, std::size_t length)
    {
        if (get_remaining() < length) {
            return false;
        }
        vec.resize(length);
        return read_data(vec.data(), length);
    }

    template <typename T>
    bool read(T& value)
    {
        return read_data(&value, sizeof(T));
    }

    bool read(std::string& str, LengthType length_override = 0)
    {
        LengthType length = length_override;
        if (length == 0) {
            if (!read<LengthType>(length)) {
                return false;
            }
        }

        if (get_remaining() < static_cast<std::size_t>(length)) {
            return false;
        }

        str.resize(static_cast<std::size_t>(length));
        return read_data(str.data(), static_cast<std::size_t>(length));
    }

    bool read(std::string& str, std::size_t length)
    {
        if (get_remaining() < length) {
            return false;
        }
        str.resize(length);
        return read_data(str.data(), length);
    }

    template <typename T>
    ByteStream& operator>>(T& value)
    {
        read(value);
        return *this;
    }

    // Advances the read offset by `size` bytes (relative), clamped to the end of the
    // buffer so the offset can never pass it. Callers that want an absolute position
    // should use seek().
    void skip(const std::size_t size)
    {
        if (size >= data_.size() - read_offset_) {
            read_offset_ = data_.size();
            return;
        }
        read_offset_ += size;
    }

    // Sets the read offset to an absolute position, clamped to the buffer size.
    void seek(const std::size_t offset)
    {
        read_offset_ = offset > data_.size() ? data_.size() : offset;
    }

    [[nodiscard]] std::size_t get_read_offset() const { return read_offset_; }
    [[nodiscard]] std::size_t get_size() const { return data_.size(); }

    // Returns a reference: this used to return std::vector by value, which copied the
    // entire buffer on every call. The relay path calls it several times per packet
    // (including multi-hundred-KB map packets), so the copies were pure overhead and a
    // source of allocation stalls on the ENet service thread.
    [[nodiscard]] const std::vector<std::byte>& get_data() const { return data_; }

    [[nodiscard]] const std::byte* data() const { return data_.data(); }

    // Bytes still available to read after the current read offset.
    [[nodiscard]] std::size_t get_remaining() const
    {
        return read_offset_ < data_.size() ? data_.size() - read_offset_ : 0;
    }

private:
    std::vector<std::byte> data_;
    std::size_t read_offset_;
};

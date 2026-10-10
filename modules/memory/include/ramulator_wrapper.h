#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

class RamulatorWrapper {
  public:
    static constexpr auto MAX_ADDRESS = static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max());
    static void checkRange(std::uint64_t address, std::uint64_t size_bytes) {
        if (size_bytes == 0 || address > MAX_ADDRESS || size_bytes - 1 > MAX_ADDRESS - address)
            throw std::invalid_argument("Request must be nonempty and fit in Ramulator's address range");
    }

    enum class RequestType { Read, Write };

    struct ChannelInfo {
        std::uint32_t width_bits;
        std::uint32_t transaction_bytes;
    };

    struct ChannelRequest {
        std::size_t channel;
        std::uint64_t channel_address; // address within the channel's address space
        std::uint64_t request_offset;  // offset within the original request, for validation
        std::uint32_t size_bytes;
    };

    explicit RamulatorWrapper(const std::filesystem::path &yaml_file);
    ~RamulatorWrapper();
    RamulatorWrapper(const RamulatorWrapper &) = delete;
    RamulatorWrapper &operator=(const RamulatorWrapper &) = delete;
    RamulatorWrapper(RamulatorWrapper &&) = delete;
    RamulatorWrapper &operator=(RamulatorWrapper &&) = delete;

    std::size_t channelCount() const;
    const ChannelInfo &channelInfo() const;

    // only split requests, do not submit
    std::vector<ChannelRequest> splitChannelRequest(
        std::size_t channel, std::uint64_t channel_address, std::uint64_t size_bytes
    ) const;

    // return true if the request is accepted, false if the request is rejected (the caller may retry later).
    // The request must be non-empty and not cross a transaction boundary;
    // for long requests, call splitChannelRequest() first.
    // please don't call send/tick/finalize in the callback
    bool send(
        RequestType type, std::size_t channel, std::uint64_t channel_address, std::uint32_t size_bytes,
        std::function<void()> on_complete = {}
    );

    void tick();
    void tick(std::size_t channel);

    std::size_t pendingRequests() const noexcept;
    std::size_t pendingRequests(std::size_t channel) const;
    bool idle() const noexcept;
    bool idle(std::size_t channel) const;
    void finalize();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

#include <cstddef>
#include <ramulator_wrapper.h>

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include <ramulator/base/config.h>
#include <ramulator/base/factory.h>
#include <ramulator/base/request.h>
#include <ramulator/frontend/i_frontend.h>
#include <ramulator/memory_system/i_memory_system.h>

struct RamulatorWrapper::Impl {
    struct Channel {
        std::unique_ptr<Ramulator::IFrontEnd> frontend;
        std::unique_ptr<Ramulator::IMemorySystem> memory;
        std::size_t outstanding = 0;
        bool ticking = false;
    };

    std::vector<Channel> channels = {};
    ChannelInfo channel_info = {};
    bool finalized = false;

    explicit Impl(const std::filesystem::path &yaml_file) {
        const auto config = Ramulator::Config::parse_config_file(yaml_file.string());
        const int channel_count = config["channel_count"].as<int>();
        if (channel_count <= 0) throw std::invalid_argument("channel_count must be positive");

        // remove channel_count from the config map to create independent single-channel instances
        auto instance_fields = config.map();
        instance_fields.erase("channel_count");
        const Ramulator::ConfigNode instance_config(std::move(instance_fields));
        const auto &memory_config = instance_config["memory_system"];
        const auto &mapper = memory_config["channel_mapper"];
        const auto &controllers = memory_config["controllers"];
        const auto &controller = controllers.seq().at(0);

        // check invalid configurations that would break the wrapper's assumptions
        if (instance_config["frontend"]["impl"].as<std::string>("") != "External")
            throw std::invalid_argument("RamulatorWrapper requires an External frontend");
        if (memory_config["impl"].as<std::string>("") != "GenericDRAM")
            throw std::invalid_argument("RamulatorWrapper requires GenericDRAM");
        if (mapper["impl"].as<std::string>("") != "CacheLineInterleave")
            throw std::invalid_argument("RamulatorWrapper requires CacheLineInterleave for single-channel routing");
        if (controllers.seq().size() != 1)
            throw std::invalid_argument("Each instance requires exactly one controller configuration");
        if (controllers.seq().at(0)["addr_mapper"]["impl"].as<std::string>("") == "PassThroughAddrMapper")
            throw std::invalid_argument("PassThroughAddrMapper cannot map flat byte-address requests");

        // create independent single-channel instances
        const auto create_channel = [&](int) {
            Channel channel;
            channel.frontend.reset(Ramulator::Factory::create_frontend(instance_config));
            channel.memory.reset(Ramulator::Factory::create_memory_system(instance_config));
            channel.frontend->connect_memory_system(channel.memory.get());
            channel.memory->connect_frontend(channel.frontend.get());
            return channel;
        };
        channels = std::views::iota(0, channel_count)      //
                   | std::views::transform(create_channel) //
                   | std::ranges::to<std::vector<Channel>>();

        // All instances share the same channel configuration.
        channel_info = {
            .width_bits = static_cast<std::uint32_t>(controller["dram"]["channel_width"].as<int>()),
            .transaction_bytes = static_cast<std::uint32_t>(channels.front().memory->get_tx_bytes())
        };
    }
};

// constructor and destructor
RamulatorWrapper::RamulatorWrapper(const std::filesystem::path &yaml_file) : _impl(std::make_unique<Impl>(yaml_file)) {}
RamulatorWrapper::~RamulatorWrapper() = default;

std::size_t RamulatorWrapper::channelCount() const { return _impl->channels.size(); }
const RamulatorWrapper::ChannelInfo &RamulatorWrapper::channelInfo() const { return _impl->channel_info; }

std::vector<RamulatorWrapper::ChannelRequest> RamulatorWrapper::splitChannelRequest(
    std::size_t channel, std::uint64_t channel_address, std::uint64_t size_bytes
) const {
    if (channel >= channelCount()) throw std::out_of_range("Channel index out of range");
    const auto tx_bytes = channelInfo().transaction_bytes;
    checkRange(channel_address, size_bytes);

    std::vector<ChannelRequest> result;
    const auto first = tx_bytes - channel_address % tx_bytes;
    const auto count = 1 + (size_bytes - 1 >= first ? 1 + (size_bytes - 1 - first) / tx_bytes : 0);
    result.reserve(static_cast<std::size_t>(count));

    for (std::uint64_t offset = 0; offset < size_bytes;) {
        const auto local = channel_address + offset;
        const auto length = std::min(size_bytes - offset, tx_bytes - local % tx_bytes);
        result.push_back({channel, local, offset, static_cast<std::uint32_t>(length)});
        offset += length;
    }
    return result;
}

bool RamulatorWrapper::send(
    RequestType type, std::size_t channel, std::uint64_t channel_address, std::uint32_t size_bytes,
    std::function<void()> on_complete
) {
    if (_impl->finalized) throw std::logic_error("Cannot send after finalize()");
    auto &instance = _impl->channels.at(channel);
    checkRange(channel_address, size_bytes);

    const auto tx_bytes = channelInfo().transaction_bytes;
    if (size_bytes > tx_bytes - channel_address % tx_bytes)
        throw std::invalid_argument("Split requests at transaction boundaries before send()");

    const int ramulator_type =
        type == RequestType::Read ? Ramulator::Request::Type::Read : Ramulator::Request::Type::Write;

    // write merging may cause the callback to be invoked before send() returns; increment outstanding first
    ++instance.outstanding;
    const bool accepted = instance.frontend->receive_external_requests(
        ramulator_type, static_cast<Ramulator::Addr_t>(channel_address), -1,
        [state = &instance, callback = std::move(on_complete)](Ramulator::Request &) {
            --state->outstanding;
            if (callback) callback();
        },
        static_cast<int>(size_bytes)
    );
    if (!accepted) --instance.outstanding;
    return accepted;
}

void RamulatorWrapper::tick() {
    std::ranges::for_each(
        std::views::iota(std::size_t{0}, channelCount()), //
        [this](std::size_t channel) { tick(channel); }
    );
}
void RamulatorWrapper::tick(std::size_t channel) {
    if (_impl->finalized) throw std::logic_error("Cannot tick after finalize()");
    auto &instance = _impl->channels.at(channel);
    if (instance.ticking) throw std::logic_error("Cannot tick the same channel recursively");

    instance.ticking = true;
    instance.memory->tick();
    instance.ticking = false;
}

std::size_t RamulatorWrapper::pendingRequests() const noexcept {
    const auto tmp = std::views::iota(std::size_t{0}, channelCount()) |
                     std::views::transform([this](const auto channel) { return pendingRequests(channel); });
    return std::ranges::fold_left(tmp, std::size_t(0), std::plus<>());
}
std::size_t RamulatorWrapper::pendingRequests(std::size_t channel) const {
    return _impl->channels.at(channel).outstanding;
}

bool RamulatorWrapper::idle() const noexcept { return pendingRequests() == 0; }
bool RamulatorWrapper::idle(std::size_t channel) const { return pendingRequests(channel) == 0; }

void RamulatorWrapper::finalize() {
    if (_impl->finalized) return;
    if (std::ranges::any_of(_impl->channels, [](const auto &channel) { return channel.ticking; }))
        throw std::logic_error("Cannot finalize() while any channel is ticking");
    if (std::ranges::any_of(_impl->channels, [](const auto &channel) { return channel.outstanding != 0; }))
        throw std::logic_error("Cannot finalize() while any channel has pending requests");
    std::ranges::for_each(_impl->channels, [](auto &channel) {
        channel.frontend->finalize();
        channel.memory->finalize();
    });
    _impl->finalized = true;
}

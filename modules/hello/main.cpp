#include <algorithm>
#include <ramulator_wrapper.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <vector>

using RequestType = RamulatorWrapper::RequestType;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void runPhase(RamulatorWrapper &memory, RequestType type) {
    struct ChannelWork {
        std::vector<RamulatorWrapper::ChannelRequest> requests;
        std::vector<bool> finished;
        std::size_t submitted = 0;
        std::size_t completed = 0;
    };

    const auto tx_bytes = memory.channelInfo().transaction_bytes;
    const std::uint64_t start = 0x1003; // 覆盖非对齐首部和尾部。
    const auto size = static_cast<std::uint64_t>(tx_bytes) * 256 + 7;

    std::vector<ChannelWork> work =                             //
        std::views::iota(std::size_t{0}, memory.channelCount()) //
        | std::views::transform([=, &memory](std::size_t channel) {
              ChannelWork state;
              state.requests = memory.splitChannelRequest(channel, start, size);
              // Submmit consecutive write requests to the same address in the same cycle
              // cover the synchronous completion callback of write merging.
              if (type == RequestType::Write) {
                  const auto duplicate = state.requests.front();
                  state.requests.insert(state.requests.begin() + 1, duplicate);
              }
              state.finished.resize(state.requests.size(), false);
              return state;
          }) //
        | std::ranges::to<std::vector>();

    const std::size_t total = std::ranges::fold_left(
        work | std::views::transform([](const auto &state) { return state.requests.size(); }), //
        std::size_t(0), std::plus<>()
    );

    // 回调只写外部事件队列；所有检查和后续模拟器操作都在后端调用退出后进行。
    std::size_t completed = 0;
    std::size_t retries = 0;
    std::uint64_t cycles = 0;
    constexpr std::uint64_t max_cycles = 1'000'000;

    while (completed < total && cycles < max_cycles) {
        for (std::size_t channel = 0; channel < work.size(); ++channel) {
            auto &state = work[channel];
            while (state.submitted < state.requests.size()) {
                const auto index = state.submitted;
                const auto &request = state.requests[index];

                const auto callback = [&state, &completed, index] {
                    require(index < state.submitted, "Rejected request produced a completion");
                    require(!state.finished[index], "Request completed more than once");
                    state.finished[index] = true;
                    state.completed++;
                    completed++;
                };

                state.submitted++;
                const bool accepted = memory.send(type, channel, request.channel_address, request.size_bytes, callback);
                if (!accepted) {
                    retries++;
                    state.submitted--; // 重试时保持原索引。
                    break;             // 本通道满，继续尝试其他通道。
                }
            }
        }

        memory.tick();
        ++cycles;

        std::size_t pending = 0;
        for (std::size_t channel = 0; channel < work.size(); ++channel) {
            const auto expected = work[channel].submitted - work[channel].completed;
            require(memory.pendingRequests(channel) == expected, "Per-channel outstanding count mismatch");
            require(memory.idle(channel) == (expected == 0), "Per-channel idle state mismatch");
            pending += expected;
        }
        require(memory.pendingRequests() == pending, "Total outstanding count mismatch");
    }

    require(completed == total, "Simulation timed out before all requests completed");
    require(memory.idle(), "Simulator is not idle after all completions");
    std::println(
        "  {}: {} requests completed, {} cycles, {} retries", type == RequestType::Write ? "Write" : "Read", completed,
        cycles, retries
    );
}

void testMemory(const std::filesystem::path &config) {
    std::println("Configuration: {}", config.string());
    RamulatorWrapper memory{config};
    const auto &info = memory.channelInfo();
    require(memory.channelCount() > 0 && info.width_bits > 0 && info.transaction_bytes > 0, "Invalid channel metadata");
    require(memory.idle(), "New simulator should be idle");
    std::println(
        "  Channels: {}, width: {} bits, transaction: {} bytes", memory.channelCount(), info.width_bits,
        info.transaction_bytes
    );

    runPhase(memory, RequestType::Write);
    runPhase(memory, RequestType::Read);
    memory.finalize();
    std::println("  PASS");
}

int main(int argc, char **argv) {
    if (argc == 1) {
        testMemory("dram_configs/config_hbm2.yaml");
        testMemory("dram_configs/config_hbm3.yaml");
    } else {
        std::ranges::for_each(
            std::span(argv + 1, static_cast<std::size_t>(argc - 1)), //
            [](const char *arg) { testMemory(arg); }
        );
    }
    std::println("All RamulatorWrapper smoke tests passed.");
    return 0;
}

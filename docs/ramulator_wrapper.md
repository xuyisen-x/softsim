# RamulatorWrapper 使用说明

RamulatorWrapper 为每个逻辑通道创建独立的 Ramulator frontend 和单通道 memory system。
调用方通过 **通道编号 + 通道内字节地址** 提交请求，负责请求重试、模拟周期调度和上层完成事件处理。
RamulatorWrapper 模拟请求处理和时序，不存储实际读写数据。

> **必须注意：用户回调由 Ramulator 直接调用。**
> 回调可能在 `send()` 返回前同步执行，也可能在 `tick()` 内执行。
> 包装类不提供延迟回调队列、线程同步或异常恢复；调用方必须提供外部调度及必要的同步机制，
> 不能在回调中重新驱动或销毁模拟器。具体要求见[回调和 outstanding 计数](#回调和-outstanding-计数)。

## 代码与构建

| 内容 | 位置 |
|---|---|
| 公开接口 | [ramulator_wrapper.h](../modules/memory/include/ramulator_wrapper.h) |
| 实现 | [ramulator_wrapper.cpp](../modules/memory/src/ramulator_wrapper.cpp) |
| HBM2 配置 | [config_hbm2.yaml](../dram_configs/config_hbm2.yaml) |
| HBM3 配置 | [config_hbm3.yaml](../dram_configs/config_hbm3.yaml) |

使用模块时链接 `ram_wrapper`，并包含 `<ramulator_wrapper.h>`：

```cmake
target_link_libraries(your_target PRIVATE ram_wrapper)
```

项目使用 C++23。通用构建、Invoke 和编辑器配置见[开发指南](development.md)。

## 配置与初始化

YAML 顶层的 `channel_count` 指定实例数，`memory_system.controllers` 必须是仅含一个元素的列表。
以下仅展示配置结构；完整 DRAM 组织、时序和调度配置请使用仓库中的 YAML。

```yaml
channel_count: 16
frontend:
  impl: External
  clock_ratio: 16
memory_system:
  impl: GenericDRAM
  clock_ratio: 1
  channel_mapper:
    impl: CacheLineInterleave
    interleave_bits: 0
  controllers:
    - impl: HBM34
      # 此处还需 scheduler、refresh_manager、row_policy、addr_mapper 和 dram 等完整配置。
```

构造函数执行以下步骤：

1. 读取正整数 `channel_count`，从传给 Ramulator 的配置副本中删除该字段。
2. 检查 frontend、memory system 和 mapper 是否符合包装类接口，以及 controller 模板是否只有一个。
3. 用剩余配置分别创建并连接各通道的 frontend 和 memory system；不改写磁盘上的 YAML 或其余配置字段。
4. 从模板读取统一位宽，从第一个初始化后的后端读取统一事务大小；不要额外创建 DRAMSpec 对象重复解析。

所有实例采用相同配置；对外编号为 `0 .. channel_count - 1`，各后端内部的通道编号均为 0。
各实例的 controller、请求队列、统计和插件相互独立。

| 仓库配置 | 实例数 | 单通道位宽 | 事务大小 |
|---|---:|---:|---:|
| HBM2 | 8 | 64 bit | 32 B |
| HBM3 | 16 | 32 bit | 32 B |

`channel_mapper` 配置原样传入后端。包装类自身不计算交错粒度，也不提供全局地址编码或解码。
当前接口要求 `External + GenericDRAM + CacheLineInterleave`；
controller 的 `PassThroughAddrMapper` 不适用于这里的平坦字节地址输入。

## 接口

| 接口 | 行为 |
|---|---|
| `channelCount()` | 返回独立通道实例数。 |
| `channelInfo()` | 返回所有实例共用的位宽和事务字节数。 |
| `splitChannelRequest(channel, address, bytes)` | 按事务边界拆分指定通道内的连续请求，只返回描述，不提交。 |
| `send(type, channel, address, bytes, callback)` | 立即尝试提交一个子请求；返回 true 表示接受，false 表示拒绝。 |
| `tick(channel)` | 推进一个实例一个内存周期。 |
| `tick()` | 按通道编号依次推进所有实例各一个周期，不创建线程。 |
| `pendingRequests(channel)` / `pendingRequests()` | 查询单通道 / 全部通道的 outstanding 数量。 |
| `idle(channel)` / `idle()` | 判断对应 outstanding 数量是否为零。 |
| `finalize()` | 所有实例空闲且不在 tick 中时完成各实例统计；后续不能 send 或 tick。 |

`send()` 没有软件缓存队列。返回 false 后，请求未被接受，调用方可推进模拟后重试，
同时继续向其他通道提交请求；返回 true 后不能重复提交同一子请求。
请求类型使用 `RequestType::Read` 或 `RequestType::Write`。

每个提交的请求必须非空，并落在一个事务块内。地址和长度检查的是
Ramulator 地址类型可表示的数值范围，不是配置中的实际 DRAM 容量。

## 通道内请求拆分

`transaction_bytes` 表示单通道一次 DRAM 事务的传输字节数，直接从后端查询。
通常等于通道位宽乘内部预取长度再除以 8；若规格提供有效的 payload 大小覆盖值，则采用该值。

例如，事务大小为 32 B：

```cpp
auto parts = memory.splitChannelRequest(1, 20, 60);
```

结果始终属于通道 1：

| 通道内地址 | 原请求偏移 | 长度 | 覆盖范围 |
|---:|---:|---:|---|
| 20 | 0 | 12 B | 20–31 |
| 32 | 12 | 32 B | 32–63 |
| 64 | 44 | 16 B | 64–79 |

返回的 `ChannelRequest` 保存 `channel`、`channel_address`、`request_offset` 和 `size_bytes`。
调用方逐个提交并自行汇总大请求完成情况；包装类不会自动拆分 send 请求。

## 回调和 outstanding 计数

Ramulator 在普通读写完成时调用回调；同地址写合并可能在提交过程中立即调用回调，
先于 `send()` 返回。写回调表示后端认为该请求已完成或已合并，不表示包装类验证了实际数据内容。

每个通道只维护一个计数器：

- 提交前递增，覆盖 send 内同步完成的情况。
- 后端拒绝请求时扣回。
- 后端完成时先递减，再执行用户回调；即使没有提供用户回调，也正常计数。

包装类不保存完成记录，不在每次 tick 中扫描 outstanding。
`idle()` 不包含外部尚未提交的请求或尚未处理的上层事件，也不代表当前后端调用栈已经退出。

调用者必须遵守以下约定：

1. **先准备回调状态，再调用 send。** 请求标识和捕获对象必须在提交前有效，生命周期覆盖请求完成。
   不能假设“返回 true 后才会收到回调”。
2. **禁止在回调中重入模拟器。** 回调只更新上层状态或记录事件；
   后续 send、tick、finalize、对象销毁必须由外部调度阶段在后端调用返回后进行。
   即使回调观察到 idle，也不能立即销毁实例。
3. **异常后终止当前模拟。** 不让用户异常逃逸到后端后再捕获并继续推进；
   包装类不负责后端异常或内存分配失败的恢复。send 返回 false 属于正常队列反压，不是异常。
4. **共享状态需要外部同步。** 多通道并行时回调也可能并发，可使用每线程事件队列、
   并发队列或同步的上层状态。
5. **注意同步回调的锁上下文。** 如果外部持锁调用 send/tick，回调会在同一调用栈执行；
   再次获取同一把非递归锁会死锁。加锁不能代替禁止重入和外部事件调度。

## 单线程使用示例

以下示例让回调只记录完成事件，后端调用退出后再处理上层工作。实际运行可设置周期上限，
避免配置或调度问题造成无限等待。

```cpp
#include <ramulator_wrapper.h>
#include <vector>

RamulatorWrapper memory("dram_configs/config_hbm3.yaml");
using Type = RamulatorWrapper::RequestType;

const auto parts = memory.splitChannelRequest(1, 0x1003, 256);
std::vector<std::size_t> events;
events.reserve(parts.size());
std::size_t submitted = 0;
std::size_t completed = 0;

while (submitted < parts.size() || !memory.idle(1)) {
    while (submitted < parts.size()) {
        const auto &part = parts[submitted];
        const auto index = submitted; // 在 send 之前准备回调标识。
        if (!memory.send(Type::Read, part.channel, part.channel_address, part.size_bytes,
                         [&events, index] { events.push_back(index); }))
            break;
        ++submitted;
    }

    memory.tick(1);

    for (const auto index : events) {
        // 后端调用已返回；在这里更新上层状态、释放请求资源或安排后续操作。
        (void)index;
        ++completed;
    }
    events.clear();
}
memory.finalize();
```

这里只访问通道 1，因此仅调用 `tick(1)`。多通道工作负载可用 `tick()` 推进所有通道。

## 并行推进与生命周期

外部可为不同通道分配不同工作线程。每个通道的 send、tick 和状态查询必须串行，
outstanding 是普通计数器而不是原子变量。回调运行在执行对应后端操作的线程中。

若各通道需要保持相同模拟周期，应由调用者在每轮推进后设置同步点。
无参数 tick、汇总状态查询、finalize 和析构只能在所有工作线程停止访问实例后执行。
插件若访问共享输出文件或其他外部资源，也需要处理并发访问。

析构只释放对象，不隐式排空未完成请求、不触发剩余回调。
`finalize()` 要求所有通道空闲，但不负责停止或等待外部线程。
tick 的单位是内存周期，CPU/前端时钟换算和调用频率由外部调度器负责。

## 维护约定

- 保持本文定义的独立通道、通道内字节地址、配置解析、事务拆分和 outstanding 计数语义。
- 保持 Ramulator 直接回调，不引入延迟回调、完成队列或每周期在途扫描；回调和并发操作遵循上文约定。
- 异常终止当前模拟，不添加后端异常或内存分配失败的恢复机制；队列拒绝可由调用方重试。
- 参数检查聚焦于接口成立所需的条件，没有与当前任务相关的理由时，不重新加入已移除的配置检查。
- 修改公开接口、配置格式或回调行为时，同步更新本文，不在模块目录中维护重复的 README。


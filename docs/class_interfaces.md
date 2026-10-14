# 首版模拟器：类、数据结构与函数接口草案

本文供实现前检查，尚未对应已实现的 C++ 接口。架构依据见 [总体设计](overall_design.md)，
内存调用约束见 [RamulatorWrapper](ramulator_wrapper.md)。以下具体类型、命名与首版取舍均为本草案建议。
代码块是接口与成员示意，省略头文件、部分构造细节及普通访问函数，不是完整实现。

## 1. 实现边界与对象关系

- `NM`、`EM`、`IM` 各自直接持有私有 `std::vector`，不另建通用 Cache 类，也不模拟 tag、cache line 或替换算法。
- 控制状态、事件、请求进度和槽位描述使用原生结构体；缓存中的数值和记录使用类型化数组，不需要转成字节流。
- HBM 请求仍按配置的字节布局计算地址、长度并拆分事务；宿主机结构体的 `sizeof` 不直接代表模拟硬件布局。
- 首版单线程推进所有 PE。每个 PE 一个 `MemoryPort`，共用已有的 `RamulatorWrapper`，访问各自绑定的独立通道。
- NM、EM、IM 各自配置独立的读、写 credit。MemoryPort 按组件分别维护额度，在向后端发放每笔子请求前检查，不跨组件或读写方向借用。
- 同一控制器的读、写 credit 配置上限总和分别不超过其读、写队列容量；合法请求有 credit 发放就必须被接受，意外拒绝直接报错终止。
- 首版每个窗口结束前完成神经元状态写回，下一窗口重新读取，先不实现 NC 跨窗口驻留。
- spike 消息直接携带源 ID 和绝对发放步号；本地投递、广播、EM 排队与查表均保留这两个字段。
- 第五部分的容量问题先用小工作负载和足量配置绕过：EQ 足量，IB 使用 `2M` 个独立步缓冲（M 为每窗口子步数）；首轮实现不启用 HE/HI 溢出。
  NC 和 PF 仍有限，并正常模拟等待、复用和反压。EQ 超容量必须明确报错，不能丢事件或偷偷扩成无限缓存。
- EM 待查表的 spike 列表使用动态 `std::vector`，收到时追加，由容器按需扩容；首版不建模该接收列表的容量与反压。

| 对象 | 直接拥有的数据 | 对外职责 |
| --- | --- | --- |
| `NM` | NC 状态数组、槽位元数据、CGRA 在途任务 | 状态预取、分组更新、输出 spike、状态写回 |
| `EM` | spike 待查表记录、查询槽、时间桶、事件槽、PF | 创建事件、预取下一窗口突触、交给 IM |
| `IM` | 按绝对步号标记的 `2M` 个步缓冲及各自消费标记 | 按到期步分类累加、外部刺激注入、向 NM 提供指定步输入 |
| `PeBackingStore` | 本 PE 的 HBM 功能数据 | 保存数值与记录，不自行推进时序 |
| `MemoryPort` | 有界的逻辑请求、事务完成状态及按组件独立保存的读写 credit 状态 | 拆分请求、按 credit 等待与发放、汇总完成；不拥有组件缓存 |
| `Simulator` | PE 集合、硬件时钟、全局窗口号 | 瞬时广播、推进周期、判定全局屏障 |

NM 与 EM 都通过 IM 的函数交互，不读取 IM 的私有数组。IM 不访问 PF。
`MemoryPort` 只是上层请求管理辅助类，不修改现有 `RamulatorWrapper` 的接口或回调行为。

## 2. 公共类型与配置

### 2.1 标识、范围与跨组件数据

```cpp
// B：神经元最小仿真时间步；M：一个同步窗口包含的 B 子步数。
// PE：处理单元；CGRA：神经元更新的计算黑盒；Scalar：32 bit 浮点数。
using Scalar = float;                  // 首版统一使用 32 bit 浮点数。
using SourceId = std::uint64_t;         // 全局源神经元 ID。
using LocalIndex = std::size_t;         // 本 PE 内的目标索引。
using RecordIndex = std::uint64_t;      // HBM 逻辑数组中的记录下标。
using SimStep = std::uint64_t;          // 以 B 为单位的绝对子步号。
using WindowId = std::uint64_t;
using Cycle = std::uint64_t;            // PE/CGRA 周期，与 SimStep 分开使用。

struct NeuronRange {
    LocalIndex first;
    std::size_t count;
};

struct Spike {
    SourceId source;
    SimStep emitted_step;              // 产生 spike 的绝对仿真步号，属于消息本身。
};

struct IsynContribution {
    SimStep due_step;
    LocalIndex target;
    std::uint8_t channel;
    Scalar weight;
};

using SpikeSink = std::function<void(Spike)>;
```

`Spike` 同时包含源 ID 和绝对发放步号，接收方直接从消息读取发放时间。
`SimStep` 同时确定窗口与子步：`window = step / M`、`substep = step % M`。
同一源在一个窗口内不同子步的 spike 是独立消息，不能按源 ID 合并，也不能从接收顺序推断发放步。
`SpikeSink` 在 CGRA 结果到达时同步调用；它只通知 EM，不允许在函数内推进硬件周期或调用内存后端。

### 2.2 CGRA 功能接口

```cpp
// P：一次 CGRA 调用处理的神经元数；S：每个神经元的 states 分量数；K：Isyn 输入分量数。
// C：全部输入电流分量数，首版 C=K；L：CGRA 延迟周期数；II：两次发射的最小周期间隔。
// B：最小仿真时间步；A：最小突触延迟单元；M=A/B：每窗口子步数；Dmax：最大突触延迟。
// NC：神经元状态缓存；NM、IM、EM：神经元、输入累加、事件管理组件；PF：突触预取缓冲区。
// read_credits/write_credits：所属组件允许同时在途的 HBM 读/写子请求数上限。
struct CgraInputView {
    std::span<const Scalar> states;      // P × S，按神经元排列。
    std::span<const Scalar> inputs;      // P × K，按神经元排列。
};

struct NeuronResult {
    std::vector<Scalar> states;          // 长度 S。
    bool spike;
};

using CgraFunction =
    std::function<std::vector<NeuronResult>(const CgraInputView &)>;

struct ModelConfig {
    std::uint8_t state_count;            // S，包含运行时可变参数。
    std::uint8_t input_count;            // K，1～4；首版 C=K。
    CgraFunction evaluate;
};

struct WindowConfig {
    std::uint32_t steps_per_window;      // M=A/B，至少为 1。
    WindowId max_delay_windows;          // Dmax/A，至少为 1。
};

struct NmConfig {
    std::size_t group_size;              // P：一次 CGRA 调用处理的神经元数，至少为 1。
    Cycle latency;                      // L，首版要求至少为 1；II 固定为 1。
    std::size_t nc_slots;                // NC 可同时容纳的组数，每槽存一组、最多 P 个神经元。
    std::size_t read_credits;
    std::size_t write_credits;
};

struct ImConfig {
    std::size_t additions_per_cycle;
    std::size_t read_neurons_per_cycle;  // 至少能读一组实际神经元。
    std::size_t read_credits;            // 首版不访问 HBM，可设为 0。
    std::size_t write_credits;           // 首版不访问 HBM，可设为 0。
};

struct EmConfig {
    std::size_t lookup_slots;
    std::size_t event_slots;
    std::size_t pf_slots;
    std::size_t synapses_per_pf_slot;
    std::size_t metadata_records_per_cycle;
    std::size_t deliveries_per_cycle;
    std::size_t read_credits;            // 索引、组描述和突触读取共用 EM 的读额度。
    std::size_t write_credits;           // 首版未启用 HE，可设为 0。
};
```

CGRA 抽象为 `output = f(input)`。LIF 时间常数等固定参数在编译时吸收进 CGRA，
模拟器中由 `CgraFunction` 的闭包捕获，不设置独立的 parameters 字段或读取通路。
运行时可变参数视为 `states` 的一部分，计入 S，随状态一起读入、更新和写回。
Isyn 输入使用 `inputs`，外部刺激也进入 Isyn，不增加一条背景输入接口。
S+K 目前预计最多约 8，计数使用 `std::uint8_t`；容器长度、偏移与容量计算使用 `std::size_t`。

`group_size` 和 `nc_slots` 的单位不同：前者是一组中的神经元数，后者是 NC 的组槽位数。
例如 `group_size=4`、`nc_slots=8`，表示一次 CGRA 调用处理 4 个神经元，NC 最多容纳 8 组、共 32 个神经元的状态。
NC 数值容量为 `nc_slots × group_size × S` 个 Scalar；改变槽位数不改变一次 CGRA 调用的宽度。
读、写 credit 限制的是 HBM 在途子请求数，与 NC 槽位数、CGRA 在途任务数及 IM 每周期处理额度独立。
六份额度分别配置；未启用的访问方向可设为 0，需要发放请求的方向必须为正。

每次 CGRA 调用输入和输出均覆盖 P 个 lane；末组不足 P 时，NM 将无效 lane 的输入补零，并忽略其全部输出。
真实 HBM 访问和 IM 消费只覆盖有效神经元。`evaluate` 必须使各 lane 的更新相互独立，不能让补齐 lane 影响真实神经元。
返回值长度必须为 P，每条结果的状态长度必须为 S；spike 使用结构体字段，不借用某个浮点元素表示。

功能函数可在发射时立即计算结果，但只能在 `c+L` 周期使结果可见；它不得修改外部状态或保留输入 span。
闭包捕获的常量在运行期间保持不变；所有需要跨调用保存的可变模型状态均通过 states 输入和结果返回。

## 3. HBM 功能数据与地址布局

### 3.1 类型化 backing store

```cpp
// N：本 PE 的神经元数；S：每个神经元的 states 分量数，包含运行时可变参数。
// A：最小突触延迟单元；q：延迟的 A 倍数，即 delay_windows；HBM：高带宽内存。
struct NeuronGroupsRecord {
    RecordIndex groups_begin;
    std::uint64_t group_count;
};

struct SynapseGroupRecord {
    RecordIndex synapses_begin;
    std::uint64_t synapse_count;
    WindowId delay_windows;              // q，表示延迟 qA。
};

struct Synapse {
    std::uint64_t target;                // 草案约定低 2 bit 为输入通道。
    Scalar weight;
};

struct PeBackingStore {
    std::vector<Scalar> neuron_states;   // N × S，本地神经元顺序。
    std::vector<NeuronGroupsRecord> neuron_groups; // 按全局 source ID 索引。
    std::vector<SynapseGroupRecord> synapse_groups;
    std::vector<Synapse> synapses;
};

struct ByteRange {
    std::uint64_t address;               // 通道内字节地址。
    std::uint64_t bytes;
};

struct MemoryRegion {
    std::uint64_t base;
    std::uint64_t record_bytes;
    std::uint64_t record_count;
};

struct PeMemoryLayout {
    MemoryRegion neurons;
    MemoryRegion neuron_groups;
    MemoryRegion synapse_groups;
    MemoryRegion synapses;
};

ByteRange recordRange(const MemoryRegion &, RecordIndex first, std::uint64_t count);
std::uint64_t packTarget(LocalIndex target, std::uint8_t channel);
LocalIndex targetIndex(std::uint64_t packed);
std::uint8_t inputChannel(std::uint64_t packed);
```

`Scalar` 按 4 B 建模，各记录的模拟步长分别取 `S×4`、16、24、12 B。
其中突触记录为 8 B target 加 4 B weight；宿主结构体可能存在的填充不计入模拟步长。
`recordRange()` 计算 `base + first × record_bytes` 与 `count × record_bytes`，检查范围和溢出。
以后调整布局时只改变布局计算及必要的数值转换，不需要让控制结构体按硬件位宽打包。

神经元状态由 NM 读写；三张连接表初始化后只读，由 EM 访问。
读取只在对应逻辑请求全部完成后复制到组件私有数组；写回完成后才将保留在 NC 的新状态复制到 backing store。
NM 在该复制完成前不复用槽位，同一神经元也不会开始下一窗口的读取，因此没有同地址旧写覆盖新写的问题。
backing store 是功能数据的保存位置，不是绕过 HBM 时序的读取捷径。

### 3.2 PE 配置与生命周期

```cpp
// N：local_neuron_count，本 PE 的神经元数；M：每窗口子步数；K：Isyn 输入分量数。
// PE：处理单元；全局神经元 ID = source_id_base + 本地 index。
struct PeConfig {
    std::size_t memory_channel;
    std::size_t global_neuron_count;
    SourceId source_id_base;             // 本 PE 连续神经元区间的起始全局 ID。
    std::size_t local_neuron_count;      // N。
    WindowConfig time;
    ModelConfig model;
    NmConfig nm;
    ImConfig im;
    EmConfig em;
    PeMemoryLayout memory_layout;
};
```

`N=local_neuron_count`，`global_id=source_id_base+local_index`，不保存逐神经元的 ID 映射数组。
各组件引用同一份不可变配置，避免在 NM、IM、EM 分别维护不一致的 N、M、K。
配置、backing store 与 `MemoryPort` 的地址在运行期间保持稳定，生命周期覆盖引用它们的组件和所有回调。
全局 source ID 采用 `[0, global_neuron_count)` 的稠密编号，每个 PE 获得连续区间 `[source_id_base, source_id_base+N)`，
这些区间互不重叠且恰好覆盖全部神经元。神经元重排序及对应连接表的调整由编译器负责。

## 4. 内存辅助类 MemoryPort

```cpp
// NM、EM、IM：神经元、事件、输入累加管理组件；MemoryPort：单个 PE 的 HBM 请求管理器。
// slot/generation：请求槽下标及复用代次；ByteRange 使用通道内字节地址和字节数。
// credit：一个拆分后 HBM 子请求的在途额度；limit 为配置上限，available 为当前可用数量。
enum class MemoryOwner { NM, EM, IM };

struct MemoryCreditState {
    std::size_t read_limit;
    std::size_t write_limit;
    std::size_t read_available;
    std::size_t write_available;
};

struct TransferId {
    std::size_t slot;
    std::uint64_t generation;
};

enum class PartState { Pending, Submitting, Accepted, Complete };

struct TransferPart {
    RamulatorWrapper::ChannelRequest request;
    PartState state;
};

struct TransferState {
    std::uint64_t generation;
    bool occupied;
    MemoryOwner owner;
    RamulatorWrapper::RequestType type;
    std::vector<TransferPart> parts;
};

class MemoryPort {
  public:
    MemoryPort(RamulatorWrapper &, const PeConfig &, std::size_t transfer_slots);

    std::optional<TransferId> tryRead(MemoryOwner, ByteRange);
    std::optional<TransferId> tryWrite(MemoryOwner, ByteRange);
    const MemoryCreditState &credits(MemoryOwner) const;
    bool finished(TransferId) const;
    void release(TransferId);            // 仅允许释放已完成请求。
    void submitPending();               // 只由外部调度阶段调用。
    bool empty() const;                  // 包括尚未提交及尚未 release 的请求。

  private:
    RamulatorWrapper &memory_;
    std::size_t channel_;
    std::vector<MemoryCreditState> credits_; // 固定三项，分别属于 NM、EM、IM。
    std::vector<TransferState> transfers_;
    // 另有空闲槽索引和轮询位置。
};
```

credit 的归属与初始化：

- MemoryPort 从 PeConfig 取得通道号以及 NM、EM、IM 各自的 read_credits/write_credits，分别初始化三份状态。
  `credits_` 只是集中维护的位置，每项的读、写额度仍由对应组件独占，不能汇总成可相互借用的公共额度。
- 装配阶段从用于创建后端的同一份控制器配置取得实际生效的 `read_buffer_size`、`write_buffer_size`，
  校验该 PE 三个组件的 read_credits 之和不超过读队列容量，write_credits 之和不超过写队列容量；不满足则拒绝启动。
  读写分别校验，不能跨通道抵扣；进入这两类队列的所有上层请求必须由 MemoryPort 按 credit 发放。
- 可用额度初值等于各自上限，不随 `beginCycle()` 或 `beginWindow()` 重置。`credits(owner)` 仅提供只读查询。
- 按实际调用后端 `send()` 的子请求计数；不同大小的子请求都占一个对应方向的 credit。
  EM 的元数据与突触读取共同消耗 EM 读 credit，NM 读写及 IM 未来的 HI 访问使用各自额度。

首版以完成回调作为 credit 归还时点，即使事务已离开控制器请求队列也不提前归还。
回调不重入发放；在后端调用返回后的外部发放点，队列占用不超过尚未归还的 credit 数，因此有 credit 发放的合法事务必须被控制器接受。
该保证只覆盖控制器接收；请求登记槽、NC/PF 空间和组件端口额度仍须分别检查。
具体额度分配留待 [N08：credit 分配方案](notice_me.md#n08credit-分配方案) 确认。

接口语义：

1. `tryRead/tryWrite` 登记整个逻辑访问，并用 `splitChannelRequest()` 生成事务。
   返回 ID 只表示被 `MemoryPort` 的有界请求表接收，不表示后端已接收所有事务；表满返回 `nullopt`，完全不改变调用方状态。
   登记不占 credit；额度暂时不足时可以保留 Pending 事务等待，但实际需要访问的方向不能配置为 0。
2. `submitPending()` 在活跃请求间轮询，只向对应组件、对应方向有可用 credit 的 Pending 事务发放。
   无额度时跳过该事务，继续检查其他可发请求。逻辑访问允许分批提交，不要求同时获得其全部子请求的额度。
3. 提交前建立稳定 token 和全部事务记录，将当前事务标为 `Submitting`，预扣对应 read_available 或 write_available 后调用 send。
   回调将该事务标为 `Complete` 并归还同一组件、同一方向的一个 credit，恰好执行一次，不在回调中发新请求。
   send 返回 true 时，仅将仍为 `Submitting` 的状态改成 `Accepted`，不能覆盖同步完成，也不能再扣一次 credit。
   后端返回 false 表示容量配置或 credit 记账约束被破坏，报告组件、方向及额度并终止仿真，不恢复为正常待重试事务。
   `RamulatorWrapper` 保留原有 bool 返回接口，MemoryPort 在首版按上述约定检查；已接受的事务不重复提交。
   `finished()` 要求每个事务都是 `Complete`。
4. 回调按 `TransferId + part_index` 定位，不能捕获 vector 元素引用。请求表预先定长，未完成请求不释放或移动。
   回调不复制功能数据，不调用组件调度，也不重入后端。
5. 组件观察完成后，先复制读数据或提交写数据，再 `release()`。
   同一个 TransferId 在完成前一直保存在其 NC、查询槽或 PF 槽内，不随周期重复登记。
   credit 已在各子请求的回调中归还，`release()` 不再归还；credit 可用不代表 NC/PF 空间已经释放。

例如 NM 有 2 个读 credit，一次逻辑读取拆成 3 个子请求，可以先提交 2 个，待其中一个完成归还额度后再提交第 3 个。
EM 仍可使用自己的读 credit；同一时刻 NM 的写请求由 NM 写 credit 独立控制。
每份 available 始终处于 `[0, limit]`；全部子请求完成后额度恢复至上限，不需要在窗口屏障处手动补回。

`MemoryPort` 自己不调用 `tick()`，也不保存数值副本。初期可以直接扫描有界请求表，
先使用简单轮询策略，不在这里展开总体设计第五部分中的 HBM 仲裁优化。

## 5. NM：Neurons Management

### 5.1 私有数据结构与接口

```cpp
// P：每次 CGRA 调用的神经元数；S：每个神经元的 states 分量数；K：Isyn 输入分量数。
// M：每窗口子步数；NC：神经元状态缓存；nc_slots：NC 组槽位数，每槽最多 P 个神经元。
// NM：神经元管理组件；IM：输入累加管理组件；Cycle 使用硬件周期，SimStep 使用仿真子步。
enum class NcPhase { Free, NeedRead, Reading, Ready, Computing, NeedWrite, Writing };

struct InFlightUpdate {
    Cycle ready_cycle;
    SimStep step;
    std::vector<NeuronResult> results;
};

struct NcSlot {
    NcPhase phase;
    NeuronRange neurons;
    std::uint32_t next_substep;
    std::optional<TransferId> transfer;
    std::optional<InFlightUpdate> update;
};

class IM;

class NM {
  public:
    NM(const PeConfig &, PeBackingStore &, MemoryPort &);
    void beginWindow(WindowId);
    void completeCycle(Cycle, const SpikeSink &);
    void issueCycle(Cycle, IM &);
    bool updatesComplete() const;
    bool writebacksComplete() const;

  private:
    std::span<Scalar> statesFor(std::size_t slot);
    void allocateReadSlots();
    void collectMemoryResults();
    void collectCgraResults(Cycle, const SpikeSink &);
    bool tryLaunch(std::size_t slot, Cycle, IM &);
    void issueMemoryRequests();

    const PeConfig &config_;
    PeBackingStore &store_;
    MemoryPort &memory_;
    WindowId window_;
    std::vector<Scalar> nc_states_;       // nc_slots × P × S，预先定长。
    std::vector<NcSlot> nc_slots_;
    std::vector<Scalar> input_scratch_;   // P × K，一次发射使用。
    std::size_t read_head_;
    std::size_t write_head_;
    LocalIndex next_neuron_;
    std::size_t completed_neurons_;
};
```

`NcSlot` 是控制元数据；真正的 NC 状态集中在 `nc_states_`，不需要每个神经元单独动态分配 vector。
`InFlightUpdate::results` 是模拟器保存的黑盒在途结果，不代表新增一份可被上层访问的硬件缓存。
它的数量受 NC 槽位约束；输出位置已经在对应槽位内预留，不设置独立的 CGRA 缓冲额度。

### 5.2 接口行为与状态推进

| 接口 | 行为 |
| --- | --- |
| `beginWindow(j)` | 上一窗口写回结束后，重置本窗口的读取范围和完成计数；不清掉尚未完成的请求来强行开始新窗口。 |
| `completeCycle(c, sink)` | 接收内存完成结果；在恰好 `ready_cycle==c` 时写入 CGRA 输出并调用 sink。 |
| `issueCycle(c, im)` | 分配/推进状态读取，选择最多一组 Ready 状态发射，并安排已完成数据块的写回。 |
| `updatesComplete()` | 本 PE 全部真实神经元完成 M 次更新，所有对应 spike 已交给 sink；可能还有状态写回。 |
| `writebacksComplete()` | 本窗口已全部分配和更新，所有状态已提交 backing store，NC 槽位与内存请求均已回收。 |

一个槽位的生命周期：

```text
# M：一个同步窗口包含的最小仿真步数；每个 NC（神经元状态缓存）槽保存一组神经元。
Free → NeedRead → Reading → Ready → Computing
                                 ↑         │
                                 └─────────┘ 尚有子步
                                           │ 完成 M 次更新
                                           ↓
                                      NeedWrite → Writing → Free
```

实现步骤：

1. 按循环队列分配连续的神经元组；末组记录实际 count，读取有效部分的状态。读事务全部完成后才进入 Ready。
2. `tryLaunch()` 先确认槽位为 Ready、没有在途更新或写回、结果覆写位置已保留，且当前周期未发射，准备好 P×K 的输入空间，
   再调用 IM 的原子消费接口。消费失败保持 Ready，不能先增加子步号或标记 Computing。
3. 消费成功后调用模型，将结果及 `ready_cycle=c+L` 保存在本槽位；发放步为 `jM+next_substep`。
   每槽同一时刻只有一个在途更新，下一子步必须等待返回状态。
4. 在结果到达周期覆写 NC，对实际发放的真实 lane 输出 `Spike{source_id_base+local_index, update.step}`。
   消息中的步号来自该 CGRA 任务，不能使用结果返回的硬件周期或调度器当前处理的子步。
   尚有子步则回到 Ready，否则进入 NeedWrite，并增加已完成神经元数。
5. **该数据块完成全部 M 个子步后即可写回**，不等待整个 PE 更新完成。写回过程中保持 NC 内容不变，完成后回收。

同周期先 complete 再 issue，因此一个槽位可以在 `c+L` 收到结果后发射下一子步，不额外强加一周期间隔。
循环队列按分配顺序回收，较早块未完成时允许后续块等待；不能仅因写指针移动就视为请求完成。

## 6. IM：Isyn Management

### 6.1 私有数据结构与接口

```cpp
// N：本 PE 的神经元数；M：每窗口子步数；K：每个神经元每子步的 Isyn 输入分量数。
// IM：输入累加管理组件；NM：神经元管理组件；B：最小仿真时间步；SimStep：以 B 为单位的绝对步号。
// buffers：2M 个步缓冲，当前窗口 M 个供消费，下一窗口 M 个供累加；每个缓冲独立标记步号。
enum class InputPhase { Open, Sealed };

struct InputStepBuffer {
    SimStep step;
    InputPhase phase;
    bool external_closed;
    std::vector<Scalar> values;          // N × K，仅对应 step。
    std::vector<std::uint8_t> consumed;  // N；每个神经元一次消费全部 K 分量。
    std::size_t consumed_neurons;
};

class IM {
  public:
    explicit IM(const PeConfig &);
    void initialize(std::span<const IsynContribution> initial_stimuli);
    void beginCycle(Cycle);

    bool tryAccumulate(const IsynContribution &);
    bool tryConsume(SimStep, NeuronRange, std::span<Scalar> output);
    void injectExternal(const IsynContribution &);
    void closeExternalInputs(WindowId);

    bool externalInputsClosed() const;   // 下一窗口的 M 个步缓冲均已关闭外部刺激。
    bool currentConsumed() const;        // 当前窗口的 M 个步缓冲均已全部消费。
    void sealNextWindow();              // 全局生产完成由调用方保证。
    void advanceWindow();

  private:
    InputStepBuffer &bufferFor(SimStep); // 按步号取模定位，并校验绝对步号。
    const InputStepBuffer &bufferFor(SimStep) const;
    std::size_t valueOffset(LocalIndex, std::uint8_t channel) const;

    const PeConfig &config_;
    WindowId current_window_;
    std::vector<InputStepBuffer> buffers_; // 固定 2M 项，各项数组预先定长。
    std::size_t additions_left_;
    std::size_t read_neurons_left_;
};
```

全部步缓冲归 IM 独占。绝对步号为 step 的输入定位到 `buffers_[step % buffers_.size()]`，
并要求该项的 `step` 标签一致；缓冲内数值下标为 `target × K + channel`，消费标记下标为 target。
首版固定 `2M` 个步缓冲，总数值容量为 `2 × M × N × K` 个 Scalar，即 `2 × M × N × K × 4` 字节，
另有 `2M×N` 个消费标记和各步控制状态；容量应计入配置报告。
窗口 j 期间同时保留步 `jM ... (j+2)M-1`，不同 NC 组可在当前窗口的不同子步推进，IM 按请求步号选择缓冲。
M=1 时自然只有两个步缓冲；M>1 时使用多个独立步缓冲，首版不按 Dmax 预分配全部未来输入。

### 6.2 接口语义

| 接口 | 成功行为 / 调用条件 |
| --- | --- |
| `initialize(initial_stimuli)` | 仅启动时调用：分配并清零 2M 个缓冲，步号为 `0 ... 2M-1`。按 due_step 注入窗口 0 的刺激，其 M 个缓冲标记 external_closed 并 Sealed；窗口 1 的 M 个缓冲为 Open，外部刺激尚未关闭。首版无负时间历史事件。 |
| `beginCycle(c)` | 每硬件周期调用一次，重置本周期读与累加额度。 |
| `tryAccumulate(x)` | 按 x.due_step 定位步缓冲，要求其属于下一窗口且为 Open；额度足够时立即执行一次加法并返回 true，额度不足返回 false 且完全不生效。 |
| `tryConsume(step, range, out)` | 按 step 定位步缓冲，要求其属于当前窗口且为 Sealed；out 长度必须为 `range.count×K`。整组读额度足够才复制、清零并标记消费，否则输出和内部状态均不变。 |
| `injectExternal(x)` | 按 x.due_step 加到下一窗口对应的 Open 步缓冲，不占突触累加额度；该缓冲已 close 或已 seal 时不能继续注入。 |
| `closeExternalInputs(w)` | 要求 w 是下一窗口，调度器承诺该窗口所有步的外部刺激全部注入，将其 M 个缓冲全部标记 external_closed；无刺激也要关闭。 |
| `externalInputsClosed()` / `currentConsumed()` | 分别检查下一窗口 M 个缓冲的外部刺激完成标记、当前窗口 M 个缓冲各自的 N 个神经元全部消费。 |
| `sealNextWindow()` | 仅在全局生产完成且下一窗口所有步的外部刺激已关闭后，将其 M 个缓冲全部变为 Sealed；不自行猜测 EM 是否还会产生贡献。 |
| `advanceWindow()` | 当前 M 个缓冲全部消费、下一 M 个已 seal 后，将旧当前缓冲的步号各增加 2M，清零并重置消费标记、计数、external_closed，改为 Open；current_window_ 加 1，原下一窗口缓冲原位保留供消费。 |

越出当前/下一窗口范围、缓冲步号标签不匹配、非法目标/通道、重复消费属于调用错误，直接诊断；`false` 只表示可重试的吞吐反压。
过期贡献不得写入同一取模槽位的新步数据；首版只在窗口屏障处复用旧当前窗口的 M 个缓冲，不因某个神经元或某一步消费结束就提前改标签。
未收到任何贡献的分量自然保持零。消费标记针对 `(绝对步号, 神经元)`，不能用数值等于零判断是否已消费。
每周期读、累加额度由全部步缓冲共用，不能按缓冲数放大吞吐量。外部刺激关闭不阻止 EM 继续累加，Sealed 后禁止继续累加及刺激注入，消费时仍可清零。

首版 `tryAccumulate()` 返回 true 时加法已经完成，没有隐藏的 IM 接收队列，因此 EM 可以立即推进交付位置。
同一地址的贡献按调用顺序串行执行，不会发生并发读改写丢更新。将来启用异步累加或 HI 时，需要增加在途状态和完成判断，
不能沿用“返回 true 就代表整个累加链路完成”的内部实现假设。

实现先完成步号定位、累加、消费，再加入每周期额度与屏障处缓冲复用；IM 不需要独立的内存访问逻辑即可跑通首版。
IM 的 read_credits/write_credits 首版可为 0；`beginCycle()` 只重置 SRAM 读取和累加的每周期额度，不重置 HBM credit。
该简化及后续 HI 访问需求记录在 [后续注意事项 N07](notice_me.md#n07im-的内存访问与溢出)。

## 7. EM：Event Management

### 7.1 私有数据结构

```cpp
// EM：事件管理组件；PF：突触预取缓冲区；一个事件对应一次 spike 的一个突触组。
// SimStep：绝对仿真子步号；WindowId：绝对窗口号；generation：槽位复用代次。
enum class LookupPhase { Free, NeedIndex, ReadingIndex, NeedGroup, ReadingGroup, HaveGroup };

struct LookupSlot {
    LookupPhase phase;
    Spike source;
    std::optional<TransferId> transfer;
    NeuronGroupsRecord groups;
    RecordIndex next_group;
    std::optional<SynapseGroupRecord> ready_group;
};

struct EventId {
    std::size_t slot;
    std::uint64_t generation;
};

struct GroupEvent {
    SimStep due_step;
    RecordIndex synapses_begin;
    std::uint64_t count;
    std::uint64_t next_prefetch;
    std::uint64_t delivered;
    std::size_t active_pf_slots;
    std::optional<std::size_t> next_in_bucket;
};

struct EventSlot {
    std::uint64_t generation;
    std::optional<GroupEvent> event;
};

struct TimeBucket {
    std::optional<WindowId> window;       // 绝对窗口号，防止环形复用串代。
    std::optional<std::size_t> first_event;
    std::size_t event_count;
};

enum class PfPhase { Free, Reading, Ready };

struct PfSlot {
    PfPhase phase;
    EventId event;
    std::uint64_t event_offset;
    std::size_t count;
    std::size_t delivered;
    std::optional<TransferId> transfer;
};
```

时间桶只存链表头和代次；事件放在一个定长槽位池中，通过下标链接，不使用每桶自动扩容的独立缓存。
桶数量先取 `max_delay_windows+1`。这只是随工作负载配置的逻辑范围，不是固定硬件最大延迟。
PF 槽对应固定大小的突触块，允许一个大组占用多个槽，或分多轮完成。

### 7.2 类接口

```cpp
// EM：事件管理组件；IM：输入累加管理组件；PE：处理单元；PF：突触预取缓冲区。
// pf_slots：PF 槽位数；synapses_per_pf_slot：每槽最多容纳的突触记录数。
class EM {
  public:
    EM(const PeConfig &, const PeBackingStore &, MemoryPort &);
    void beginWindow(WindowId);
    void receiveSpike(Spike);
    void completeCycle();
    void issueCycle(IM &);
    bool queriesDrained() const;
    bool nextWindowDrained() const;

  private:
    void startLookups();
    void advanceLookups();
    void enqueueGroup(const Spike &, const SynapseGroupRecord &);
    bool tryPrefetch(EventId);
    void deliverReady(IM &);
    std::span<Synapse> synapsesFor(std::size_t pf_slot);
    void retireEvent(EventId);

    const PeConfig &config_;
    const PeBackingStore &store_;
    MemoryPort &memory_;
    WindowId window_;
    std::vector<Spike> inbox_;           // 保存源 ID 与发放步号，由 vector 按需扩容。
    std::size_t inbox_read_;
    std::vector<LookupSlot> lookups_;
    std::vector<TimeBucket> buckets_;
    std::vector<EventSlot> events_;
    std::vector<std::size_t> free_events_;
    std::vector<PfSlot> pf_slots_;
    std::vector<Synapse> pf_values_;     // pf_slots × synapses_per_pf_slot。
};
```

`receiveSpike()` 保存完整的 Spike 消息，不同步完成 HBM 查表。
接收时直接向 `inbox_` 追加 `Spike`，由 `std::vector` 按需扩容，不按全局神经元数预分配最坏情况空间。
待处理范围为 `[inbox_read_, inbox_.size())`。开始查表时将记录按值复制到查询槽，
不保留指向 inbox 元素的引用或指针，避免后续追加扩容使引用失效。
窗口切换且查询排空后调用 `clear()` 并重置读取位置，保留容器容量供复用；首版不模拟该列表的硬件容量或入口反压。

### 7.3 查询、预取与交付计划

1. **接收与查表。** 从 inbox 分配查询槽，先通过 HBM 读取一条 neuron-groups。
   `group_count=0` 时释放查询槽；否则按组逐条读取描述。多个查询槽可同时工作，
   描述查询每周期最多推进 `metadata_records_per_cycle` 条；HaveGroup 中的记录创建事件后才能继续下一条。
2. **创建事件。** 每条组描述产生一个事件，`due_step=source.emitted_step+delay_windows×M`，放入对应绝对窗口的桶。
   零长度组直接跳过。首版不合并不同 spike 的事件；事件槽配置不足时报告容量错误。
3. **预取。** 仅选择 `due_step/M==window_+1` 的事件。先保留空 PF 槽，再登记该块 HBM 读取。
   `tryRead()` 因请求表满返回 `nullopt` 时撤销这次预留、不推进游标；登记成功后递增 `next_prefetch` 和 `active_pf_slots`。
   已登记请求等待 EM 读 credit，发给后端时必须被接受。
   一个 PF 槽在整个逻辑读取完成前保持 Reading，期间已占用完整容量。
4. **接收数据。** `completeCycle()` 将完成读取的连接表记录放入查询槽，或将突触记录复制到对应 PF 范围，
   然后释放 TransferId。PF 变为 Ready，槽位仍不能复用。
5. **交付。** EM 从 Ready PF 读取记录，解码 target，使用所属事件的 due_step 构造 `IsynContribution`，调用 IM 累加到对应步缓冲。
   每周期至多尝试交付 `deliveries_per_cycle` 条；IM 返回 true 才同时推进 PF 的交付位置和事件的 delivered。
   返回 false 保留该条及后续数据，下周期重试。PF 全块交付后才释放，并减少事件的 active_pf_slots。
6. **回收事件。** 只有 `next_prefetch==count`、`delivered==count` 且 `active_pf_slots==0` 才从桶中删除事件并回收槽位。
   多块可以乱序返回和交付，不能用最后一块返回或单个游标判断整组完成。

`queriesDrained()` 要求 inbox 已读完，所有查询槽已释放，没有待处理的索引、组描述、请求或尚未入桶的描述。
`nextWindowDrained()` 要求下一窗口的桶为空，且没有相关 PF 读取或待交付数据。
两者只是当前状态的查询；在 NM 仍能产生 spike 时，不能单凭这两个 true 提前封闭下一窗口。
更远窗口的事件保留在桶中，不影响本次屏障。

`beginWindow()` 只在全局切换时调用：确认旧窗口的查询/PF 已排空，复用 inbox，改变当前窗口号；
不得清空仍含未来事件的时间桶，也不得重置这些事件的处理进度。

## 8. PE 组合与全局调度

```cpp
// PE：处理单元；NM、IM、EM：神经元、输入累加、事件管理组件。
// Cycle：PE/CGRA 硬件周期号；WindowId：神经仿真窗口号，两者独立推进。
class PE {
  public:
    // config 和 store 由 Simulator 稳定持有。
    PE(const PeConfig &, PeBackingStore &, RamulatorWrapper &, std::size_t transfer_slots);

    MemoryPort memory;
    IM im;
    EM em;
    NM nm;
};

struct ClockConfig {
    std::uint64_t pe_period;
    std::uint64_t memory_period;          // 两者使用同一整数时间基准。
};

class Simulator {
  public:
    void initialize();
    void run(std::uint64_t window_count); // 初始化后调用一次，执行指定数量的完整窗口。

  private:
    void step();                        // 推进一个 PE 周期及期间的内存周期，不切换窗口。
    void broadcast(Spike);
    void advanceMemoryTo(Cycle);
    bool canAdvanceWindow() const;
    void advanceWindow();

    // 稳定持有配置、backing stores、RamulatorWrapper 及 vector<unique_ptr<PE>>。
    Cycle cycle_;
    WindowId window_;
};
```

PE 仅负责组合对象，不再添加一层 cache 或复制内部数据。`broadcast()` 对所有 PE 的 EM 各调用一次
`receiveSpike()`，原样传递源 ID 和发放步号，其中包括源 PE；本地投递不能再额外调用一次导致重复。

### 8.1 启动

初始化所有 backing store、容量和槽位，IM 分配 2M 个步缓冲，准备并封闭窗口 0 的 M 个缓冲。
NM、EM 开始窗口 0；调度器按 due_step 将窗口 1 的外部刺激注入对应步缓冲，然后调用 `closeExternalInputs(1)`。
无刺激也显式关闭。后续每次切换后，对新开放的下一窗口 M 个缓冲做同样操作。

### 8.2 每周期顺序

各阶段先遍历所有 PE，再进入下一阶段，避免按 PE 编号推进整个流水而引入额外的通信顺序差异。

1. 按时钟比例推进到当前 PE 周期边界之前及该边界上的内存 tick，回调仅记录完成并归还所属组件的对应 credit。
2. 所有 IM 执行 `beginCycle()`；所有 NM 执行 `completeCycle(c, broadcast)`，所有 EM 执行 `completeCycle()`。
3. 所有 EM 执行 `issueCycle(im)`，推进查表、预取和下一窗口累加。
4. 所有 NM 执行 `issueCycle(c, im)`，读取当前窗口输入、发射 CGRA、安排状态读写。
5. 所有 MemoryPort 执行 `submitPending()`。同步回调产生的完成标志由下一次 complete 阶段处理，不重入组件。
6. `run()` 在 step 返回后判断全局屏障；非最后窗口满足条件时整体切换，新窗口从下一 PE 周期开始发射。
   最后窗口按下节的终止流程处理。

时间零点先执行 PE 的初始周期，不预先增加内存 tick；之后按正整数周期间隔推进。
PE 与内存边界重合时先处理内存完成，再处理 PE。`cycle_` 不因窗口切换清零。
首版可把两个 period 都设为 1；A/B 是神经仿真步比例，不参与硬件时钟换算。

### 8.3 窗口切换条件

对所有 PE 同时检查：

```text
# NM：神经元管理；EM：事件管理；IM：输入累加管理；MemoryPort：本 PE 的 HBM 请求管理器。
NM.updatesComplete()
&& NM.writebacksComplete()
&& EM.queriesDrained()
&& EM.nextWindowDrained()
&& IM.currentConsumed()
&& IM.externalInputsClosed()
&& MemoryPort.empty()
```

其中查询排空显式覆盖尚未读到 delay 的 spike，不能只看已经生成的事件。
首版 IM 的成功累加是同步的，所以不再有额外的累加完成队列；引入异步路径时必须扩展此判定。
`MemoryPort.empty()` 是收尾检查，不能替代上述功能完成条件；它也不要求排空未到期的事件桶。

条件全部成立后，先对所有 IM 调用 `sealNextWindow()`，再调用 `advanceWindow()`，
最后令所有 NM、EM 开始新窗口。封闭与旧步缓冲复用期间不再产生旧窗口的 spike，原下一窗口的输入原位保留。

`run(window_count)` 从初始化的窗口 0 开始，恰好执行 window_count 个完整窗口；首版为一次性运行接口。
最后窗口仍等待上述完整屏障，然后只封闭下一窗口的 M 个步缓冲，不复用旧缓冲、不调用 NM/EM 的 beginWindow，也不注入再下一窗口的刺激。
因此最终状态对应完整窗口末尾，同时保留下一窗口输入与更远事件；仿真结束不要求未来事件桶为空。
window_count 为 0 时不执行更新。正常结束时确认内存请求全部结束，再调用后端 `finalize()`，不在同一后端上继续运行。

## 9. 首轮实现顺序与检查点

| 顺序 | 实现内容 | 关键检查 |
| --- | --- | --- |
| 1 | 公共记录、布局、backing store 与 MemoryPort | 队列容量与 credit 总额校验、credit 独立与耗尽等待、事务拆分与部分提交、意外拒绝报错、同步及乱序回调恰好归还一次 |
| 2 | IM 的 2M 个步缓冲、消费与屏障处复用 | K=1/4、M=1/多个子步、同目标不同步分离、空输入为零、重复消费、环回复用与过期步号检测 |
| 3 | NM 的 NC 循环队列与 CGRA 黑盒 | L>1、II=1、尾组、同神经元状态依赖、小 NC 多轮复用、c+L 固定输出 |
| 4 | EM 查询、时间桶和多个 PF 槽 | 同源不同子步、大组分块、返回乱序、IM 反压重试、长延迟跨桶复用 |
| 5 | 多 PE 瞬时广播与窗口屏障 | 本地仅一次、跨 PE 保留发放步号、同源不同子步发放的到期步正确、未完成索引阻止过屏障、未来事件允许保留 |

配置检查集中在装配阶段：单个 PE 内 N/S/M/P/K 的使用一致，所有 PE 的 M、global_neuron_count 与时间基准一致，
通道编号有效且各 PE 独占；各 PE 的连续神经元 ID 区间合法、互不重叠且覆盖全局范围。
各 PE 内四个 HBM 数据区互不重叠，记录数与 backing store 一致，
地址范围与布局步长合法，连接索引有效，延迟属于配置范围。
非空负载具有必要的 NC/查询/PF/内存请求槽及非零处理额度。
实际发起 HBM 读或写的组件，其对应方向 credit 上限必须为正；首版未启用的 EM 写、IM 读写方向可为 0。
每个控制器分别检查三个组件的读、写 credit 上限总和不超过其实际读、写队列容量，遵循第 4 节。
IM 的整组读取额度至少为 `min(P, N)`，避免配置造成永久无法发射。
IM 因吞吐额度不足返回 false、MemoryPort 因请求表满返回 nullopt 均为可重试反压；
后端 `send()` 返回 false 或超出首版未实现的溢出能力则明确报错终止。

函数实现优先使用 `std::span` 表达借用范围，使用 ranges/算法处理过滤、检查与复制；
从范围构建普通结果集合时优先 `std::ranges::to<std::vector>()`。
NC、PF、事件槽池及 IM 输入数组在初始化时定长，运行期间按槽位复用；
EM 的 spike 接收列表按需扩容，使用 `size()` 和读取游标管理有效范围。

后续启用 HE/HI 时，在 EM 增加事件驻留/回载状态，在 IM 增加输入块驻留、脏位、代次和串行读改写状态，
由同一个 MemoryPort 提交相应地址区的请求。该扩展保留现有跨组件交付与消费语义，具体容量和替换策略另行设计。

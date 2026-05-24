# FastDFS 动态 Group 负载反馈调度改进说明

## 1. 改进背景

本项目使用 FastDFS 作为文件存储系统，业务侧通过 FastDFS Client 向 Tracker 请求可用的 Storage 节点，然后将文件上传到对应 Storage。

FastDFS 的上传调度天然分为两层：

```text
先选择 group
再选择 group 内的 storage
```

其中 group 是 FastDFS 的副本边界。同一个 group 内的 Storage 会同步文件，不同 group 之间不互相同步。因此，上传文件落到哪个 group，会决定这个文件后续属于哪个副本组。

FastDFS 原生提供的上传调度策略比较简单，常见方式包括：

- 轮询选择 group
- 指定 group
- 选择剩余空间最大的 group
- group 内 storage server 轮询
- group 内按 storage server 优先级选择

原生策略的优点是简单稳定，但 `store_lookup=2` 只按 group 剩余空间选择目标 group，不能完整反映 group 当前整体负载。

例如，某个 group 的剩余空间最大，但该 group 内 Storage 当前上传任务较多、CPU 或内存压力较高。如果 Tracker 继续把新文件分配给这个 group，就可能造成该副本组短时间内压力集中，增加上传等待时间。

因此，本项目在 FastDFS 原生 group 调度基础上，增加 **动态 Group 负载反馈调度策略**。该策略不改变 group 内 Storage 轮询逻辑，而是在选择 group 时综合考虑 group 内各 Storage 的实时负载，选择更适合承接新文件的 group。

## 2. 改进目标

本次改进的目标是优化 Tracker 对 group 的选择逻辑，让上传请求在副本组维度分配得更加合理。

核心目标：

- 避免请求持续集中到单个剩余空间较大的 group
- 让上传任务优先分配给整体负载较低的 group
- 保留 group 内 storage 轮询，减少对 FastDFS 原生副本语义的影响
- 降低单个 group 的瞬时压力
- 减少上传接口平均响应时间和高分位响应时间
- 提高集群整体吞吐和资源利用率

## 3. 原生策略的问题

FastDFS 原生 `store_lookup=2` 的含义是：

```conf
store_lookup=2
```

即上传时选择剩余空间最大的 group。

该策略主要关注容量维度，但没有考虑：

- group 内 Storage 当前任务数
- group 内 Storage CPU 使用率
- group 内 Storage 内存使用率
- group 内 Storage 磁盘使用率差异
- 不同 group 的整体实时压力

因此在高并发上传场景下，可能出现以下问题：

- 某个 group 因为剩余空间较大，被持续分配任务
- 某个 group 空间略少，但整体负载更低，却没有被优先使用
- 调度结果更偏向存储容量，而不是实时处理压力
- 用户请求可能集中排队在少数 group 上

## 4. 总体设计

改进后的调度策略只改变第一层选择：

```text
原方案：动态选择 storage
改为：动态选择 group，group 内 storage 保持轮询
```

最终调度流程为：

```text
1. Tracker 根据 group 动态负载选择目标 group
2. Tracker 继续使用原生 store_server=0 在 group 内轮询选择 storage
```

推荐配置：

```conf
store_lookup=3
store_server=0
```

含义：

```text
store_lookup=3  # 动态 Group 负载反馈调度
store_server=0  # group 内 storage 继续轮询
```

这样设计的好处是：

- 贴合 FastDFS “先 group、后 storage” 的原生模型
- 不破坏 group 作为副本边界的语义
- 改动范围主要集中在 Tracker 的 group 选择逻辑
- group 内 Storage 轮询简单、稳定、可预测
- 比全局选择 Storage 更容易解释和验证

## 5. Group 负载评价指标

负载评价值 `group_load_score` 用来衡量一个 group 当前是否适合接收新文件。值越小，表示 group 越适合承接上传任务。

本项目先由每个 Storage 采集自身指标，再由 Tracker 在 group 维度聚合。

Storage 侧采集指标：

| 指标 | 含义 | 固定采集方式 |
| --- | --- | --- |
| 磁盘使用率 | 当前磁盘已使用比例 | 调用 `statfs` 获取 `f_blocks`、`f_bavail`、`f_frsize` |
| 任务使用率 | 当前连接/任务压力 | 当前实现复用 FastDFS 已有 `connection.current_count / connection.max_count`，可进一步扩展为精确 `current_tasks` |
| CPU 使用率 | 当前 CPU 繁忙程度 | 读取 `/proc/stat`，两次采样计算 CPU 使用率 |
| 内存使用率 | 当前内存使用比例 | 读取 `/proc/meminfo`，使用 `MemTotal` 和 `MemAvailable` 计算 |

Group 聚合方式：

| Group 指标 | 聚合方式 |
| --- | --- |
| `group_disk_usage_rate` | group 内 active storage 的平均磁盘使用率，必要时可结合最小剩余空间兜底 |
| `group_task_usage_rate` | group 内 active storage 的平均任务使用率 |
| `group_cpu_usage_rate` | group 内 active storage 的平均 CPU 使用率 |
| `group_memory_usage_rate` | group 内 active storage 的平均内存使用率 |

Group 负载评价公式：

```text
group_load_score =
    w1 * group_disk_usage_rate +
    w2 * group_task_usage_rate +
    w3 * group_cpu_usage_rate +
    w4 * group_memory_usage_rate
```

其中：

```text
w1 + w2 + w3 + w4 = 1
```

参考权重：

```text
磁盘使用率：0.30
任务使用率：0.40
CPU 使用率：0.20
内存使用率：0.10
```

上传场景中，任务数和磁盘使用率对用户等待时间影响更明显，因此权重更高。

## 6. Storage 指标上报

FastDFS 原生 Storage 已经会周期性向 Tracker 上报心跳和磁盘使用情况：

```conf
heart_beat_interval=30
stat_report_interval=60
```

其中：

- `heart_beat_interval`：Storage 心跳上报周期，默认 30 秒
- `stat_report_interval`：Storage 磁盘使用情况上报周期，默认 60 秒

本次改进在原有上报基础上扩展 Storage 的动态负载信息。Storage 周期性采集自身运行状态，并将指标上报给 Tracker。Tracker 不直接到 Storage 上执行 shell 命令，避免调度路径引入额外开销。

扩展后的上报信息包括：

```c
typedef struct {
    int64_t total_mb;
    int64_t free_mb;
    double disk_usage_rate;
    double task_usage_rate;
    double cpu_usage_rate;
    double memory_usage_rate;
    int current_tasks;
    int max_tasks;
    int64_t report_timestamp;
} StorageLoadStat;
```

### 6.1 任务使用率采集

任务使用率计算方式：

```text
task_usage_rate = current_tasks / max_tasks
```

其中：

- `current_tasks`：当前 Storage 正在处理的上传或文件操作任务数量
- `max_tasks`：Storage 能同时处理的最大任务数，来自 `storage.conf` 中的 `max_connections`

当前源码实现先复用 FastDFS 已有的连接统计字段作为任务压力近似值，即 `connection.current_count / connection.max_count`。后续如果希望进一步精确区分上传、下载、同步等任务，可以在 Storage 上传处理入口维护独立的 `current_tasks` 计数。

### 6.2 磁盘使用率采集

磁盘使用率固定通过 `statfs` 获取文件系统信息：

```text
total_disk = f_blocks * f_frsize
free_disk  = f_bavail * f_frsize
used_disk  = total_disk - free_disk
disk_usage_rate = used_disk / total_disk
```

该指标用于判断节点存储空间压力。

## 7. Tracker 调度流程

Tracker 收到上传请求后，执行以下流程：

1. 获取所有 group
2. 过滤没有 active storage 的 group
3. 过滤剩余空间低于保留阈值的 group
4. 检查 group 内 Storage 的动态负载信息是否可用
5. 聚合 group 内 active storage 的负载指标
6. 计算每个 group 的 `group_load_score`
7. 选择 `group_load_score` 最低的 group
8. 如果动态指标不可用或候选 group 为空，则回退到原生 `store_lookup` 策略
9. 选中 group 后，继续调用原生 `tracker_get_writable_storage(pStoreGroup)`
10. group 内 Storage 仍按 `store_server=0` 轮询

伪代码：

```c
Group *select_best_group_by_dynamic_load()
{
    Group *best_group = NULL;
    double best_score = 0;

    for each group in groups {
        if (group->active_count <= 0) {
            continue;
        }

        if (!group_has_reserved_space(group)) {
            continue;
        }

        if (!group_load_stat_available(group)) {
            continue;
        }

        group_load = aggregate_storage_load(group);

        score =
            W_DISK * group_load.disk_usage_rate +
            W_TASK * group_load.task_usage_rate +
            W_CPU  * group_load.cpu_usage_rate +
            W_MEM  * group_load.memory_usage_rate;

        if (best_group == NULL || score < best_score) {
            best_group = group;
            best_score = score;
        }
    }

    return best_group;
}

StorageServer *select_upload_storage()
{
    Group *group = select_best_group_by_dynamic_load();
    if (group == NULL) {
        group = select_group_by_native_strategy();
    }

    return tracker_get_writable_storage(group);
}
```

## 8. FastDFS 源码修改点

本次改进主要涉及 FastDFS 的 `storage` 和 `tracker` 两部分。

FastDFS 源码目录：

```text
fastdfs-6.06/storage/
fastdfs-6.06/tracker/
```

### 8.1 Storage 侧修改

Storage 侧负责采集本机指标，并周期性上报 Tracker。

涉及模块：

```text
fastdfs-6.06/storage/tracker_client_thread.c
fastdfs-6.06/storage/storage_global.c
fastdfs-6.06/storage/storage_func.c
```

主要修改：

- 增加 CPU、内存、任务数等指标采集逻辑
- 在上传任务开始和结束时维护 `current_tasks`
- 扩展 Storage 上报给 Tracker 的状态结构
- 将动态负载指标随心跳或状态包上报给 Tracker

### 8.2 Tracker 侧修改

Tracker 侧负责保存每个 Storage 的动态负载信息，并在上传调度时聚合成 group 负载评分。

涉及模块：

```text
fastdfs-6.06/tracker/tracker_service.c
fastdfs-6.06/tracker/tracker_mem.c
fastdfs-6.06/tracker/tracker_types.h
```

主要修改：

- 在 Storage 信息结构中增加动态负载字段
- 解析 Storage 上报的负载指标
- 保存指标上报时间戳
- 增加 `group_load_score` 计算逻辑
- 修改上传 group 选择逻辑
- 保留 group 内原生 Storage 轮询逻辑
- 保留原生策略作为兜底

原生最大剩余空间 group 选择逻辑主要位于：

```text
fastdfs-6.06/tracker/tracker_service.c
```

原生相关函数：

```c
tracker_find_max_free_space_group()
tracker_deal_service_query_storage()
```

改进后新增动态 group 调度逻辑：

```c
tracker_find_best_group_by_dynamic_load()
```

## 9. 策略兼容设计

为兼容 FastDFS 原有策略，本次改进保留原有配置项，并增加动态 Group 负载反馈策略开关。

示例：

```conf
store_lookup=3
store_server=0
```

含义：

```text
store_lookup=0  # round robin group
store_lookup=1  # specify group
store_lookup=2  # max free space group
store_lookup=3  # dynamic group load feedback

store_server=0  # group 内 storage round robin
```

当动态负载指标不可用、指标过期或候选 group 为空时，Tracker 自动回退到原生 group 选择策略，避免调度失败。

## 10. 稳定性处理

### 10.1 指标时效性

每次 Storage 上报负载指标时，Tracker 记录 `report_timestamp`。

如果某个 Storage 指标超过阈值未更新，则认为该节点负载指标失效：

```text
current_time - report_timestamp > load_stat_expire_time
```

如果一个 group 内可用指标不足，则该 group 不优先参与动态调度，必要时回退到原生策略。

### 10.2 防止调度抖动

为避免 group 评分轻微变化导致频繁切换，采用以下方式：

- 对 CPU、内存、任务数等指标做滑动平均
- 设置评分差阈值
- 对刚被选中的 group 增加短时间惩罚因子
- 在评分接近时保留一定轮询特性

### 10.3 任务计数准确性

Storage 内部维护当前任务数：

```text
上传开始：current_tasks++
上传结束：current_tasks--
上传失败：current_tasks--
```

这样比统计 TCP 连接数更接近真实任务压力。

## 11. 改进效果验证

可以通过三组压测对比改进效果。

### 11.1 原生轮询策略

```conf
store_lookup=0
store_server=0
```

观察：

- 各 group 上传请求数
- 各 Storage 上传请求数
- 平均响应时间
- P95 响应时间
- CPU、内存使用率

### 11.2 原生最大剩余空间策略

```conf
store_lookup=2
store_server=0
```

观察：

- 请求是否集中到剩余空间最大的 group
- group 间磁盘空间是否更均匀
- 高负载 group 响应时间是否上升

### 11.3 动态 Group 负载反馈策略

```conf
store_lookup=3
store_server=0
```

观察：

- 请求是否优先分配给整体低负载 group
- 高负载 group 是否被自动降权
- group 内 Storage 是否仍保持轮询分布
- 平均上传耗时是否下降
- P95/P99 响应时间是否下降
- 集群整体吞吐是否提升

## 12. 项目表述

面试中可以这样表达：

> FastDFS 的上传调度分为两层：先选择 group，再选择 group 内的 storage。group 是副本边界，同一个 group 内的 Storage 会同步文件，不同 group 不同步。因此我没有直接做全局 Storage 选择，而是在 group 选择阶段做动态负载反馈优化。
>
> FastDFS 原生的 `store_lookup=2` 主要按剩余空间选择 group，但剩余空间不能反映 group 当前真实压力。比如某个 group 空间最大，但组内 Storage 当前任务数、CPU 或内存压力较高，继续把新文件写入这个 group 会导致请求集中。
>
> 所以我扩展 Storage 的状态上报，让 Storage 周期性上报磁盘使用率、任务使用率、CPU、内存等指标。Tracker 将同一个 group 内 active Storage 的指标聚合成 group 负载评分，优先选择整体负载较低的 group。选中 group 后，仍然保留 FastDFS 原生的 group 内 Storage 轮询逻辑。这样既优化了副本组维度的负载分布，又不破坏 FastDFS 的 group 语义。

## 13. 一句话总结

本次改进将 FastDFS 原本偏静态的 group 选择策略，扩展为基于 group 内 Storage 实时运行状态的动态负载反馈调度；选中 group 后仍保留 group 内 Storage 轮询，使上传请求能够更合理地分配到整体负载较低的副本组。

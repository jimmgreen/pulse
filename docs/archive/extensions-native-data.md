# 原生数据视图（开发预览）

`pulse.ui.data` V1 将插件的数据提供者绑定到 `pulse.ui.model` 已创建的空 `virtualList`、`tree` 或 `dataTable`。它复用同一渲染器和数据控制器；插件通过版本化 C ABI 发布行，不需要创建第二套窗口或传输 JSON 文本。契约以 [ui_data.h](../../sdk/native/include/pulse/ui_data.h) 和 [机器接口清单](../../sdk/native/api-catalog.json) 为准。

本页说明作者契约和使用顺序，不代表完整宿主功能、交互、主题或 DPI 已验收。SDK 保持 developer preview；查询接口失败时，应将该功能显示为当前宿主不可用。`PulseUiModelNode::data_source`、`context.*` 绑定和 `PulseUiModelAction::data_operation` 仍未开放；该接口不改变这些字段的 `UNAVAILABLE` 结果。

## 创建与绑定

1. 使用 `pulse.ui.model` 创建页面。目标节点必须已经存在、类型为三种数据视图之一且没有行；选择状态初始值必须为空。`data_source` 保持空指针。
2. 在模型中声明筛选、排序、状态、错误、进度所需的状态键。绑定中配置的键必须存在、角色各不相同，也不能与选择键重叠；排序初始值使用 `nameAsc` 或 `nameDesc`。
3. 查询 `PULSE_UI_DATA_INTERFACE` / `PULSE_UI_DATA_VERSION`，注册提供者，再绑定目标节点。每个插件实例最多 16 个提供者、16 个绑定；句柄仅属于创建它的实例。
4. `bind` 本身不发起读取；显式请求 `PULSE_UI_DATA_REFRESH` 开始数据流。

下面片段假定页面中已经有名为 `results` 的空数据节点，以及 `filter`、`sort`、`status`、`error`、`progress` 状态键。请求和取消回调由插件实现，`user` 的生存期覆盖注册和所有工作线程：

```cpp
#include <pulse/sdk.hpp>
#include <pulse/ui_data.h>

void PULSE_CALL OnRequest(void* user, const PulseUiDataRequest* request);
void PULSE_CALL OnCancel(void* user, uint64_t request);

PulseStatus BindResults(const PulseHostV1& host, uint64_t panel, void* user,
                        PulseUiDataV1& data, uint64_t& provider) {
    using pulse::sdk::View;
    provider = 0;
    PulseStatus result = pulse::sdk::Host(host).Query(
        PULSE_UI_DATA_INTERFACE, PULSE_UI_DATA_VERSION, data);
    if (result != PULSE_OK) return result;
    result = data.register_provider(data.context, OnRequest, OnCancel, user,
                                    &provider);
    if (result != PULSE_OK) return result;
    PulseUiDataBinding binding{};
    binding.size = sizeof(binding);
    binding.page_size = 100;
    binding.filter_state = View("filter");
    binding.sort_state = View("sort");
    binding.status_state = View("status");
    binding.error_state = View("error");
    binding.progress_state = View("progress");
    result = data.bind(data.context, panel, View("results"), provider, &binding);
    if (result == PULSE_OK) {
        result = data.request(data.context, panel, View("results"),
                              PULSE_UI_DATA_REFRESH);
        if (result == PULSE_OK) return result;
        data.unbind(data.context, panel, View("results"));
    }
    data.unregister_provider(data.context, provider);
    provider = 0;
    return result;
}
```

配置或绑定失败时应检查返回值，按已经成功的步骤清理。成功更新模型后需要重新绑定；不要依赖旧请求或旧节点继续供数。使用 `pulse.ui` 的 `set_states` / `get_state` 操作同一页面状态，使用 `pulse.ui.events` 观察状态变化。

## 请求与工作线程

`PulseUiDataRequest` 提供本次请求 ID、提供者 ID、页面 ID、目标节点、筛选、排序、游标和页大小。请求回调与取消回调在页面所属 UI 线程执行，必须及时返回。回调中的字符串仅借用到回调返回；将任务交给插件自有工作线程前，复制所有需要的字符串和值。

工作线程可以调用 `publish`。它同步复制整页的借用字符串、行、子行和数组，不会调用插件代码，因此该调用返回后即可释放本次发布使用的临时存储。其他 `pulse.ui.data` 操作仍在 UI 线程执行；后台工作不要直接调用普通 UI 接口，也不要跨 ABI 抛异常。

请求/取消回调中不能重入数据控制操作，返回 `PULSE_ERROR_STATE`；需要变更注册、绑定或发起请求时，用 `pulse.dispatch` 延后。回调中仍可发布结果或通过 `pulse.ui.close` 关闭页面，宿主会先失效请求再处理释放。

`PULSE_OK` 表示该页已复制并排入队列，不表示已经显示。数据控制器处理队列后，配置状态键反映初始空态、加载、错误、刷新、分页和增量结果；过期版本、显示行数超限或队列溢出会进入错误状态。插件需要区分发布被接受与最终视图状态。

## 快照和分页

| 页字段 | 作者要求 |
|---|---|
| `size` | 设置为 `sizeof(PulseUiDataPage)` |
| `status` | `LOADING`、`READY` 或 `ERROR` |
| `mode` | 完整页使用 `SNAPSHOT`；增量使用 `DELTA` |
| `progress` | 0–100 |
| `revision` | `READY` 必须提供非空版本，最多 128 UTF-8 字节 |
| `next_cursor` | 存在下一页时提供，最多 256 UTF-8 字节 |
| `error` | `ERROR` 必须提供，最多 1024 UTF-8 字节 |
| `rows` / `row_count` | `PulseUiModelRow` 数组，所有借用数据在 `publish` 调用期间有效 |

`LOADING` / `ERROR` 不携带行、游标或增量。`READY` 可以发布零行快照以表达空结果。行 ID 在页内唯一、最多 64 UTF-8 字节；行命令必须是模型中声明过的原生命令 ID，继续通过 `pulse.ui.model::subscribe_command` 接收。

```cpp
// 在插件工作线程中；data 表和 request_id 在任务开始前已经复制。
PulseUiModelRow row{};
row.size = sizeof(row);
row.id = pulse::sdk::View("item-17");
row.text = {1, pulse::sdk::View("Example item")};
PulseUiDataPage page{};
page.size = sizeof(page);
page.status = PULSE_UI_DATA_READY;
page.mode = PULSE_UI_DATA_SNAPSHOT;
page.progress = 100;
page.revision = pulse::sdk::View("revision-1");
page.rows = &row;
page.row_count = 1;
PulseStatus result = data.publish(data.context, request_id, &page);
// result == PULSE_OK 仅表示已复制/排队；STATE 表示该请求已失效。
```

绑定页大小为 1–256，单个快照的顶层行数不超过该值；含子行总计最多 4096 行、深度最多 8，每行最多 16 个单元格，整页总 UTF-8 数据最多 65536 字节。视图累计显示最多 4096 行。

`REFRESH` 启动一个可持续发布的流。筛选或排序状态变化会自动取消旧流并发起新流，插件应使用新请求携带的筛选/排序。需要下一页时，在 UI 线程调用 `request(..., PULSE_UI_DATA_NEXT_PAGE)`；宿主取消旧流，用其游标发起新请求。下一页版本必须与已显示版本相同，不要将不同数据快照的分页结果拼接在一起。

## 增量

增量页的 `status` 必须是 `READY`，`mode` 为 `DELTA`，并在已经建立快照后使用。`base_revision` 必须等于当前显示版本，`revision` 表示应用后的版本。`rows` 按稳定 ID 插入或更新，`removed_ids` 删除顶层行，单次最多 256 个删除 ID。

树的后代发生变化时，提交其完整顶层祖先行及子树；删除也只按顶层 ID 进行。增量不允许用于下一页请求流，并清除分页游标。不要把增量当成绕过单页字节、行、层级或累计行上限的路径。

## 取消与销毁

`PULSE_UI_DATA_CANCEL`、成功更新模型、关闭页面、停止插件、注销提供者和解除绑定都会取消相关请求。请求在取消回调发生前已经失效，后续 `publish` 返回 `PULSE_ERROR_STATE`；取消回调只发送停止信号，不要在 UI 线程等待工作线程结束。

`unbind` 同时清空显示行和选择；提供者仍保留到 `unregister_provider` 或插件停止。插件可以在 UI 线程解除绑定或注销提供者，随后在合适的销毁阶段结束并 join 所有自有工作线程。宿主 context 有效到插件 `destroy` 返回，之后不要再发布。即使取消已经抑制结果，工作线程和回调所引用的插件内存仍需由插件自己保持有效。

每个控制器的待处理队列最多 64 页。溢出会使本次流失败，直到刷新；因此应合并高频变化、响应取消并控制生产速度，而不是无限堆积结果。

## 作者验证

SDK 的 `test_native_sdk.py` 校验源码包、公开头与清单一致性，并在完整作者编译模式检查 `PulseUiData*` 结构的打包对齐。清单和源码包通过不等于宿主行为通过；插件自己的定向检查至少应覆盖空结果、加载/错误、刷新、版本一致的分页、增量版本冲突、取消后的发布、快速筛选/排序，以及销毁时工作线程退出。视图验收证据由实际运行的宿主检查单独记录。

完整可编译示例见 [live-data](../../examples/native/live-data/README.md)：单工作线程管理 4096 条生成数据，后台筛选排序、128 条分页和稳定 ID 增量更新。它展示开发者自有数据链路；目录变化订阅服务仍需后续开放。

# 原生 DLL 示例

- [basic](basic/)：最小入口、接口查询封装与生命周期，不依赖私有头。
- [rename-panel](rename-panel/README.md)：由 DLL 创建原生页面，自己计算重命名规则，使用宿主准备计划、原生确认和任务服务。同一源码生成 ASCII 小写与大写两个 DLL。

- [inspector](inspector/README.md)：DLL 自行按扩展名聚合目录数据，用 DataTable 展示，并调用 stat、CAS 设置和自有审计接口。

- [reactive-panel](reactive-panel/README.md)：状态事件驱动 C++20 工作线程，通过 dispatch 返回 UI；含关键生命周期封装测试。

- [live-data](live-data/README.md)：DLL 自行生成 4096 条数据、后台筛选排序、128 条分页与稳定 ID delta；单工作线程演示取消、空结果和错误，不包含目录 watch。

这些开发预览示例可按原生包契约生成清单、打包和安装，并从正常命令面板挂载。显式 DLL 参数仅用于开发调试。示例不意味着 UI 挂载点或全部内部服务已经开放。参见 [SDK 契约](../../sdk/native/README.md)。

- [directory-browser](directory-browser/README.md)：只组合公开目录/context/UI 服务的 C++ 浏览参考包，含 256 条分页、watch 失效合并、snapshot entry 打开和异步生命周期测试。
- [file-operations](file-operations/README.md)：组合公开 UI 与 `pulse.fileops` 的当前选择复制/移动/回收站示例，展示原生确认、精确请求状态与取消。
- [directory-transfer](directory-transfer/README.md)：组合公开 UI、目录 V1 和文件操作 V2，将当前普通文件选择复制/移动到手工输入并打开的显式目标目录，展示目标关闭与请求寿命分离。
- [directory-workbench](directory-workbench/README.md)：独立 DLL 组合真实分组卡片、DataTable、目录 watch、CAS 设置与 `pulse.ui.mount`，可直接挂入窗口、侧栏或工作区；完整实际宿主验收入口见 [预览指南](../../sdk/native/README.md)。

# libevlite

轻量级 C 网络通信库，采用多 IO 线程、事件驱动和时间轮定时器，提供 TCP 服务端/客户端、外部描述符关联及 KCP 服务端。Linux 使用 epoll，Darwin / BSD 使用 kqueue；KCP 源码随库提供。

## 快速开始

以下命令适用于 Linux，需要 GCC、GNU Make 和 POSIX 线程支持：

```sh
make -j
gcc -Iinclude examples/echo.c -L. -levlite -pthread \
    -Wl,-rpath,'$ORIGIN' -o echo
./echo 9029
```

示例监听 `127.0.0.1:9029`，按 Ctrl+C 退出。在另一终端测试：

```sh
printf 'hello\n' | nc 127.0.0.1 9029
```

可选安装：`make install PREFIX=/your/install/prefix`。其他平台及编译配置见[构建与安装](docs/INSTALL)。

## 使用指南

### 创建与接入

```c
iolayer_t layer = iolayer_create(4, 4096, 20);
```

三个参数分别为 IO 线程数、会话池初始容量和时间轮刻度（毫秒）。线程数使用 `1..128`；容量不是在线会话上限。TCP 常用 `20ms` 刻度，KCP 常用 `8ms`，实际调度仍受回调耗时与系统负载影响。

| 接口 | 用途 |
| --- | --- |
| `iolayer_listen()` | TCP / KCP 服务端监听 |
| `iolayer_connect()` | 异步 TCP 连接，支持重连 |
| `iolayer_associate()` | 关联外部描述符，提供 `reattach` 时支持重连 |
| `iolayer_set_service()` | 注册会话回调和业务上下文 |

接入成功后，在接入回调中注册 `ioservice_t` 并返回 `0` 接受会话；非 `0` 拒绝。连接失败时，连接回调返回 `0` 表示继续重试。除可选的 `transform` 外，服务回调均须提供有效实现；完整示例见 [echo.c](examples/echo.c)。`NETWORK_UDP` 常量保留，但不提供通用 UDP 监听服务。

### 回调与线程

同一会话的回调通常在所属 IO 线程串行执行，不同线程可能并发；避免阻塞回调，共享数据由业务同步。会话级 `iolayer_set_*()` 只能在所属 IO 线程调用；网络层上下文及 transform 应在接入前配置。发送、广播、关闭及任务接口支持投递到目标线程，同线程发送或任务可能同步执行。

| 回调 | 约定 |
| --- | --- |
| `start` | 首次建立及重连成功时调用，返回值不决定关闭 |
| `process` | 返回已消费字节数，`0` 保留数据，负数请求结束 |
| `keepalive` | 返回 `0` 继续保活，非 `0` 停止保活 |
| `timeout` | 非 `0` 结束；`0` 时临时会话重新计时，可重连会话重连 |
| `error` | 临时会话结束；可重连会话返回 `0` 重连，否则结束 |
| `perform` | 自行回收 task，包括返回负数请求关闭的路径 |
| `shutdown` | 最终结束时释放业务上下文；`way=0` 主动，`way=1` 被动 |

TCP 需自行处理半包、粘包；`process` 返回值不得超过输入长度。接收缓冲区属于库，不能修改、释放或在回调后继续持有；异步处理时复制数据。拒绝接入时自行释放已分配的业务上下文。

### 发送与缓冲区

```c
iolayer_send(layer, sid, data, length, 0);
```

- `isfree=0`：库按需复制，调用返回后可复用或释放原缓冲区。
- `isfree!=0`：移交可由 `free()` 释放的缓冲区，调用失败也不要重复释放。
- 广播复制内容及 SID 数组；发送成功只表示接受操作，不代表对端收到或业务处理完成。
- `transform` 先执行网络层，再执行会话层。新缓冲区须可 `free()`；返回 `NULL` 表示失败且不得释放输入；会话改造不要原地修改共享广播数据。

### 超时与 KCP

`iolayer_set_timeout()`、`iolayer_set_keepalive()` 的单位为秒，负数用于关闭；`iolayer_set_sndqlimit()` 限制待发送消息条数，非正值不限制。TCP 超时随读事件重新安排；KCP 业务超时独立计时，收包不会自动续期。已设置超时的 TCP 常驻读事件改为负值后，旧超时仍可能重新生效。

KCP 监听的 `options` 传 `NULL` 使用默认配置；自定义时必须完整填写，字段不会逐项补默认值：`mtu=1000`、`minrto=30ms`、`sndwnd=rcvwnd=128`、`stream=1`、`resend=2`、`deadlink=30`、`interval=20ms`、`ntransfer=16`。

运行中设置 MTU 的有效范围为 `50..65507` 字节，返回 `0` 表示接受请求；待发送和待确认分片清空后生效。最后一次有效请求覆盖前次请求，设为当前 MTU 会取消待生效请求。

### 任务调度

| 接口 | 执行方式与回收 |
| --- | --- |
| `iolayer_invoke()` | 无 clone 时轮询选择一个线程；有 clone 时向其他线程投递副本，最后向 0 号线程投递原任务。任务由执行回调回收；返回 `-1` 表示网络线程组已停止，未投递任何任务，业务需自行释放 task |
| `iolayer_perform()` | 在指定会话所属线程执行；返回 `0` 表示已交付，进入 `perform` / `onPerform` 后由业务自行回收 task；返回 `-1` 表示失败，网络层已通过 `recycle` 回收，调用方不能再释放 |
| `iolayer_schedule()` | 在当前 IO 线程注册，外部调用则轮询选线程；间隔为正毫秒数，回调返回 `0` 继续，非 `0` 结束并由业务释放任务 |

`perform` / `onPerform` 一旦接收任务，就承担其后续回收责任，库不会在执行后再调用 `recycle`。所有处理分支都应完成回收，或明确将所有权转交给后续处理方，避免泄漏或重复释放。

周期任务不复制内容，首次执行需等待定时器。销毁时残留周期任务以 `interval=-1` 回调回收。

### 重连与退出

可重连会话发生错误或超时且业务同意继续时，约 `200ms` 后重连，保留未出队发送消息，成功后再次调用 `start()`。部分发送的队首消息会从头重发，业务需自行处理去重与确认。

`iolayer_shutdown()` 投递关闭请求，尝试排空发送队列；KCP 还会等待协议确认。关闭设有约 `10s` 超时，TCP 写事件重新注册可能延后截止时间；等待期间再次关闭会强制结束，错误和销毁也可能丢弃数据。

`iolayer_stop()` 停止新会话接入及业务接收处理，已有发送和 KCP 协议处理仍可继续。最终由管理线程调用 `iolayer_destroy()` 等待线程退出、释放资源；调用前停止外部提交，不能在自身 IO 回调中销毁。残留会话的 `shutdown` 和周期任务回收回调在销毁线程执行。

底层事件 API 见 [event.h](include/event.h)：事件集只能由所属线程操作，销毁事件前先解除注册，业务 fd 由应用关闭。

## 文档

- [测试说明](docs/README)：回归测试、故障注入、内存检查和压测入口。
- [历史性能数据](docs/benchmark.md)：早期测试环境和结果。
- 公开接口：[network.h](include/network.h)、[event.h](include/event.h)、[threads.h](include/threads.h)。

完整的接入与资源管理示例见 [examples/echo.c](examples/echo.c)。许可证见 [LICENSE](LICENSE)。

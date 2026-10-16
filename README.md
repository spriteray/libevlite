
# libevlite

轻量级 C 网络通信库，提供 TCP 服务端/客户端、外部描述符关联及 KCP 服务端。采用多 IO 线程、事件驱动和时间轮定时器；Linux 使用 epoll，Darwin / BSD 使用 kqueue。

## 快速开始

Linux 下使用 GCC、GNU Make 和 POSIX 线程：

```sh
make -j
gcc -Iinclude examples/echo.c -L. -levlite -pthread \
    -Wl,-rpath,'$ORIGIN' -o echo
./echo 9029
```

示例监听 `127.0.0.1:9029`，可用 `printf 'hello\n' | nc 127.0.0.1 9029` 测试，Ctrl+C 退出。安装使用 `make install PREFIX=/your/install/prefix`，其他配置见[构建与安装](docs/INSTALL)。

## 事件层

事件库可独立使用，声明见 [event.h](include/event.h)。每个事件集只由所属线程操作，fd 由应用关闭。

### 事件：`event_t`

`event_t` 描述单个事件关注的 fd、触发条件和回调。创建并配置后，将它加入事件集；不关联 fd 时可作为纯定时器使用。

| 接口                                        | 用途与关键参数                                                                      |
| ----------------------------------------- | ---------------------------------------------------------------------------- |
| `event_create()`                          | 分配事件，失败返回 `NULL`                                                             |
| `event_set(ev, fd, flags)`                | 设置 fd 和事件类型；读写用 `EV_READ/EV_WRITE`，持久监听加 `EV_PERSIST`；纯定时器用 `fd=-1, flags=0` |
| `event_set_callback(ev, cb, arg)`         | 回调签名 `cb(fd, events, arg)`；events 为本次触发原因，可包含 `EV_TIMEOUT`                   |
| `event_get_current(ev)`                   | 获取单调时间（毫秒）；已关联事件集时使用其缓存时间，不是 Unix 时间戳                                        |
| `event_reset(ev)`                         | 解除注册并清空配置，便于复用                                                               |
| `event_destroy(ev)`                       | 释放事件；须先解除注册，不关闭 fd                                                           |

默认事件触发前自动解除注册，继续监听需再次添加。`EV_PERSIST` 保留监听，配置了超时时还会自动重新计时。

### 事件集：`evsets_t`

`evsets_t` 管理 IO 监听和定时器。通过 add/del 注册或移除事件，循环调用 dispatch 等待并执行回调；所有操作应在所属线程完成。

| 接口                         | 用途与关键参数                            |
| -------------------------- | ---------------------------------- |
| `evsets_create(precision)` | 创建事件集；定时器刻度为正毫秒数，失败返回 `NULL`       |
| `evsets_add(sets, ev, tv)` | 添加监听，tv 单位毫秒，含义见下表                 |
| `evsets_del(sets, ev)`     | 取消 IO 监听、定时器和待执行回调，清除保存的超时间隔；不释放事件 |
| `evsets_dispatch(sets)`    | 等待并执行一轮事件，返回执行的事件数；可能阻塞            |
| `evsets_get_current(sets)` | 获取缓存的单调时间（毫秒），不是 Unix 时间戳          |
| `evsets_destroy(sets)`     | 销毁事件集；应用创建的事件仍由应用释放                |

| `evsets_add()` 的 tv | 含义 |
| --- | --- |
| `<0` | 不添加或修改超时；已有定时器保持不变 |
| `0` | 加入立即超时队列，在后续调度的立即队列处理阶段触发，不同步回调 |
| `>0` | 添加或替换超时，实际触发受时间轮刻度和调度影响 |

`evsets_add()` 返回位标记：`1` 表示添加 IO 监听，`2` 表示添加定时器，`3` 表示两者；`0` 表示未新增，负数表示失败。

取消超时、继续监听 IO：

```c
evsets_del(sets, ev);
evsets_add(sets, ev, -1);
```

删除会清除旧间隔，后续 IO 不会恢复旧定时器。注意它也取消已经激活但尚未执行的回调。会话接口的 `iolayer_set_timeout(-1)` 已封装这一步。

## 网络层

完整声明见 [network.h](include/network.h)，使用示例见 [echo.c](examples/echo.c)。

### 创建与接入

先创建网络层，再选择监听、主动连接或关联已有描述符。接入成功后以 SID 标识会话，并为它设置业务回调。

| 接口                                              | 用途与关键参数                                                               |
| ----------------------------------------------- | --------------------------------------------------------------------- |
| `iolayer_create(nthreads, nclients, precision)` | 创建网络层；线程数 `1..128`，`nclients` 为会话池初始容量，`precision` 为定时器的刻度（毫秒）        |
| `iolayer_listen()`                              | 监听 `TCP/KCP`；`type` 为 `NETWORK_TCP` 或 `NETWORK_KCP`，`host/port` 为本地地址 |
| `iolayer_connect()`                             | 异步连接 `TCP` 服务端；`host/port` 为远端地址                                      |
| `iolayer_associate()`                           | 关联外部 fd；`privdata` 保存业务资源，提供 `reattach` 时可重新关联                        |
| `iolayer_set_iocontext()`                       | 在接入前设置各 IO 线程的上下文，数组长度应等于线程数                                          |
| `iolayer_set_transform()`                       | 接入前设置网络层改造；允许原地改造，具体细节参考数据改造                                          |

`precision` 常用 `TCP` 20ms、`KCP` 8ms，不是实时精度保证。接入回调中设置 service，返回 `0` 接受会话、非 `0` 拒绝；连接失败回调返回 `0` 表示继续重试。除可选的 `transform` 外，service 回调均须提供有效实现。`NETWORK_UDP` 仅保留常量。

### 回调

通过 `iolayer_set_service()` 为会话注册 `ioservice_t` 回调和业务上下文，通常在接入成功回调中完成。

同一会话的回调在所属 IO 线程串行执行，不同线程可能并发。会话参数在所属 IO 线程设置；发送、广播、关闭及任务接口支持跨线程投递，同线程调用可能同步执行。避免阻塞回调，共享数据由业务同步。

| 回调          | 返回值及用途                                  |
| ----------- | --------------------------------------- |
| `start`     | 建立或重连成功；返回值不决定关闭                        |
| `process`   | 返回消费的字节数，`0` 保留数据，负数请求结束                |
| `transform` | 可选的发送前改造；**禁止原地改造**，具体细节参考数据改造          |
| `keepalive` | `0` 继续保活，非 `0` 停止保活                     |
| `timeout`   | 非 `0` 结束；`0` 时临时会话重新计时，可重连会话重连          |
| `error`     | 普通错误：临时会话结束；可重连会话返回 `0` 重连，否则结束；队列告警见下文 |
| `perform`   | 处理并自行回收任务；负数请求结束会话                      |
| `shutdown`  | 最终结束，释放业务上下文；`way=0` 主动，`way=1` 被动      |

TCP 需自行处理半包、粘包。`process` 返回值不能超过输入长度；接收缓冲区只读，不能释放或在回调后持有，异步处理需复制。拒绝接入时自行释放业务上下文。

### 发送

通过 SID 单播或向多个会话广播；发送成功表示操作已被接受，不保证对端收到。`isfree` 决定缓冲区所有权，转换回调用于发送前处理数据。

| 接口 | 用途与关键参数 |
| --- | --- |
| `iolayer_send(..., isfree)` | 向指定 SID 发送；`isfree=0` 按需复制，返回后可复用原缓冲区；非 `0` 移交可由 `free()` 释放的缓冲区，失败也不能重复释放 |
| `iolayer_broadcast()` | 向 SID 数组广播，复制数组及消息内容 |
| `iolayer_broadcast2()` | 向所有会话广播 |

发送前的改造与缓冲区所有权规则见下文「数据改造」。

### 数据改造

发送前先执行网络层改造，再执行会话层改造。回调形式为 `transform(context, buf, nbytes)`：`buf` 为输入缓冲区，`*nbytes` 为有效数据长度。

**网络层：允许原地改造。** 通过 `iolayer_set_transform()` 设置。可以修改输入内容、更新 `*nbytes`，并返回原指针；前提是输入可写，修改和返回的长度均不能超出原缓冲区范围。需要扩容时，应分配新缓冲区并返回。

**会话层：禁止原地改造。** 通过 `ioservice_t::transform` 设置。广播输入可能由多个接收者共享，不能修改输入内容；返回原指针表示不改造，必须保持 `*nbytes` 不变。需要改变内容或长度时，应分配独立的新缓冲区并返回。

两级改造均遵守以下约定：

- **返回新缓冲区**：使用独立的 `malloc()` 分配，并更新 `*nbytes`。由网络库负责 `free()`，返回后业务不能再修改或释放。
- **返回 `NULL`**：表示改造失败。不得释放输入；本次尚未交付的临时分配由回调自行回收。
- **禁止释放或 `realloc()` 输入**，禁止返回输入内部的偏移指针、栈内存或静态缓冲区作为新结果。

网络层原地改造还需注意：同线程 `iolayer_send(..., isfree=0)` 可能直接使用调用方缓冲区，因此调用方必须提供可写数据，不能传入字符串常量或只读内存。网络层回调可能被多个 IO 线程并发调用，共享上下文需同步。

### 任务调度

把业务任务交给 IO 线程执行：`invoke` 面向线程，`perform` 面向会话，`schedule` 用于周期任务。任务所有权随交付转移，执行或回收回调负责释放。

| 接口 | 行为与所有权 |
| --- | --- |
| `iolayer_invoke()` | `clone=NULL` 轮询投递到一个 IO 线程；提供 clone 时向其他线程投递副本、向线程 0 投递原任务。由 execute 回收；返回 `-1` 表示未投递，业务自行回收 |
| `iolayer_perform()` / `iolayer_performs()` | 向会话投递任务；交付后由 perform 回调回收，交付失败由 recycle 回收 |
| `iolayer_schedule()` | 周期任务，`interval` 为毫秒；回调返回 `0` 继续，非 `0` 结束并释放任务。销毁时以 `interval=-1` 回调回收 |

### 会话层设置

会话参数在所属 IO 线程设置，超时、窗口等参数建议在 `start` 中配置。

| 接口 | 参数含义 |
| --- | --- |
| `iolayer_set_timeout(seconds)` | 读超时，单位秒；负数禁用，`0` 按一个时间轮刻度计时 |
| `iolayer_set_keepalive(seconds)` | 保活回调间隔，单位秒；负数禁用，`0` 按一个刻度计时 |
| `iolayer_set_persist(onoff)` | 非 `0` 保持读事件注册，`0` 使用单次事件 |
| `iolayer_set_sndqlimit(queuelimit)` | 待发送消息条数告警阈值；非正数不检查，不拒绝入队 |
| `iolayer_set_mtu(mtu)` | 参数交给 KCP；返回 `0` 表示接受请求，待发送/待确认分片清空后应用 |
| `iolayer_set_wndsize(sndwnd, rcvwnd)` | KCP 发送、接收窗口，单位为分片数 |
| `iolayer_set_minrto(minrto)` | KCP 最小重传超时，单位毫秒 |

KCP 读超时由可交付的非空业务数据续期，纯 ACK、控制包和未完成重组的分片不续期。发送队列检查后仍达到阈值时回调 `error(eIOError_SendQueueLimit)`：返回 `0` 保留连接和队列，非 `0` 结束。TCP 注册写事件时附带约 1 秒检查超时，KCP 由协议定时器检查；建议首次发送前设置阈值。

MTU 多次设置以最后一次请求为准，设为当前值取消待生效请求。立即应用时透传 KCP 返回值；延后应用被拒绝时保留当前 MTU，分配失败重试。

### 退出与关闭

`shutdown` 结束指定会话，`stop` 停止接入与业务接收，`destroy` 最终回收网络层。销毁前应停止外部提交，由管理线程完成销毁。

| 接口 | 行为与所有权 |
| --- | --- |
| `iolayer_shutdown()` / `iolayer_shutdowns()` | 请求排空发送队列后关闭，KCP 还等待协议确认；再次关闭会强制结束 |
| `iolayer_stop()` | 停止接入与业务接收，已有发送和 KCP 协议处理可继续 |
| `iolayer_destroy()` | 等待线程退出并释放资源；调用前停止外部投递，不能在自身 IO 回调中调用 |

可重连会话重连时保留未出队消息，部分发送的队首消息会从头重发，业务需处理去重。关闭等待约 10 秒，TCP 重新注册写事件可能延后期限；强制关闭可能丢弃数据。销毁时残留任务和会话的清理回调在销毁线程执行。

## 更多文档

- [测试说明](docs/README)：回归测试、故障注入与压测入口。
- [构建与安装](docs/INSTALL)、[线程接口](include/threads.h)、[历史性能数据](docs/benchmark.md)。
- 完整示例：[examples/echo.c](examples/echo.c)。许可证：[LICENSE](LICENSE)。

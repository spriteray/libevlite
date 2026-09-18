# libevlite 网络通信库(Linux, Darwin, \*BSD)

轻量级的 C 语言网络通信库, 采用多 IO 线程 + 事件驱动模型, 支持 `TCP` / `UDP` / `KCP` 三种传输方式。

---

## 1. 基础事件模块( `include/event.h` )

**支持的IO复用机制: epoll和kqueue**

### 1.1 事件类型说明

- 读事件(`EV_READ`)
- 写事件(`EV_WRITE`)
- 超时事件(`EV_TIMEOUT`)
- 在三种事件类型的基础上, 支持事件驻留在事件集中的永久模式(`EV_PERSIST`)

> `EV_PERSIST` 可避免每次事件触发后重新注册, 在读事件上开启后极端场景下可提升 IO 性能约 40%,
> 对应会话级别的开关是 `iolayer_set_persist()`。

### 1.2 基于事件(`event_t`)的方法说明

| 方法 | 说明 |
|------|------|
| `event_create()` | 创建事件对象 |
| `event_set()` | 设置事件属性(描述符 + 事件类型) |
| `event_set_callback()` | 设置事件回调函数及上下文 |
| `event_destroy()` | 销毁事件对象 |

### 1.3 基于事件集(`evsets_t`)的方法说明

| 方法 | 说明 |
|------|------|
| `evsets_create()` | 创建事件集, 参数为时间精度 |
| `evsets_add()` | 向事件集中添加事件 |
| `evsets_del()` | 从事件集中删除事件 |
| `evsets_dispatch()` | 分发并处理事件(阻塞直到有事件就绪或超时) |
| `evsets_destroy()` | 销毁事件集 |
| `evsets_get_version()` | 获取库版本号 |

---

## 2. 网络线程模块( `include/threads.h` )

每个网络线程持有独立的事件集(`evsets_t`)和任务队列, 线程之间不共享会话。

### 2.1 线程模型

```
主线程
└── iothreads_start()
      ├── IO线程 1 → evsets_dispatch() 循环
      ├── IO线程 2 → evsets_dispatch() 循环
      ├── ...
      └── IO线程 N → evsets_dispatch() 循环
```

会话按 `sid` 哈希绑定到固定的 IO 线程, 生命周期内不迁移。
因此 `ioservice_t` 的各回调对同一会话而言总在同一线程中被触发, **会话内部状态无需加锁**。

### 2.2 跨线程任务投递

- 每个 IO 线程持有一个任务队列, 由自旋锁保护, 采用队列交换(swap)方式批量取出
- 仅当队列由空转为非空时才写 `eventfd` 唤醒目标线程, 避免高频唤醒带来的上下文切换
- 对应的对外接口是 `iolayer_invoke()` 和 `iolayer_perform()`

---

## 3. 通信模块( `include/networks.h` )

### 3.1 创建网络通信层 `iolayer_create()`

- `nthreads`: 指定网络线程的个数
- `nclients`: 推荐连接数
- `precision`: 事件集的时间精度(建议值 20ms)

### 3.2 设置网络通信层的方法(仅在IO线程中才能使用)

- 设置线程上下文: `iolayer_set_iocontext()`
- 设置网络层数据改造方法: `iolayer_set_transform()`

> `transform` 作用于该网络层的所有会话, 常用于统一的加密或压缩。
> 若只需针对个别会话做变换, 应在 `ioservice_t::transform` 上单独实现。
>
> 改造后返回的新缓冲区由网络层负责 `free()`, 因此 `transform` 的实现必须使用 `malloc` 分配;
> 若返回的指针与入参相同, 则视为未改造, 网络层不做释放。

### 3.3 监听端口/开启服务端 `iolayer_listen()`

- `type`: 网络类型, 支持 `TCP`, `UDP` 和 `KCP`
- `host`: 绑定的地址
- `port`: 监听的端口号
- `options`: 服务器全局参数(当前主要是 `KCP` 的参数配置)
- `callback`: 新会话创建成功的回调
- `context`: 上下文参数

### 3.4 连接远程服务/开启客户端 `iolayer_connect()`

发起异步连接, 连接结果(无论成功或失败)均通过回调通知。

回调返回值决定后续行为:

| 返回值 | 含义 |
|--------|------|
| `0` | 接受本次结果; 失败时由网络层按退避策略自动重连 |
| `-1` | 拒绝这条连接, 网络层关闭并清理 |
| `-2` | 放弃重连, 不再尝试 |

> 底层 `tcp_connect()` 失败时同样投递事件到网络层, 重连的定时与退避完全在网络层内部消化,
> 业务层只需在回调中表态是否继续。

### 3.5 关联描述符的读写事件 `iolayer_associate()`

把外部创建的描述符纳入网络层管理, 适用于已有 fd 需要复用事件循环的场景。

### 3.6 设置会话的方法(仅在IO线程中才能使用)

| 方法 | 说明 |
|------|------|
| `iolayer_set_timeout()` | 设置会话的超时时间 |
| `iolayer_set_keepalive()` | 设置会话的保活时间 |
| `iolayer_set_service()` | 设置会话的IO服务逻辑 |
| `iolayer_set_persist()` | 设置会话的读事件常驻事件集 |
| `iolayer_set_sndqlimit()` | 设置会话的发送队列长度限制 |
| `iolayer_set_mtu()` | 设置最大传输单元(仅限 `KCP` 有效) |
| `iolayer_set_minrto()` | 设置最小重传时间(仅限 `KCP` 有效) |
| `iolayer_set_wndsize()` | 设置发送接收窗口(仅限 `KCP` 有效) |

### 3.7 IO服务逻辑( `ioservice_t` )

会话的全部业务回调集中在这个结构体中, 通过 `iolayer_set_service()` 注册。

| 回调 | 触发时机 | 说明 |
|------|----------|------|
| `start()` | 会话建立完成 | 断线重连后同样触发, 适合放握手/注册逻辑 |
| `process()` | 收到数据 | 返回已消费的字节数, 未消费部分留在接收缓冲区 |
| `transform()` | 发送前 | 会话级别的数据改造, 可为空 |
| `timeout()` | 超时 | 返回非 0 关闭会话 |
| `keepalive()` | 保活周期到达 | 通常在此发送心跳包 |
| `error()` | 发生错误 | 返回非 0 关闭会话 |
| `perform()` | 任务投递到本会话 | 配合 `iolayer_perform()` 使用 |
| `shutdown()` | 会话关闭 | 释放业务资源的唯一时机 |

> `process()` 返回值是**已消费长度**而非处理结果。
> 半包场景下应返回已完整解析的部分, 剩余数据会与下次收到的数据拼接后再次回调。

### 3.8 发送数据 `iolayer_send()`

```c
int32_t iolayer_send( iolayer_t self, sid_t id,
                      const char * buf, size_t nbytes, int32_t isfree );
```

`isfree` 决定缓冲区的归属:

| 取值 | 语义 |
|------|------|
| `0` | 网络层拷贝一份, 调用方保留缓冲区所有权 |
| `1` | 所有权转移, 网络层发送完成后 `free()` |

### 3.9 广播数据 `iolayer_broadcast()`, `iolayer_broadcast2()`

- `iolayer_broadcast()`: 向指定的会话列表广播
- `iolayer_broadcast2()`: 向网络层内的全部会话广播

> 广播任务会被投递到每个 IO 线程, 由各线程并行遍历自己持有的会话。
> 广播规模较大时, 单条消息产生的系统调用次数为**会话总数**, 这是该模型的固有开销;
> 若需进一步优化, 应从减少会话数量或降低广播频率入手。

### 3.10 关闭会话 `iolayer_shutdown()`, `iolayer_shutdowns()`

- `iolayer_shutdown()`: 关闭单个会话
- `iolayer_shutdowns()`: 批量关闭会话列表

调用后不会立即销毁会话, 待发送队列冲刷完毕后才触发 `ioservice_t::shutdown()`。

### 3.11 提交任务到网络层 `iolayer_invoke()`, `iolayer_perform()`

- `iolayer_invoke()`: 提交任务到**所有** IO 线程, 需提供 `clone` 方法为每个线程复制一份
- `iolayer_perform()`: 提交任务到**指定会话**所在的 IO 线程, 触发 `ioservice_t::perform()`

`iolayer_perform()` 投递失败时会立即调用传入的 `recycle` 方法回收任务, 调用方无需重复释放。

### 3.12 停止服务 `iolayer_stop()`

- 停止对外提供接入服务, 不再接受新的连接;
- 停止所有连接的接收服务, 不再回调 `ioservice_t::process()`

已建立的连接仍可发送数据, 适用于服务器优雅停机的过渡阶段。

### 3.13 销毁网络层 `iolayer_destroy()`

停止所有 IO 线程并释放全部资源。调用前应确保已完成 `iolayer_stop()` 及必要的数据落地。

---

## 4. 典型用法

### 4.1 服务端

```c
// 1. 创建网络层
iolayer_t layer = iolayer_create( 4, 10000, 20, 1 );

// 2. 监听端口
iolayer_listen( layer, NETWORK_TCP, "0.0.0.0", 9029, NULL, on_accept, ctx );

// 3. 在 on_accept 中为新会话注册服务逻辑
static int32_t on_accept( void * context, void * iocontext,
                          sid_t id, const char * host, uint16_t port )
{
    ioservice_t service = {
        .start     = on_start,
        .process   = on_process,
        .timeout   = on_timeout,
        .keepalive = on_keepalive,
        .error     = on_error,
        .shutdown  = on_shutdown,
        .perform   = on_perform,
        .transform = NULL,
    };
    iolayer_set_service( layer, id, &service, session_create() );
    iolayer_set_timeout( layer, id, 30 );
    return 0;
}
```

### 4.2 拆包处理

```c
static ssize_t on_process( void * context, const char * buffer, size_t nbytes )
{
    size_t nprocess = 0;

    for ( ;; ) {
        size_t nleft = nbytes - nprocess;
        const char * p = buffer + nprocess;

        if ( nleft < sizeof( struct msghead ) ) break;

        struct msghead head;
        decode_head( p, &head );

        if ( nleft < sizeof( struct msghead ) + head.size ) break;

        handle_message( context, &head, p + sizeof( struct msghead ) );
        nprocess += sizeof( struct msghead ) + head.size;
    }

    return nprocess;   // 返回已消费长度, 剩余部分留待下次
}
```

---

## 5. 注意事项

- `ioservice_t` 的所有回调均在 IO 线程中触发, 业务对象若需被逻辑线程访问必须自行同步
- `iolayer_set_*()` 系列方法只能在 IO 线程中调用, 通常是在 `accept` / `connect` 回调内
- `shutdown()` 是释放业务资源的唯一时机, `error()` 和 `timeout()` 返回非 0 后仍会走到 `shutdown()`
- `transform()` 返回的新缓冲区必须由 `malloc` 分配, 网络层会在发送完成后 `free()`
- 会话与 IO 线程的绑定关系在生命周期内固定, 不要跨线程直接操作会话对象, 应通过 `iolayer_perform()` 投递

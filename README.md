# libevlite网络通信库(Linux, Darwin, *BSD)

1. #### 基础事件模块( include/event.h ), 支持的IO复用机制: epoll和kqueue

   - ##### 事件类型说明

      - 读事件(EV_READ)
      - 写事件(EV_WRITE)
      - 超时事件(EV_TIMEOUT)
      - 在三种事件类型的基础上, 支持事件驻留在事件集中的永久模式(EV_PERSIST)

   - ##### 基于事件(event_t)的方法说明

      - 设置事件属性 event_set()
      - 设置事件回调函数 event_set_callback()

   - ##### 基于事件集(evsets_t)的方法说明

      - 向事件集中添加事件 evsets_add()
      - 从事件集中删除事件 evsets_del()
      - 分发并处理事件 evsets_dispatch()

2. #### 网络线程模块( include/threads.h )

3. #### 通信模块( include/networks.h )

   - ##### 创建网络通信层 iolayer_create()

     - nthreads: 指定网络线程的个数
     - nclients: 推荐连接数
	 - precision: 事件集的时间精度(建议值8ms)
     - immediately: 数据是否会立刻推送到网络层，对实时性要求很高的场景, 建议设置为1

   - ##### 设置网络通信层的方法(仅在IO线程中才能使用)

     - 设置线程上下文: iolayer_set_iocontext()
     - 设置网络层数据改造方法: iolayer_set_transform()

   - ##### 监听端口/开启服务端 iolayer_listen()

     - type: 网络类型, 支持TCP, UDP和KCP
     - host: 绑定的地址
	 - port: 监听的端口号
	 - options: 服务器全局参数(当前主要是KCP的参数配置)
	 - callback: 新绘画创建成功的回调
	 - context: 上下文参数

   - ##### 连接远程服务/开启客户端 iolayer_connect()

   - ##### 关联描述符的读写事件 iolayer_associate()

   - ##### 设置会话的方法(仅在IO线程中才能使用)

     - 设置会话的超时时间 iolayer_set_timeout()
     - 设置会话的保活时间 iolayer_set_keepalive()
     - 设置会话的IO服务逻辑 iolayer_set_service()
     - 设置会话的读事件常驻事件集 iolayer_set_persist()
     - 设置会话的发送队列长度限制 iolayer_set_sndqlimit()
     - 设置会话的最大传输单元(仅限KCP有效) iolayer_set_mtu()
     - 设置会话的最小重传时间(仅限KCP有效) iolayer_set_minrto()
     - 设置会话的发送接收窗口(仅限KCP有效) iolayer_set_wndsize()

   - ##### 发送数据 iolayer_send()

   - ##### 广播数据 iolayer_broadcast(), iolayer_broadcast2()

   - ##### 关闭会话 iolayer_shutdown(), iolayer_shutdowns()

   - ##### 提交任务到网络层 iolayer_invoke(), iolayer_perform()

   - ##### 停止服务 iolayer_stop()

     - 停止对外提供接入服务, 不再接受新的连接;
     - 停止所有连接的接收服务, 不再回调ioservice_t::process()

   - ##### 销毁网络层 iolayer_destroy()


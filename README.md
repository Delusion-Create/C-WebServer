# C-WebServer

基于 Linux 的 **C++17 高性能 HTTP 服务器**：**MultiReactor(one loop per core)** + epoll 边缘触发 +
非阻塞 IO + sendfile 零拷贝，实现 HTTP/1.1 协议处理、Keep-Alive 长连接管理、定时器超时回收、
异步日志，并内置**静态文件服务**、**路由表**、**内存 KV 存储**与**令牌桶限流**。

> 极限压测真实可复现（4 核 Ubuntu 22.04，wrk 同机压测，全程 0 错误）：
> **keep-alive 最高 82,727 QPS**，**sendfile 大文件吞吐 3.91 GB/s**，详见[极限压测](#极限压测与瓶颈分析)。

## 功能特性

- **MultiReactor 架构**：每 CPU 核一个事件循环(one loop per thread)，监听 socket 使用
  **SO_REUSEPORT** 由内核负载均衡分发连接，4 核机器实测 QPS 较单 Reactor 提升 **约 12 倍**
- **无锁连接处理**：连接的所有读写/解析/响应都在所属 loop 线程内串行完成，无需互斥锁
- **HTTP/1.1 协议解析**：请求行/请求头/查询参数/请求体(Content-Length)，GET/POST/PUT/DELETE，
  完整返回 200/400/403/404/405/413/429/431/501 状态码
- **半包处理**：每连接输入缓冲跨事件累积，正确处理 TCP 粘包/拆包，支持 HTTP 流水线
- **静态文件服务**：`/static/*` 前缀路由，**sendfile 零拷贝**(实测 3.9GB/s)，MIME 识别，路径穿越防护
- **路由表**：精确路径、`:param` 参数路径、`/*` 前缀路径；路径存在但方法不匹配返回 405
- **内存 KV 存储**：`PUT/GET/DELETE /kv/:key`，互斥锁保护哈希表，O(1) 读写
- **令牌桶限流**：按客户端 IP 限流，超限返回 429，可对指定路由开启
- **Keep-Alive 长连接**：HTTP/1.1 默认长连接，30 秒空闲超时自动回收
- **定时器**：每 loop 独立；红黑树(multiset)有序 + 哈希表 O(1) 定位 + 惰性删除
- **异步日志**：单例 + 前后台双缓冲 + 等级过滤，队列满丢弃最旧、不阻塞业务线程
- **并发安全**：连接关闭幂等化(防 fd 复用误关)、半关闭(EPOLLRDHUP)正确读尽避免 RST、
  每 loop 私有 eventfd 优雅退出(epoll 唤醒回调是排他的, 不能共享唤醒 fd)

## 架构

MultiReactor：N 个事件循环(每核一个)，各自独立线程 + epoll 实例 + 定时器；
监听 socket 用 SO_REUSEPORT 绑定同一端口，内核按连接四元组哈希分发，天然多核负载均衡：

```
┌────────────── 主线程(装配与退出协调) ──────────────┐
│  创建 N 个 EventLoop(每核一个)并启动线程            │
│  SIGINT/SIGTERM → 向所有 loop 的 eventfd 广播       │
└────────────────────────────────────────────────────┘
┌────────── EventLoop × N (one loop per thread) ──────┐
│  listenFd(SO_REUSEPORT) + 连接fd 注册在同一个 epoll │
│  epoll_wait(超时=最近定时器)                        │
│   ├─ listenFd → accept, 连接归本 loop               │
│   ├─ 连接fd   → handleRead / handleWrite            │
│   └─ eventfd  → 收到退出信号, 优雅退出              │
│  连接状态(缓冲/解析/输出队列)只在所属 loop 线程访问  │
│  → 无锁; 文本响应直接 send, 静态文件走 sendfile      │
└─────────────────────────────────────────────────────┘
```

## 路由表

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | / | 首页文本 |
| GET | /about | 关于页 |
| GET | /static/* | 静态文件(sendfile 零拷贝, 路径穿越防护) |
| PUT | /kv/:key | 写入内存 KV(请求体为值) |
| GET | /kv/:key | 读取 KV 值 |
| DELETE | /kv/:key | 删除 KV 键 |
| GET | /kv | 列出全部键值 |
| POST | /echo | 回显请求体(示例: 开启令牌桶限流) |

新增接口只需在 `src/HttpServer.cpp` 的 `getRouter()` 中注册 handler。

## 目录结构

| 文件 | 职责 |
|------|------|
| src/main.cpp | 程序入口: 日志初始化、SIGPIPE 处理、启动服务器 |
| src/TcpServer.h/.cpp | MultiReactor 总控: 创建 EventLoop、信号广播、退出协调 |
| src/EventLoop.h/.cpp | 单事件循环: epoll + SO_REUSEPORT accept + 定时器 + 连接管理 |
| src/HttpServer.h/.cpp | 每连接对象: 输入缓冲/输出段队列、路由分发、sendfile、背压保护 |
| src/Router.h/.cpp | 路由表: 精确/参数/前缀匹配、405 判定、限流开关 |
| src/KVStore.h/.cpp | 内存键值存储(互斥锁 + 哈希表) |
| src/RateLimiter.h/.cpp | 令牌桶限流器(按 IP) |
| src/HttpRequest.h/.cpp | 无状态请求解析器: 报文尺寸限制、Keep-Alive 判定 |
| src/HttpResponse.h/.cpp | 响应组装: 状态码 + 标准原因短语 + 文件响应标记 |
| src/TcpTimer.h/.cpp | 定时器管理器: multiset + unordered_map + 惰性删除 |
| src/TimerNode.h/.cpp | 定时器节点 |
| src/Logger.h/.cpp | 异步日志(单例): 双缓冲、等级过滤 |
| src/Util.h/.cpp | SIGPIPE 忽略、信号处理安装 |
| static/ | 静态文件示例(index.html / test.txt / big.bin) |

## 构建与运行

```bash
# 构建
bash build.sh                      # 等价于 cmake -S . -B build && cmake --build build -j
# 运行(建议从项目根目录运行, 以便定位到 static/)
./out/webserver                    # 默认监听 127.0.0.1:8848
./out/webserver 0.0.0.0 8080       # 指定 IP 与端口
# 测试
curl -i http://127.0.0.1:8848/
curl -i http://127.0.0.1:8848/about
curl -i -X POST -d "hello" http://127.0.0.1:8848/echo
curl -i http://127.0.0.1:8848/static/index.html
curl -i -X PUT -d "hello" http://127.0.0.1:8848/kv/greeting
curl -i http://127.0.0.1:8848/kv/greeting
# 压测
wrk -t4 -c400 -d20s http://127.0.0.1:8848/
```

日志输出到可执行文件同目录的 server.log；发送 SIGINT/SIGTERM 优雅退出。

## 极限压测与瓶颈分析

### 环境与工具
- 环境：Ubuntu 22.04.5，4 vCPU / 3.8GB RAM，g++ 11.4.0 -O2；内核参数已调优
  (somaxconn=4096, ip_local_port_range=1024-65535, tcp_tw_reuse=1)
- 工具：wrk 4.1（同机压测，wrk 客户端与服务器共享 4 核）

### 结果（全程 0 错误）

| 场景 | 工具/参数 | QPS | 吞吐 | 说明 |
|------|-----------|-----|------|------|
| keep-alive 小响应 | wrk -t4 -c400 | **82,727** | 12.9 MB/s | 4 loop 峰值 |
| keep-alive | wrk -t4 -c1000 | 76,377 | 12.0 MB/s | 连接数增加 QPS 略降 |
| keep-alive | wrk -t4 -c2000 | 72,727 | 11.4 MB/s | |
| 短连接风暴(Connection: close) | wrk -t4 -c1000 | **17,109** | 2.2 MB/s | 每请求一次 TCP 建连/拆除 |
| 静态 1MB 文件(sendfile) | wrk -t4 -c200 | 3,999 | **3.91 GB/s** | 零拷贝极限吞吐 |

### 瓶颈定位（top -H 线程级采样）

压测时 CPU 分布：**idle = 0%（已 100% 打满）**，us(用户态)≈32%，**sy(内核态)≈47%**，
si(软中断)≈21%。线程采样显示 **4 个 EventLoop 线程各占 53%~60% CPU 并行工作**
(wrk 客户端 4 线程与服务器共享 4 核，故各约半核)，main 与日志线程接近 0%。

结论——这台机器的极限瓶颈是 **CPU**，具体构成：
1. **系统调用开销(sy ≈ 47%)**：每个请求需 recv/send/epoll_wait 等多次 syscall，内核态开销大于用户态
2. **软中断(si ≈ 21%)**：同机回环压测的固有成本(virtio 网络栈 + softirq)，真实网卡 + RSS 多队列可分摊
3. **多核利用**：MultiReactor 已让 4 核全部参与(单 Reactor 时仅约 1.25 核)，这是最大的已解决问题

### 已做的针对性优化

| 优化 | 效果 |
|------|------|
| 单 Reactor → MultiReactor(one loop per core + SO_REUSEPORT) | QPS 约 7,000 → **82,727(约 12 倍)**, 4 核全利用 |
| 连接状态去锁(one loop one thread) | 消除线程池互斥锁竞争 |
| 连接级日志降为 DEBUG | 短连接风暴 QPS 12,627 → **17,109(+35%)**, 消除日志线程瓶颈 |
| recv 缓冲 4KB → 16KB | 减少 ET 读尽循环的 recv syscall 次数 |
| sendfile 零拷贝 | 1MB 文件吞吐 **3.91 GB/s**(内核态 DMA, 用户态零拷贝) |
| 内核参数调优 | somaxconn/端口范围/tw_reuse, 支撑万级并发连接 |

### 剩余瓶颈与后续方向

- **syscall 开销**：当前每次请求 2~4 次系统调用，可进一步用 **io_uring** 批量提交 recv/send 减少上下文切换
- **软中断**：同机压测固有；生产环境用物理网卡多队列(RSS) + 高 MTU/GSO 卸载分摊
- **短连接场景**：QPS 受 TCP 建连/拆除成本限制(TIME_WAIT/三次握手)，可用连接池或 HTTP/2 多路复用缓解
- 暂不支持 chunked 传输编码、HTTPS/TLS；KV 无持久化；限流为进程内状态

## 关键设计(面试点)

- **one loop per thread**：每核一个事件循环，连接归属固定 loop，读写解析都在单线程内完成
  → 连接状态无锁，避免多线程竞争；配合 SO_REUSEPORT 实现内核级连接分发
- **半包/粘包**：连接级输入缓冲 + Content-Length 判定完整请求，支持流水线与尺寸上限
- **半关闭处理**：EPOLLRDHUP 不直接 close(接收队列可能仍有数据, 直接 close 触发 RST)，
  由读操作 recv 到 0 后干净关闭——压测中真实发现并修复
- **epoll 唤醒的排他性**：epoll 在 fd 上的等待回调是 EXCLUSIVE，共享 eventfd 只唤醒一个
  loop(实测只有 loop[0] 退出)，故每个 loop 持私有 eventfd，信号处理器广播写入
- **关闭幂等化**：close 前先移除连接表，重复关闭直接返回，杜绝 fd 复用误关
- **输出段队列**：文本段/文件段(sendfile)按序发送，流水线下顺序正确，队列上限背压保护
- **sendfile 零拷贝**：文件内容内核态 DMA 直发网卡，1MB 文件 3.9GB/s
- **令牌桶限流**：桶容量控突发 + 固定速率补令牌，O(1) 判断，按 IP 隔离
- **优雅退出**：每 loop 私有 eventfd 注册进 epoll，SIGINT/SIGTERM 广播唤醒所有 loop
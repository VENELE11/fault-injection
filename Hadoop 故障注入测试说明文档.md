# **Hadoop 故障注入测试说明文档**

## **一、测试环境信息**

- **Master**：192.168.1.10
- **Slave1**：192.168.1.11
- **Slave2**：192.168.1.12
- **故障注入工具路径**：/root/hadoop-fi/hadoop_injector
- **网络接口**：eth1

## **二、已完成测试列表（共 16 项）**

| **#** | **功能**          | **命令**        | **用法示例**                               | **预期结果**                                                 | **状态** |
| ----- | ----------------- | --------------- | ------------------------------------------ | ------------------------------------------------------------ | -------- |
| 1     | Map 任务崩溃      | crash-map       | ./hadoop_injector crash-map slave1         | 随机杀死一个 Map 任务进程，YARN 日志显示 Container killed on request. Exit code is 137 | ✅        |
| 2     | Reduce 任务崩溃   | crash-reduce    | ./hadoop_injector crash-reduce slave1      | 随机杀死一个 Reduce 任务进程，任务会被 YARN 重新调度         | ✅        |
| 3     | 网络延迟          | delay           | ./hadoop_injector delay slave1 200         | 在 slave1 的 eth1 网卡注入约 200ms 网络延迟                  | ✅        |
| 4     | 网络丢包          | loss            | ./hadoop_injector loss slave1 10           | 在 slave1 注入 10% 丢包率                                    | ✅        |
| 5     | CPU 压力          | cpu-stress      | ./hadoop_injector cpu-stress slave1 10 2   | 在 slave1 启动 2 个线程消耗 CPU，持续 10 秒                  | ✅        |
| 6     | 内存压力          | mem-stress      | ./hadoop_injector mem-stress slave1 512    | 在 slave1 占用 512MB 内存                                    | ✅        |
| 7     | NodeManager 挂起  | hang nm         | ./hadoop_injector hang nm                  | NodeManager 进程状态变为 [STOPPED]                           | ✅        |
| 8     | DataNode 崩溃     | crash dn        | ./hadoop_injector crash dn                 | DataNode 进程被杀死，节点失联                                | ✅        |
| 9     | HDFS 安全模式开启 | hdfs-safe enter | ./hadoop_injector hdfs-safe enter          | HDFS 进入 Safe Mode，只读                                    | ✅        |
| 10    | HDFS 安全模式关闭 | hdfs-safe leave | ./hadoop_injector hdfs-safe leave          | HDFS 退出 Safe Mode，恢复读写                                | ✅        |
| 11    | 网络乱序          | reorder         | ./hadoop_injector reorder slave1 30        | 在 eth1 注入 30% 网络乱序                                    | ✅        |
| 12    | 网络隔离          | isolate         | ./hadoop_injector isolate slave1           | Hadoop 相关端口被 iptables DROP                              | ✅        |
| 13    | 磁盘填充          | disk-fill       | ./hadoop_injector disk-fill slave1 100     | 生成 100MB 磁盘占用文件                                      | ✅        |
| 14    | YARN 节点不健康   | yarn-unhealthy  | ./hadoop_injector yarn-unhealthy slave1 on | NodeManager 被标记为 UNHEALTHY                               | ✅        |
| 15    | 心跳超时          | heartbeat       | ./hadoop_injector heartbeat slave1 5000    | 心跳/网络延迟约 5000ms，触发超时                             | ✅        |
| 16    | IO 限速           | io-slow         | ./hadoop_injector io-slow slave1 on        | 节点 IO 吞吐被限制，磁盘写入速率约 1MB/s                     | ✅        |

## **三、MR 作业提交说明（测试前置步骤）**

部分故障注入（如 crash-map、crash-reduce）**依赖正在运行的 MapReduce 作业**，在执行相关测试前，需要先提交一个 MR 示例作业。

### **1. 示例 MR 作业提交命令（WordCount）**

```
hadoop jar $HADOOP_HOME/share/hadoop/mapreduce/hadoop-mapreduce-examples-*.jar \
  wordcount /test/input /test/output_$(date +%s)
```

### **2. 说明**

- /test/input：HDFS 中已存在的输入目录（需提前准备测试文件）
- /test/output_$(date +%s)：动态生成的输出目录，避免目录已存在导致作业失败
- 作业提交成功后，可通过 YARN Web UI 或 yarn application -list 确认作业正在运行

> ⚠️ **注意**：必须在 Map / Reduce 任务仍在运行期间执行 crash-map 或 crash-reduce 注入，否则不会命中目标进程。

### **3. MR 作业 + 故障注入完整示例流程**

```
# 1. 提交一个 MR 作业（后台运行）
hadoop jar $HADOOP_HOME/share/hadoop/mapreduce/hadoop-mapreduce-examples-*.jar pi 50 10000 &

# 2. 等待作业启动并产生 Map / Reduce 任务
sleep 5

# 3. 注入故障示例一：杀死 Map 任务
./hadoop_injector crash-map slave1

# 4. 注入故障示例二：网络延迟（500ms）
./hadoop_injector delay slave1 500

# 5. 观察作业与集群状态
yarn application -list
```

该流程可用于验证 **Map / Reduce 任务失败后的自动重试、节点容错与作业恢复能力**。

## **四、关键测试说明补充**

### **1. 网络类故障验证方式**

- 使用 ping 验证延迟与心跳超时效果
- 使用 tc qdisc show dev eth1 验证 delay / loss / reorder 注入结果
- 使用 iptables -L -n 验证 isolate 隔离规则

### **2. 资源类故障验证方式**

- **CPU**：top -bn1 | head -5
- **内存**：检查 /tmp/hadoop_mem_stress 文件大小
- **磁盘**：检查 /tmp/disk_hog 文件大小

### **3. 进程与状态类故障**

- NodeManager / DataNode 状态通过 ./hadoop_injector list 或 YARN / HDFS Web UI 验证
- YARN 不健康节点通过 /tmp/yarn_node_health_check 文件模拟

## **五、IO 限速测试说明**

### **1. 在 slave1 上启用 IO 限速**

```
./hadoop_injector io-slow slave1 on
```

### **2. 登录 slave1 手动测试（需将 shell 加入限速 cgroup）**

```
ssh root@192.168.1.11

echo $$ > /sys/fs/cgroup/io_limited/cgroup.procs

dd if=/dev/zero of=/root/test_io bs=1M count=10 oflag=direct
```

**预期结果**：磁盘写入速度约为 **1MB/s**。

### **3. 关闭 slave1 IO 限速**

```
./hadoop_injector io-slow slave1 off
```

### **4. 在 slave2 上进行同样测试（可选）**

```
./hadoop_injector io-slow slave2 on

ssh root@192.168.1.12

echo $$ > /sys/fs/cgroup/io_limited/cgroup.procs

dd if=/dev/zero of=/root/test_io bs=1M count=10 oflag=direct

./hadoop_injector io-slow slave2 off
```

### **5. 查看 IO 限速状态（在 Slave 上执行）**

```
# 查看限速规则
cat /sys/fs/cgroup/io_limited/io.max

# 查看已加入限速组的进程
cat /sys/fs/cgroup/io_limited/cgroup.procs
```

### **6. 注意事项与原理说明**

> **cgroup 的 IO 限速是基于进程的**：只有被加入限速 cgroup 的进程才会受到 IO 限制。

当执行以下命令时：

```
./hadoop_injector io-slow slave1 on
```

工具内部会自动完成以下操作：

1. 创建一个名为 **io_limited** 的 cgroup 限速组
2. 将该 cgroup 的 **IO 上限设置为约 1MB/s**
3. **自动把 slave1 上的所有 Java 进程加入该 cgroup**，也就是 Hadoop 相关组件，包括：
   - DataNode
   - NodeManager
   - 其他 Hadoop / YARN Java 进程

因此，在开启 IO 限速后，**Hadoop 本身已经处于被限速状态**，无需额外操作。

### **为什么手动执行** **dd**还需要**echo $$** ？

当我们希望通过 dd 命令**手动验证 IO 限速效果**时，需要注意：

- 新登录的 shell **默认不属于任何限速 cgroup**
- 如果直接执行 dd，该进程不会受到 IO 限制，测试结果不准确

因此，必须先将**当前测试用的 shell 进程**加入限速组：

```
echo $$ > /sys/fs/cgroup/io_limited/cgroup.procs
```

其中：

- $$ 表示**当前 shell 的进程号（PID）**
- 执行后，该 shell 中启动的所有子进程（包括 dd）都会继承该 cgroup 限速规则

这也是为什么 **每次重新打开终端后，都需要重新执行一次 echo $$**。

## **六、清理与恢复命令汇总**

| **故障类型**     | **清理 / 恢复命令**                         |
| ---------------- | ------------------------------------------- |
| 网络延迟         | ./hadoop_injector delay-clear               |
| 网络丢包         | ./hadoop_injector loss-clear                |
| 网络乱序         | ./hadoop_injector reorder-clear             |
| 网络隔离         | ./hadoop_injector isolate-clear             |
| 内存压力         | ./hadoop_injector mem-stress-clear          |
| 磁盘填充         | ./hadoop_injector disk-fill-clear           |
| NodeManager 恢复 | ./hadoop_injector resume nm                 |
| DataNode 恢复    | hdfs --daemon start datanode                |
| YARN 不健康恢复  | ./hadoop_injector yarn-unhealthy slave1 off |

## **五、测试结论**

- 本次共执行 **15 项 Hadoop / YARN / HDFS 故障注入测试**
- 覆盖 **计算、存储、网络、资源、心跳、节点健康状态** 等关键故障场景
- 所有测试均按预期生效并可成功清理恢复
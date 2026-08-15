# Zephyr 进程模型 - 当前状态和快速开始指南

## 📊 当前实现状态

### ✅ 已完成
- [x] 基础进程结构 (`struct z_process`)
- [x] 进程创建/销毁 (`process_create`, `process_exit`)
- [x] 进程表管理（静态分配，16 个进程）
- [x] FD 表（固定 16 个 FD）
- [x] 环境变量（动态链表）
- [x] 进程-线程关联
- [x] new_task() API（Embox 兼容）
- [x] waitpid() 实现
- [x] fork() 基础实现
- [x] ANL loader（动态加载模块）

### ⚠️ 已知问题（需修复）
- [ ] **严重**: FD 表浅复制导致多进程共享对象
- [ ] **严重**: 无 FD 引用计数和 close() 回调
- [ ] **重要**: 孤儿进程 parent 指针悬垂
- [ ] **重要**: 缺少资源模块化框架
- [ ] 无 vfork 支持
- [ ] 环境变量 malloc 碎片化

---

## 🎯 核心差异：Embox vs 当前 Zephyr

| 特性 | Embox | Zephyr (当前) | 差距 |
|------|-------|--------------|------|
| **FD 管理** | 引用计数 + close() 回调 | 直接指针，无引用计数 | 🔴 严重 |
| **fork 复制** | 深度复制（增加引用） | 浅复制（memcpy） | 🔴 严重 |
| **资源框架** | 模块化插件系统 | 硬编码在结构体中 | 🟡 中等 |
| **孤儿处理** | 自动重新关联到 init | 不处理 | 🟡 中等 |
| **vfork** | 完整支持 | 不支持 | 🟢 次要 |
| **环境变量** | 预分配存储池 | 动态 malloc | 🟢 次要 |

---

## 🚀 快速开始：验证当前实现

### 1. 编译 RocketPi ANL Loader

```bash
cd /opt/Program/UCAS/OneWo-zepLinux

# 构建
west build -p always -b rocket_pi -d build-rocket-pi-anl \
    zephyr/samples/ansilic/rocket_pi_shell_process

# 烧录
west flash -d build-rocket-pi-anl --runner openocd
```

### 2. 测试 fork_demo

```bash
# 编译 fork_demo.anl (ARM Cortex-M4)
~/zephyr-sdk-0.17.4/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc \
    -c -O1 -mthumb -mcpu=cortex-m3 \
    -fno-builtin -ffreestanding -mlong-calls \
    -o /tmp/fork_demo.o tools/examples/fork_demo.c

python3 tools/anl_link.py /tmp/fork_demo.o /tmp/fork_demo.anl --entry main

# 上传到板子
sudo python3 tools/upload/upload_hex.py /dev/ttyACM0 115200 fork_demo /tmp/fork_demo.anl

# 在串口运行
uart:~$ fork_demo
```

**预期输出**：
```
[anl] main start
[anl] spawned
[child] id=1
[child] id=2
[child1] done
[child2] done
```

### 3. 测试当前进程模型

```bash
# 在 RocketPi shell 中测试
uart:~$ ps              # 查看进程列表
uart:~$ fork            # 测试 fork 命令
uart:~$ new_task test   # 测试 new_task API
```

---

## 🔧 开始改进：Phase 1 (FD 引用计数)

### Step 1: 创建 idesc 基类

```bash
cd /opt/Program/UCAS/OneWo-zepLinux

# 创建头文件
cat > zephyr/include/zephyr/kernel/idesc.h << 'EOF'
#ifndef ZEPHYR_INCLUDE_KERNEL_IDESC_H_
#define ZEPHYR_INCLUDE_KERNEL_IDESC_H_

#include <zephyr/sys/atomic.h>

struct idesc_ops;

struct idesc {
    const struct idesc_ops *ops;
    atomic_t refcount;
    uint32_t flags;
};

struct idesc_ops {
    int (*close)(struct idesc *desc);
    ssize_t (*read)(struct idesc *desc, void *buf, size_t len);
    ssize_t (*write)(struct idesc *desc, const void *buf, size_t len);
    int (*ioctl)(struct idesc *desc, int request, void *arg);
};

/* 引用计数 API */
static inline struct idesc *idesc_get(struct idesc *desc)
{
    if (desc) {
        atomic_inc(&desc->refcount);
    }
    return desc;
}

static inline void idesc_put(struct idesc *desc)
{
    if (desc && atomic_dec(&desc->refcount) == 1) {
        if (desc->ops && desc->ops->close) {
            desc->ops->close(desc);
        }
    }
}

#endif /* ZEPHYR_INCLUDE_KERNEL_IDESC_H_ */
EOF
```

### Step 2: 修改 process.c 使用引用计数

```bash
# 编辑 zephyr/kernel/process.c
# 在 process_idesc_table_add() 中添加:
#   idesc_get(desc);
# 在 process_idesc_table_remove() 中添加:
#   idesc_put(desc);
```

### Step 3: 修改 fork 实现

```bash
# 在 process_fork() 中，将 memcpy 改为:
for (int i = 0; i < CONFIG_MAX_FD_PER_PROCESS; i++) {
    if (parent->fd_table.allocated_mask & BIT(i)) {
        struct idesc *desc = parent->fd_table.entries[i].idesc;
        child->fd_table.entries[i].idesc = idesc_get(desc);  // 引用计数 +1
        child->fd_table.entries[i].flags = parent->fd_table.entries[i].flags;
        child->fd_table.allocated_mask |= BIT(i);
    }
}
```

### Step 4: 测试

```bash
# 编写测试用例
cat > zephyr/tests/kernel/process/test_fork_fd.c << 'EOF'
// 测试 fork 后 FD 引用计数
void test_fork_fd_refcount(void) {
    // 创建一个文件描述符
    // fork
    // 子进程 close，父进程仍可用
    // 父进程 close，此时才真正关闭
}
EOF

# 运行测试
west build -t run
```

---

## 📚 关键文件位置

### Zephyr 实现
```
zephyr/include/zephyr/kernel/process.h       - 进程 API
zephyr/kernel/process.c                      - 进程实现 (474 行)
zephyr/samples/ansilic/*/src/shell_process.* - new_task/waitpid 实现
```

### Embox 参考
```
/opt/Program/UCAS/embox/src/kernel/task/multi/multi.c           - task 管理
/opt/Program/UCAS/embox/src/kernel/task/resource/idesc_table/  - FD 表
/opt/Program/UCAS/embox/src/kernel/task/resource/env/          - 环境变量
```

### 文档
```
EMBOX_ALIGNMENT_PLAN.md          - 完整改进方案（本文件配套）
plan.md                          - 原始迁移计划
```

---

## 🐛 调试技巧

### 1. 查看进程表状态

```c
// 在 shell 命令中添加
void cmd_ps(void) {
    for (int i = 0; i < CONFIG_MAX_PROCESS_COUNT; i++) {
        struct z_process *proc = &process_table[i];
        if (process_allocated_mask & BIT(i)) {
            printk("PID %d: parent=%d, threads=%d, fds=%d\n",
                   proc->pid,
                   proc->parent ? proc->parent->pid : 0,
                   sys_dlist_len(&proc->threads),
                   __builtin_popcount(proc->fd_table.allocated_mask));
        }
    }
}
```

### 2. 跟踪 FD 操作

```c
// 在 process_idesc_table_add/remove 中添加
printk("FD %d: add/remove, refcount=%d\n", fd, atomic_get(&desc->refcount));
```

### 3. 检测孤儿进程

```c
// 在 process_exit 中
printk("Process %d exiting, children count: %d\n",
       proc->pid, sys_dlist_len(&proc->children));
```

---

## ✅ 验收检查清单

完成 Phase 1 后，应通过以下测试：

- [ ] fork 后子进程 close(fd) 不影响父进程使用
- [ ] 父进程 close(fd) 不影响子进程使用
- [ ] 两者都 close 后，底层对象才真正释放
- [ ] refcount 正确递增/递减
- [ ] 在 RocketPi 和 AS32x601 上运行稳定

---

## 🆘 常见问题

**Q: 编译时找不到 `struct k_thread` 的 `process` 字段？**

A: 需要在 `zephyr/include/zephyr/kernel_structs.h` 中的 `struct k_thread` 添加：
```c
struct z_process *process;
sys_dnode_t process_thread_node;
```

**Q: fork 后程序崩溃？**

A: 检查栈空间是否足够（`TASK_STACK_SIZE`），以及是否正确调用 `process_register_thread()`。

**Q: FD 泄漏？**

A: 确保每个 `idesc_get()` 都有对应的 `idesc_put()`，使用引用计数调试输出跟踪。

---

## 📞 联系和协作

- **代码仓库**: `/opt/Program/UCAS/OneWo-zepLinux`
- **Embox 参考**: `/opt/Program/UCAS/embox`
- **工作分支**: `private-loader`
- **主分支**: `main`

---

**最后更新**: 2026-08-15
**下一步**: 开始实施 Phase 1 - FD 引用计数

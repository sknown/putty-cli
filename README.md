# PuTTY - macOS CLI 版本

## 项目目的

本项目是 [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/) 0.83 源码的 **macOS 命令行版**改造。

PuTTY 原项目在 Unix/macOS 上依赖 GTK 图形库编译 GUI 版本（`putty`、`pterm`），在 macOS 上默认没有安装 GTK，导致 `putty` 无法编译。本改造为 `putty` 提供了纯命令行（CLI）版本，使其可以在不安装 GTK 的情况下正常运行。

## 主要修改

### 1. 新增 `unix/putty-cli.c`

这是 CLI 版 `putty` 的入口文件，基于原有的 `unix/plink.c`（plink 的命令行实现）改造而来：

- **重命名为 `putty`** — 使用 `PuTTY` 作为程序名，而非 `plink`
- **复用 plink 的 I/O 架构** — stdin/stdout 直连 SSH 会话，支持管道和重定向
- **本地终端支持** — 自动检测 stdin 是否为 tty，设置合适的终端模式（回显、行编辑、信号处理）
- **终端尺寸感知** — 通过 `SIGWINCH` 信号和 `TIOCGWINSZ` ioctl 获取终端窗口大小变化
- **控制序列净化** — 输出到终端时可选净化控制字符，防止恶意 SSH 服务的欺骗攻击

### 2. 修改 `unix/CMakeLists.txt`

在 GTK 不可用时，`putty` 目标会自动编译为 CLI 版本：

- **GTK 可用时** — 编译原版 GUI `putty`（不变）
- **GTK 不可用时** — 编译 CLI `putty`，链接 `noterminal` + `console` 库，无需 GTK 依赖

## 编译方法

### 前置条件

- macOS 或 Linux
- C 编译器（macOS 自带的 `clang` 或 `gcc`）
- CMake 3.7+

### 编译步骤

```bash
# 克隆仓库（如尚未克隆）
# git clone <your-repo-url>
# cd putty-0.83

# 创建构建目录
mkdir build && cd build

# 配置 CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译所有 CLI 工具
cmake --build . --target putty
cmake --build . --target plink
cmake --build . --target pscp
cmake --build . --target psftp
cmake --build . --target puttygen

# 或者一次性编译所有
cmake --build .
```

编译产物在 `build/` 目录下。

### 仅编译 CLI 工具（跳过 GUI）

macOS 上默认没有 GTK，CMake 会自动跳过 GUI 程序（`pterm`、`puttytel` 等）。如果安装了 GTK 但仍只想编译 CLI 工具，可以手动禁用 GTK：

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_FIND_PACKAGE_GTK3=ON
```

## 可用的 CLI 工具

| 可执行文件 | 大小 | 功能 |
|-----------|------|------|
| **`putty`** | ~1.0MB | 命令行 SSH/Telnet/Serial 客户端（本改造新增） |
| `plink` | ~1.0MB | 命令行连接工具（原项目已有） |
| `pscp` | ~1.0MB | 安全文件复制（SCP/SFTP） |
| `psftp` | ~1.0MB | 安全文件传输（SFTP） |
| `puttygen` | ~430KB | SSH 密钥生成与管理 |

## 使用示例

```bash
# SSH 连接
./build/putty user@example.com

# 执行远程命令
./build/putty user@example.com ls -la

# 指定端口和用户
./build/putty -P 2222 -l admin example.com

# 端口转发（本地转发）
./build/putty -L 8080:localhost:80 user@example.com

# 端口转发（远程转发）
./build/putty -R 8080:localhost:80 user@example.com

# 使用密钥认证
./build/putty -i ~/.ssh/id_rsa user@example.com

# Telnet 连接
./build/putty -telnet host 23

# 序列端口连接
./build/putty -serial /dev/ttyUSB0 -sercfg 115200,8,n,1

# 从文件读取远程命令
./build/putty user@example.com -m commands.txt

# 使用代理命令
./build/putty -proxycmd 'nc %host %port' user@example.com
```

### plink 与 putty 的区别

两者在功能上相似，区别在于：

| 特性 | `putty` | `plink` |
|------|---------|---------|
| 程序名 | `putty` / `PuTTY` | `plink` / `Plink` |
| 协议默认值 | SSH (22)，可通过 `PUTTY_PROTOCOL` 环境变量覆盖 | SSH (22)，可通过 `PLINK_PROTOCOL` 环境变量覆盖 |
| 命令行风格 | 支持 `-P port`（大写 P） | 支持 `-P port`（大写 P） |
| 原始 putty 用户 | 与原 GUI putty 命令行一致 | 不同的命令行风格 |

## 项目文件结构

```
putty-0.83/
├── unix/
│   ├── putty.c          # GUI putty 主程序（需 GTK）
│   ├── putty-cli.c      # [新增] CLI putty 主程序（无 GTK 依赖）
│   └── plink.c          # plink CLI 主程序
├── build/               # 构建输出目录
├── CMakeLists.txt       # 顶级 CMake 配置
└── README.md            # 本文件
```

## 许可证

本项目基于 PuTTY 原始许可证（MIT 类许可证）。详见 `LICENCE` 文件。
# putty-cli

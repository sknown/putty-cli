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

### 3. Daemon/Client 模式（AI 工具集成支持）

在 `putty-cli.c` 中新增了 daemon/client 模式，用于持久化 SSH 连接，方便 AI 工具（如 AI skill/MCP）多次调用：

- **Daemon 模式** (`--daemon socket-path`)：建立 SSH 连接后，通过 Unix domain socket 监听客户端连接。SSH 会话保持活跃，多个客户端可以依次连接、发送数据、断开，而 SSH 连接不中断。前台运行时按 Ctrl+C 可正常退出并自动清理 socket 文件。
- **Client 模式** (`--connect socket-path [command]`)：作为轻量客户端连接 daemon 的 Unix socket，将 stdin 数据转发给 daemon，daemon 回复的数据输出到 stdout。支持直接在命令行传递要执行的远程命令（如 `putty --connect /tmp/sock hostname`），无需通过管道。

工作原理：
```
┌─────────────────┐     Unix socket      ┌──────────────────┐
│  putty --connect │ ◄──────────────────► │  putty --daemon  │ ◄──── SSH ────► 远程服务器
│  (client)        │     /tmp/xxx.sock    │  (持久连接)       │
└─────────────────┘                       └──────────────────┘
      ▲                                            ▲
      │ 多次调用，共享同一 SSH 连接                   │ 一次建立，持续保活
      ▼                                            ▼
  AI skill / MCP                              后台进程（nohup/launchd）
```

### 4. MCP 模式（Model Context Protocol）

新增 MCP STDIO 服务器模式，让 AI 客户端（如 Claude Desktop、Cursor 等）可以通过 MCP 协议直接调用远程命令执行：

- **MCP 模式** (`--mcp socket-path`)：作为 MCP STDIO 服务器运行，通过 stdin/stdout 交换 JSON-RPC 消息。连接到已运行的 daemon，暴露以下工具：
  - `execute_command` — 在远程服务器上执行命令，返回输出（支持 timeout 参数）
  - `list_sessions` — 扫描目录下的 `.sock` 文件，列出活跃的 daemon 会话

工作原理：
```
AI Client (Claude/Cursor)
     │ stdin/stdout (JSON-RPC)
     ▼
putty --mcp /tmp/sock    ← MCP STDIO 服务器
     │ Unix socket (raw relay)
     ▼
putty --daemon /tmp/sock ← 已有 daemon
     │ SSH
     ▼
远程服务器
```

MCP 工具定义：

| 工具 | 参数 | 说明 |
|------|------|------|
| `execute_command` | `command` (必填), `timeout` (可选，默认30秒) | 通过持久 SSH 连接执行远程命令 |
| `list_sessions` | `directory` (可选，默认 /tmp) | 列出活跃的 putty-cli daemon 会话 |

## 编译方法

### 前置条件

- macOS 或 Linux
- C 编译器（macOS 自带的 `clang` 或 `gcc`）
- CMake 3.7+
- libcjson（可选，用于 MCP 模式。如未安装，将使用内置的轻量 JSON 解析器自动编译）

macOS 安装 libcjson：`brew install cjson`

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

# Daemon 模式：启动持久 SSH 连接（后台运行）
nohup ./build/putty --daemon /tmp/myserver.sock user@example.com &

# Daemon 模式：前台运行（Ctrl+C 退出，自动清理 socket）
./build/putty --daemon /tmp/myserver.sock user@example.com

# Client 模式：通过 daemon 执行命令（管道方式）
echo "ls -la" | ./build/putty --connect /tmp/myserver.sock
echo "uptime" | ./build/putty --connect /tmp/myserver.sock

# Client 模式：直接传命令执行（无需管道）
./build/putty --connect /tmp/myserver.sock "ls -la"
./build/putty --connect /tmp/myserver.sock "hostname"

# 指定 SSH 端口启动 daemon
nohup ./build/putty --daemon /tmp/myserver.sock -P 2222 admin@example.com &

# 带密码文件启动 daemon（非交互式）
nohup ./build/putty --daemon /tmp/myserver.sock -pwfile ~/.ssh/pwd.txt user@example.com &

# MCP 模式：启动 MCP STDIO 服务器（需先启动 daemon）
./build/putty --mcp /tmp/myserver.sock
```

### Claude Desktop MCP 配置

在 Claude Desktop 的配置文件中添加 putty-cli MCP 服务器：

```json
{
  "mcpServers": {
    "putty-ssh": {
      "command": "/path/to/putty",
      "args": ["--mcp", "/tmp/myserver.sock"]
    }
  }
}
```

配置文件位置：
- macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`
- Windows: `%APPDATA%\Claude\claude_desktop_config.json`

### Qoder IDE MCP 配置

在项目根目录创建 `.mcp.json` 文件，Qoder IDE 会自动识别：

```json
{
  "mcpServers": {
    "putty-ssh": {
      "command": "/path/to/putty",
      "args": ["--mcp", "/tmp/myserver.sock"]
    }
  }
}
```

使用前需确保 daemon 已启动：
```bash
nohup ./build/putty --daemon /tmp/myserver.sock user@example.com &
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
│   ├── putty-cli.c      # [新增] CLI putty 主程序（无 GTK 依赖，含 daemon/client/MCP 模式）
│   ├── CMakeLists.txt   # [修改] 编译配置（CLI/GTK 自动切换，libcjson 检测）
│   └── plink.c          # plink CLI 主程序
├── .mcp.json             # [新增] Qoder IDE MCP 服务器配置
├── build/               # 构建输出目录
├── CMakeLists.txt       # 顶级 CMake 配置
└── README.md            # 本文件
```

## 许可证

本项目基于 PuTTY 原始许可证（MIT 类许可证）。详见 `LICENCE` 文件。

# Linux 服务器在线双人 PvP 部署指南 (`rmexe_server`)

`rmexe_server` 是一个单文件、零第三方运行时依赖（仅依赖 Linux 标准 `libc` / `libstdc++` / `pthread`）的 **TCP HTTP + UDP 实时中继服务器**，同时监听同一个端口（默认 `8765`）：
- **TCP `8765`**：处理房间创建 (`/pvp/create`)、房间列表 (`/pvp/rooms`)、快速匹配/指定房号加入 (`/pvp/join`)、服务器在线状态 (`/pvp/status`) 以及原有的单机排行榜/芯片交换接口。
- **UDP `8765`**：处理双人实时 PvP 的 NAT 穿透绑定、玩家 `.sav` 存档构筑同步 (`PKT_BUILD_SYNC`)、选卡界面同步确认 (`PKT_CUSTOM_COMMIT`) 以及带 8 帧滑动冗余窗口与丢包重传的帧锁定/GGPO 回滚输入流 (`PKT_INPUT_BUNDLE` / `PKT_INPUT_REQ`)。

---

## 1. 编译服务端

在开发机或直接目标 Linux 服务器上编译：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rmexe_server -j
```

生成的可执行文件位于 `build/rmexe_server`，可直接拷贝至任意同架构 Linux 服务器运行。

---

## 2. 部署与 systemd 自启动

在你的 Linux 云服务器上执行：

```bash
sudo useradd -r -s /usr/sbin/nologin rmexe || true
sudo mkdir -p /opt/rmexe-server /var/lib/rmexe-server
sudo cp rmexe_server /opt/rmexe-server/rmexe_server
sudo chmod +x /opt/rmexe-server/rmexe_server
sudo chown -R rmexe:rmexe /opt/rmexe-server /var/lib/rmexe-server

sudo cp rmexe-server.service /etc/systemd/system/rmexe-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now rmexe-server
sudo systemctl status rmexe-server
```

查看实时日志：
```bash
sudo journalctl -u rmexe-server -f
```

---

## 3. 防火墙与云服务器安全组配置（关键！）

**务必同时放行 `TCP` 和 `UDP` 的 `8765` 端口**（仅开 TCP 会导致房间能创建但 UDP 无法握手开战）：

### UFW (Ubuntu / Debian)
```bash
sudo ufw allow 8765/tcp
sudo ufw allow 8765/udp
```

### firewalld (CentOS / Rocky / Fedora)
```bash
sudo firewall-cmd --permanent --add-port=8765/tcp
sudo firewall-cmd --permanent --add-port=8765/udp
sudo firewall-cmd --reload
```

同时请在云厂商控制台（阿里云 / 腾讯云 / AWS / Vultr 等）安全组入站规则中添加：
- `TCP 8765` (`0.0.0.0/0`)
- `UDP 8765` (`0.0.0.0/0`)

验证服务端健康状态：
```bash
curl http://<你的服务器公网IP>:8765/pvp/status
```

---

## 4. 玩家客户端联机方式

### 方式 A：游戏内 `F1` 菜单联机（推荐）
1. 启动 `pon_jp_recomp`（默认自动读取玩家的 `.sav` 存档：HP上限、洛克炮攻击/蓄力等级、属性、导航员改造 `NaviCust` 技能、30张文件夹芯片与预设芯片 Regular Chip）。
2. 按 **`F1`** 呼出配置菜单：
   - 将 **联机模式** 切换为 `远程服务器`。
   - 在 **服务器地址** 填入 `<你的服务器公网IP>:8765`。
   - 设置 **玩家昵称**。
   - 点击 **【双人实时 PvP 对战大厅】**：
     - 房主选择 **创建对战房间 (P1)**，获得房号（如 `#1001`）并进入等待。
     - 挑战者选择 **快速加入空闲房间 (P2)** 或从 **在线房间列表** 选择对应房号加入，双方自动同步存档构筑并进入实时对战！

### 方式 B：命令行参数一键开房/进房
```bash
# 玩家 1（房主）：使用自己的存档创建房间
./pon_jp_recomp --server <服务器IP>:8765 --name "Lan" --sav /path/to/p1.sav --pvp-create

# 玩家 2（挑战者）：使用自己的存档快速加入（传 0 匹配任意空闲房，或传具体房号如 1001）
./pon_jp_recomp --server <服务器IP>:8765 --name "Chaud" --sav /path/to/p2.sav --pvp-join 0
```

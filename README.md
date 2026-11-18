# txtmux

多路 **DNS TXT 记录 → 本地端口** 转发器，带图形界面。一个 exe 里可以同时跑多条规则：

```
本机 127.0.0.1:9000  →  查 TXT(open.example.com) = 198.51.100.20:18000  →  远端
本机 127.0.0.1:7000  →  查 TXT(frps.example.com) = 198.51.100.20:18001  →  远端
```

适用于「对外端口会变、地址写在 TXT 里」的场景（STUN 打洞、frp 中转、动态端口映射）。
客户端只认本机固定端口，端口变了由 txtmux 自己跟着更新。

两种取值方式（「TXT 记录」输入框里填什么都行）：

- **TXT 记录名**：查 TXT 拿 `ip:端口`
- **入口 URL**（`http://` / `https://` 开头）：WinHTTP 请求它、**自动跟随 307/302**，
  取跳转后的 `host:端口` —— 于是 web 服务的动态 STUN 端口也能变成固定本地端口

### 泛域名配置

每条规则可带一个**匹配域名**（支持 `*`），把"客户端访问的名字"和"端口来源"分开：

```ini
[rules]
rule1=https://webdav.example.com/  8443  1  webdav.gw.example.com
rule2=open.example.com             9000  1  open.gw.example.com
rule3=https://webdav.example.com/  8445  1  *.gw.example.com
```

匹配优先级：匹配域名 → 入口域名 → 解析到的目标域名 → 未命中直连（按目标自身 A/AAAA）。
一个端点对应一条规则；通配符用于同一服务的多个别名，不能把多个服务合并到一个端口。

### 和 v2rayN 共存

Windows 的系统代理只有一个位置，v2rayN 会占着它，所以别把 txtmux 设成系统代理，用 PAC 分工：

```ini
[main]
proxy=8080
upstream=127.0.0.1:10809      ; v2rayN 的 HTTP 入站
```

浏览器代理填 `http://127.0.0.1:8080/proxy.pac`。txtmux 按当前规则生成 PAC：
命中规则的域名走本代理，其余走 `upstream`（v2rayN），互不干扰。

### 让网页访问不跳转（本地代理模式）

不要改 hosts —— `*.gw.example.com` 是双栈的，写 `127.0.0.1` 会把 AAAA 一起顶掉，
IPv6 访问就断了。用本地代理：

```ini
[main]
proxy=8080
```

浏览器代理指向 `127.0.0.1:8080`，规则填 `https://webdav.example.com/`，然后直接访问
`https://webdav.gw.example.com/`。txtmux 收到 `CONNECT` 后认出域名、跟随 307 拿到
真实 STUN 端点、直连并回 200，TLS 端到端直通（证书 `*.gw.example.com` 匹配）。
不动 DNS，不解析明文 HTTP，非匹配域名直接直连。

## 功能

- 图形界面管理多条规则：`TXT 记录 + 本地端口` → 添加 / 删除
- **单条启用/停用**（按钮或双击列表），也可全部启动 / 全部停止
- 列表实时显示每条规则的状态与当前解析到的目标地址
- 可选「启动时自动连接」，关闭窗口自动停止全部规则
- 配置存 exe 旁的 `txtmux.ini`（INI，XP 自带 API 读写）

## 行为

- 每条规则独立监听 `127.0.0.1:<端口>`，各自查自己的 TXT
- 解析结果按规则缓存 30 秒；解析失败沿用上一次成功的地址
- 已建立的连接不会被重新解析打断，只有新连接才用新地址
- TXT 值必须是 `host:port` 或 `[ipv6]:port`；目标建连目前走 IPv4

## 编译

```sh
./build.sh          # 输出 txtmux32.exe / txtmux64.exe
```

产物是 PE32（子系统 4.0）和 PE32+（5.2），只依赖系统 DLL（DNSAPI / GDI32 / KERNEL32 /
msvcrt / USER32 / WS2_32），`-static` 静态链接，不需要 VC 运行库。

**32 位版可在 XP SP1 ~ Windows 11 的 32/64 位系统上直接运行**，这也是推荐先试的那个。

## 已验证 / 未验证

- 已验证（在 Linux 上把纯逻辑单独抽出来跑）：
  - `parse_endpoint`：IPv4、域名、`[v6]:port`、带空白、缺端口、裸 IPv6、空串、端口越界
    （顺带修掉了 `[v6]:port` 把 `]` 算进主机名的 bug）
  - `parse_rule_line`：两段旧格式、禁用标记、带空白、缺端口、端口越界、空串
  - `parse_url`：https/http、带端口、带路径查询串、缺主机、端口 0、非 http 协议
- 未验证：Windows 上的窗口与多路转发的实际运行（开发环境没有 Windows 机器）

详细使用说明见 `docs/USAGE.txt`。

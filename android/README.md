# txtmux 安卓版

把 Windows 上定稿的 txtmux（多路 TXT 记录 / 入口 URL → 本地端口转发 + 本地代理）移植成
**普通安卓应用**：不 root、不装 LSPosed、不需要 Xposed，装上就能用。

产物：`txtmux-android.apk`（约 45 KB，minSdk 24 / targetSdk 34，自签名）。

## 和 Windows 版的功能对应

| Windows 版 | 安卓版 |
| --- | --- |
| 规则：TXT 记录名 → `ip:端口` | 一样，内置 DNS 查询（UDP 53 → DoH 兜底），不再依赖 `DnsQuery` |
| 规则：入口 URL → 跟随 307/302 取最终 `host:端口` | 一样，内置裸 socket + TLS 跟随，直连不经过系统代理（避免自指） |
| 本地端口转发 | 一样，`127.0.0.1:<端口>` 或 `0.0.0.0:<端口>` |
| 解析缓存 30 秒、失败沿用上次成功地址 | 一样 |
| 本地代理：HTTP CONNECT / SOCKS5 | 一样（SOCKS5 仅 no-auth + CONNECT） |
| 明文 HTTP 代理（绝对 URL 改写成 origin-form） | 一样 |
| `GET /proxy.pac` 输出 PAC | 一样 |
| 上游代理 + 「上游不可用拒绝直连」 | 一样（上游按 HTTP CONNECT 使用） |
| 匹配域名（`*` 通配）决定代理分流 | 一样；不写匹配域名的规则只做端口转发 |
| 规则排序：精确 → 通配 → 仅端口 | 一样（加载时排序） |
| 日志落盘 + 按天归档 + 总量上限 | 一样（`files/txtmux.log`，默认上限 16 MB） |
| ini 配置 | `files/txtmux.json`（同样字段：bind / proxy / upstream / upstreamFallback / autostart / rules） |
| 启动时自动连接 | 「开机自动运行」（BOOT_COMPLETED + 前台服务） |

安卓特有的差异：

- **没有全局系统代理**：安卓只有「设置 → WLAN → 当前网络 → 代理 → 手动」这一处，且只有尊重系统代理
  的应用（OkHttp、浏览器等）会走。要全量代理必须用 VPN 应用，那属于另一个话题。
- 因此安卓上更常用的是**本地端口转发**（把本地端口直接指给播放器 / 下载器 / 文件管理器），
  或者手动配置 WLAN 代理指向 `127.0.0.1:<代理端口>`。
- 前台服务常驻，通知栏显示「转发 N/M 运行 · 代理 …」，可在通知里一键「全部停止」。

## 用法

1. 安装 APK，打开应用。
2. 填「来源」（TXT 记录名，如 `infra.example.com`；或入口 URL，如 `https://webdav.example.com/`），
   需要代理分流时再填「匹配域名」（客户端实际访问的名字，可写 `*.example.com`），填本地端口，点「添加」。
3. 点「全部启动」。规则行会显示 `运行中 → 真实 ip:端口`。
4. 需要本地代理就填代理端口（如 `8080`），点「保存并应用代理设置」，然后把 WLAN 代理设为
   `127.0.0.1:8080`（或用 PAC：`http://127.0.0.1:8080/proxy.pac`，点「复制 PAC 地址」）。
5. 点一条规则可回填编辑（按钮变「保存修改」），长按规则可启用/停用。

## 后台存活（重要）

HyperOS / MIUI 会冻结后台进程（实测本机不处理时进程约 1 分钟后进入 cgroup freezer，
监听套接字还在但连接全部挂住）。应用里点一次「允许后台运行（关闭电池优化）」，
状态变成「已放行」即可；顺手在系统设置里把本应用的「省电策略」设为「无限制」更稳。

## 构建

不需要 gradle。`build.sh` 用本机 Android SDK 直接出包：

```sh
sh build.sh          # 产物 txtmux-android.apk
```

细节：

- 编译用 `android-34/android.jar`（Debian 里的 aapt2 2.19 读不了 android-36 的
  resources.arsc，报 `RES_TABLE_TYPE_TYPE entry offsets overlap`；android-34 的 arsc 它能读）。
- `aapt2`（arm64 Debian 包）+ `d8`（build-tools 36 的 java 脚本）+ `apksigner.jar`；
  本机没有 arm64 zipalign，脚本会自动跳过对齐（没有 .so，不影响安装）。
- 签名用 `/workspace/out/ks.p12`（别名 df，口令同其他模块）。

## 测试记录（本机 XIAOMI 17 / HyperOS 3，非 root 运行）

- 入口 URL 规则：`http://127.0.0.1:18080/` → 302 到 `/final` → 本地 18081 取回 `txtmux-ok path=/final`。
- TXT 规则：`frps.example.com` → `203.0.113.10:50001`，`open.example.com` → `203.0.113.10:50002`。
- 代理 CONNECT：`curl -x http://127.0.0.1:18090 https://webdav.gw.example.com/`
  命中规则后改连 307 解析出的端点（50000），返回 404（与直连一致，说明隧道通了）。
- 明文 HTTP 走代理：返回测试服务内容。
- PAC：规则域名输出 `PROXY 127.0.0.1:18090;`，兜底 `DIRECT`。
- 全部停止/全部启动：状态行与端口监听同步变化；前台服务通知常驻。

## 修复记录（2026-11-18）

- 日志区空白：界面打开时把 `txtmux.log` 尾部 200 行回填到界面，并补上「界面已打开 / 规则增删改 /
  代理设置保存」等日志，不再只有"启动"动作才写日志。
- 状态与实际不一致：
  - 启动某条规则后立刻在后台解析一次目标，行上直接显示 `监听中 → 真实 ip:端口`，
    解析失败则显示 `监听中 · 目标解析失败：TXT 查询失败`（橙色），而不是笼统的"运行中"；
  - 新增 `healthCheck()`（每秒）：线程已死或套接字已关时把状态修正为已停止并记日志；
  - 解析失败后 5 秒内不重试，避免每个连接都等满 DNS 超时。
- 规则填了「匹配域名」但代理端口为 0 时，界面明确提示"代理分流不会生效"，避免误以为通配域名分流已经在工作。
- 状态行/提示行文本去重，避免每秒 setText 造成界面反复刷新（也会让无障碍节点频繁失效）。

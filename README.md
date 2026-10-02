# ESP-NOW 无线串口（ESP32-C3 Pro Mini / HW-953AB）

两块 ESP32-C3 Pro Mini 组成一对**透明无线串口**：

```
PC ──USB──> 板A ──ESP-NOW 2.4G──> 板B ──USB──> PC
```

- 不需要任何扩展板、不需要 RS-485、不需要额外元件
- 板载 LED（**GPIO8**，低电平点亮）指示状态
- 上电**自动配对**，配对信息保存在 flash 里，掉电不丢
- 默认就是**透传模式**：串口收到的每个字节立刻通过 ESP-NOW 发出，不做换行缓存
- 输入 `+++` 进入 AT 配置模式，`ATO` 返回透传模式
- 双向心跳检测，断线自动重连

---

## 1. 硬件

| 项目 | 说明 |
| --- | --- |
| 开发板 | ESP32-C3 Pro Mini（HW-953AB）**× 2** |
| 板载 LED | GPIO8，接 3V3，**低电平点亮** |
| 天线 | 板载陶瓷天线（即板上金色蛇形天线） |
| 供电/通信 | USB-C 数据线 × 2 |

**注意**：两块板必须刷同一个固件、使用同一个信道（默认信道 1）。

### 板载串口是怎么接的

固件把控制台**同时**映射到多个物理串口：

| 端口 | 引脚 | 默认 |
| --- | --- | --- |
| `Serial` | USB-C（ESP32-C3 原生 USB Serial/JTAG，GPIO18/19） | **开**，无损 —— 配置/调试/透传主要靠它 |
| `Serial1`（UART1） | GPIO4 = RX，GPIO3 = TX | **开**，尽力而为的镜像 |
| `Serial0`（UART0） | GPIO20 = RX，GPIO21 = TX | **关**，见下 |

**UART0 默认关闭**：在这块板（HW-953AB）上 GPIO20/21 是右排最下面两个脚，正好紧挨着
PCB 天线。关掉后应用完全不驱动这两个脚（只有 ROM bootloader 每次复位后会在 GPIO21 上
打几百毫秒的 115200 启动日志，硬件行为，改不掉）。

> 需要第三个 TTL 口时，把 `include/config.h` 里的 `ENABLE_UART0_BRIDGE` 改回 `1`。
> **用 CH340/CP2102 转接的板子必须打开它** —— 那颗芯片接的正好是 UART0，不开就用不了 USB。

用 `AT+SYSINFO` 可以随时确认当前生效的映射：

```
ports         : USB + UART1(RX4/TX3)
```

开着的口收到的数据都会汇到同一条 ESP-NOW 链路上发出去；空中收到的数据也会写到所有开着的口。
不想要 UART1 就把 `ENABLE_UART1` 改成 `0`，GPIO3/GPIO4 就空出来给你用（I2C、SPI、ADC 都行）。

### 波特率与吞吐量

**USB 口不看波特率。** 它是 ESP32-C3 的原生 USB CDC，速率由 USB Full-Speed 决定，
`AT+BAUD` 对它完全没有影响 —— 所以只用 USB 的话，改不改波特率都一样。

**`AT+BAUD` 只影响 UART0/UART1**（两者共用同一个值，不能分开设），默认已经设成 **921600（8 倍）**。
接外接 TTL 设备时把它设成同样的值即可；如果接的是只支持 115200 的老设备，
用 `AT+BAUD=115200` 改回去。

两块板实测（连续灌入 32 KB，走 USB 口，`A tx` = 发送板交给空中的字节，
`PC got` = 接收板送进电脑的字节）：

| 灌入速率 | A tx | PC got | 端到端 |
| --- | --- | --- | --- |
| 115200 | 32768 | 32768 | **100%** |
| 460800 | 32768 | 32768 | **100%** |
| 921600 | 32768 | 32768 | **100%** |
| 1500000 | 26014 | 26014 | 79% |
| 3000000 | 16303 | 16303 | 50% |

结论：

- **8 倍（921600）完全可以，零丢包。**
- 天花板约 **1.0~1.16 Mbaud 等效（≈100~116 kB/s）**。
- 瓶颈在**发送板接收 USB 数据**这一段，**不是 ESP-NOW** —— 空中链路在测到的所有速率下
  都是 **0% 丢包**，接收侧环形缓冲溢出也是 0。
- 再往上灌，丢的是「主机 → 发送板」这一段。用 `AT+SYSINFO` 看 `rx overflow` 和
  `console drop` 就能判断丢在哪一环。
- 以上是两块板近距、无干扰的理想值；距离拉远、周围 2.4G 拥挤时会下降。

### UART0 的引脚可以改吗？

**可以随便改。** ESP32-C3 的 UART 信号走 **GPIO 矩阵**（核心源码里就是
`esp_rom_gpio_connect_in_signal` / `esp_rom_gpio_connect_out_signal`），不像有些 MCU 那样
把 UART 绑死在固定管脚上；核心也没有任何"可用引脚白名单"。你在
`Serial0.begin(baud, cfg, rx, tx)` 里给哪两个脚，信号就从哪两个脚走。

改法就是 `include/config.h` 里两行：

```c
#define PIN_UART0_RX   4     // 换成你想要的脚
#define PIN_UART0_TX   5
```

这块板（HW-953AB）引出来的脚是 **0 1 2 3 4 5 6 7 8 9 10 20 21**：

| | 引脚 |
| --- | --- |
| ✅ 随便用 | 0、1、3、4、5、6、7、10 |
| ⚠️ 能用但避开 | 2 / 8 / 9 是**启动 strapping 脚**，上电瞬间的电平决定启动模式；GPIO8 还接着板载 LED，GPIO9 接着 BOOT 按键 |
| ❌ 绝对不能用 | **11~17** —— 封装内 SPI Flash 占用，根本没引出来<br>**18/19** —— 原生 USB D-/D+，本固件正在用（用 CH340 的板子除外） |

`config.h` 里已经加了编译期检查，选错会**直接编译报错**，不会等烧进去才发现：

```
config.h:53:6: error: #error "GPIO11..GPIO17 are wired to the in-package SPI flash and cannot be used"
```

接线注意：

- **两边 GND 必须接一起**，否则串口通信不可靠。
- **RX/TX 交叉**接（你的 TX → 对方 RX）。
- 换脚之后，上电瞬间 ROM bootloader 仍然会把启动日志以 115200 打在 **GPIO21**
  （硬件默认的 U0TXD，改不掉）。不影响运行，只是每次复位会看到一点乱码。
- 也可以改用 **UART1**（C3 一共 2 个 UART，`Serial1`）。但保留 UART0 有实际好处：
  那些 USB-C 后面焊 CH340/CP2102 的板子，转接芯片接的正好是 UART0，
  换成 UART1 就够不到电脑了 —— 这也是本固件"一个固件兼容两种板子"的关键。

### UART1 的引脚在哪？

**没有默认引脚。** 这是 UART0 和 UART1 最大的区别：

| | 硬件默认引脚 | 说明 |
| --- | --- | --- |
| UART0 | **GPIO20 / GPIO21** | 芯片硬件层面就接在这对脚上，ROM bootloader 也用它，改不掉 |
| UART1 | **无** | 完全靠 GPIO 矩阵，`Serial1` 初始化时两个脚都是 `-1`（未分配） |

所以 UART1 用哪两个脚，**完全由下面这两行决定**，改完重新编译烧录即可：

```c
#define ENABLE_UART1  1     // 1 = 用上这个口，0 = 放开 GPIO3/GPIO4
#define PIN_UART1_RX  4     // 板子丝印 A4 / SCK
#define PIN_UART1_TX  3     // 板子丝印 A3
```

注意不能和 `PIN_UART0_*` 用同一对脚（一个脚只能承载一路信号），`config.h` 里有编译期检查会拦住。

### 怎么确认 UART1 真的在收发？

**一根杜邦线**就能完整验证（GPIO3 和 GPIO4 在左排是挨着的）：

1. 把某块板（比如 A）的 **GPIO3 和 GPIO4 短接**。
2. 从 A 的 USB 口发 `+++` 进入配置模式。
3. 固件会把横幅写到所有口，包括 UART1 TX(GPIO3)；短接后这串字符立刻回到
   UART1 RX(GPIO4)，被当成"串口收到的数据"通过 ESP-NOW 转发。
4. 于是 **B 的 USB 口会打印出这行横幅** —— TX 和 RX 一次性都验证了。

已在两块板上实测通过（各短接一根线）：

```
PASS  board A (3C:0F:02:BB:CC:44): UART1 TX=GPIO3 + RX=GPIO4 verified
PASS  board B (3C:0F:02:BB:C5:50): UART1 TX=GPIO3 + RX=GPIO4 verified
```

> 验证完记得**把短接线拔掉**。留着的话所有控制台输出都会回环、并被转发到对端，
> 对端会一直收到本机的日志噪声。

---

## 2. 编译与烧录

需要 VS Code + PlatformIO 插件（推荐，自带 PlatformIO Core 6.x），或者自己装 PlatformIO CLI。

```bash
pio run                                        # 编译
pio run -t upload -p /dev/ttyACM0              # 烧录第一块板
pio run -t upload -p /dev/ttyACM1              # 烧录第二块板
pio device monitor -p /dev/ttyACM0             # 监视第一块板，115200
pio device monitor -p /dev/ttyACM1             # 监视第二块板（另开一个终端）
```

也可以直接在 VS Code 里点 PlatformIO 工具栏的 **Build / Upload / Monitor**。
两块板刷的是**同一个** `env:esp32-c3-pro-mini`。

> ⚠️ **不要用 `sudo apt install platformio`。** Ubuntu 仓库里的是 4.3.4（2020 年版），
> 会通过 `/usr/bin/pio` 把插件自带的 6.x Core 顶掉，然后报
> `Error: Unknown development platform 'espressif32'`。用插件自带的即可；
> 想在终端用 `pio` 命令，把 `~/.platformio/penv/bin` 加进 `PATH`：
> ```bash
> export PATH="$HOME/.platformio/penv/bin:$PATH"   # 可写进 ~/.bashrc
> ```

### 两块板怎么区分？

ESP32-C3 的 **USB 描述符里的序列号就是芯片 MAC**，所以可以据此对号入座：

```bash
$ for p in /dev/ttyACM*; do printf "%-16s " $p; \
    udevadm info -q property -n $p | grep -oP '(?<=^ID_SERIAL_SHORT=).*'; done
/dev/ttyACM0     3C:0F:02:BB:CC:44
/dev/ttyACM1     3C:0F:02:BB:C5:50
```

⚠️ **`/dev/ttyACMx` 的编号是不稳定的。** 板子复位、拔插、USB 重新枚举都可能让两块板
互换编号（实测中板 B 就从 `ttyACM1` 变成了 `ttyACM2`）。写脚本或固定流程请用带 MAC 的
稳定软链接：

```bash
$ ls /dev/serial/by-id/
usb-Espressif_USB_JTAG_serial_debug_unit_3C:0F:02:BB:CC:44-if00 -> ../../ttyACM0
usb-Espressif_USB_JTAG_serial_debug_unit_3C:0F:02:BB:C5:50-if00 -> ../../ttyACM2
```

路径可以直接传给 `-p`：

```bash
P=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_3C:0F:02:BB:CC:44-if00
pio run -t upload -p "$P"
pio device monitor -p "$P"
```

### 两个注意事项

- 有**两块板**时必须用 `-p /dev/ttyACMx` 指定端口，否则 PlatformIO 只会自动挑第一个。
- **同一个串口同一时刻只能被一个程序打开**。如果已经开着一个监视器，再开一个会报：
  ```
  UserSideException: [Errno 11] Could not exclusively lock port /dev/ttyACM1
  ```
  解决办法：先关掉（`Ctrl+C`）占用的那个，或者换个端口。
  查是谁占着：`fuser -v /dev/ttyACM1`。
  烧录前也要先关掉监视器，否则同样会被占用。

---

## 3. 快速上手

1. 两块板都烧好固件，分别插到电脑上。
2. 打开两个串口监视器（115200），会各看到一行 MAC 地址：

   ```
   === espnow-serial 1.0.0 - ESP32-C3 Pro Mini ===
   MAC: 64:E8:33:12:34:56
   [link] looking for a peer ...
   ```

3. 什么都不用做 —— 两块板会互相广播配对请求，几秒内自动配对成功：

   ```
   [link] auto-paired with 64:E8:33:AB:CD:EF
   [link] peer online
   ```

4. 现在在 A 的串口里打字，B 的串口就会原样显示；反过来也一样。

> 换行符不会被自动添加，终端里的「发送」按钮请勾选 `\r\n` 或手动回车。

### LED 状态

| LED 表现 | 含义 |
| --- | --- |
| 慢闪（0.5 s） | 还没配对，正在找对端 |
| 快闪（0.12 s） | 已配对，但对端掉线/超时 |
| **常亮** | 链路正常 |
| 数据流动时轻微抖动 | 正在收发数据 |

---

## 4. 透传模式与 `+++` 转义

默认处于透传模式，所有字节双向原样搬运。

想进配置模式，先**停止发送 1 秒**，然后连续输入三个加号 `+++`：

```
+++
=== espnow-serial 1.0.0 - ESP32-C3 Pro Mini ===
MAC: 64:E8:33:12:34:56
peer: 64:E8:33:AB:CD:EF
[AT] configuration mode - AT+HELP for commands, ATO to resume data
```

- 只有「前后都有 1 秒静默」的 `+++` 才算转义，所以你完全可以正常传 `a+b`、`1++2` 这类数据。
- 在配置模式里输入 `ATO` 返回透传模式。

> ⚠️ **固件自己绝不打印字面的三个加号。** 这是个坑：如果你把 TX/RX 短接做回环，或者接了
> 一个会把收到的数据 echo 回来的设备，固件打印出来的 `+++` 会立刻被当成转义序列收回来，
> 把固件踢进配置模式；而配置模式的回复又被回环回去、又被当成 AT 指令…… 就死循环了
> （实测踩过）。所以固件里的提示文字一律写成“三个 `+`”，你写扩展代码时也要注意。

> ⚠️ **做完回环测试一定要把短接线拔掉 —— 两端都要拔。** 实测踩过，机理是：
>
> 本固件把「串口收到的」转发到空中，又把「空中收到的」写到所有串口。两块板都短接时环路闭合：
>
> ```
> A 的控制台输出 → A 的 UART1 TX → 短接线 → A 的 UART1 RX → 空中
>   → B 的控制台输出 → B 的 UART1 TX → 短接线 → B 的 UART1 RX → 空中 → 回到 A → ∞
> ```
>
> 症状：链路上反复循环同一段文字（看起来像“疯狂重启”，但数 `ESP-ROM` 只有 **1 次**真实启动，
> 其余都是被回环重放的文字）。而且**AT 指令也进不去** —— 输入行被洪泛字符灌满，`ATO` 会被
> 当成乱码，只能拔线才能停。
>
> `CONSOLE_RX_BUDGET` 能保证主循环不被饿死（板子不会硬卡死或看门狗复位），
> 但**打不断这个环路** —— 这是透传中继的固有性质，不是那个上限能解决的。
>
> 只短接**一块**板不会成环（转一圈到对端就停），但**仍然会把数据反射回发送方**：
> B 发的数据经 A 打印 → A 的回环 → A 再发回空中 → B 会收到自己刚发出去的内容
> （实测端到端计数变成 117%~145%，因为每个包都多回来一次）。所以单板回环只适合做链路验证，
> 别在做正式传输时留着。
>
> 同理，**接在 TTL 口上的设备只要有回显，对端就会收到自己发出去的数据** —— 这是透传中继的
> 固有性质。要支持回显型设备，需要在协议里加跳数/来源标记来打断反射。

---

## 5. AT 指令

| 指令 | 说明 |
| --- | --- |
| `AT` / `AT+HELP` | 显示帮助 |
| `AT+MAC` | 本机 MAC 地址 |
| `AT+FRIEND` | 已配对的对端 MAC |
| `AT+PAIR=<MAC>` | 手动与指定 MAC 配对，例：`AT+PAIR=AA:BB:CC:DD:EE:FF` |
| `AT+UNPAIR`（或 `AT+CLEAR`） | 清除配对信息 |
| `AT+CHANNEL=<1-13>` | 无线信道，两端必须一致 |
| `AT+BAUD=<rate>` | UART0/UART1 波特率（共用一个值），默认 921600；USB 口忽略该值 |
| `AT+AUTOPAIR=<0\|1>` | 是否自动配对（默认开） |
| `AT+VERBOSE=<0\|1>` | 透传模式下是否打印状态信息（默认开；传二进制数据时建议关掉） |
| `AT+TEST` | 向对端发一个测试包 |
| `AT+SYSINFO` | 系统信息与收发统计 |
| `AT+SAVE` | 保存设置到 flash |
| `AT+RESTART` | 重启 |
| `ATO` | 退出配置模式，进入透传 |

所有设置（配对 MAC、信道、波特率、自动配对、verbose）都会自动写进 NVS，重启后保留。

---

## 6. 代码结构

```
platformio.ini        PlatformIO 配置（板型 / USB / 监视器）
include/config.h      引脚、默认值、协议与定时参数
include/settings.h    NVS 配置读写接口
include/link.h        ESP-NOW 链路接口
src/settings.cpp      NVS（Preferences）持久化
src/link.cpp          ESP-NOW 收发、配对、心跳、分片
src/main.cpp          串口镜像、透传/AT 状态机、LED、AT 指令
```

### 空中协议

每帧 ESP-NOW 数据的第 1 个字节是类型，后面是负载（单帧最多 250 字节，用户数据按 200 字节切片）：

| 类型 | 值 | 含义 |
| --- | --- | --- |
| `PKT_DATA` | 0x01 | 透传的用户数据 |
| `PKT_PAIR_REQ` | 0x02 | 广播寻找配对（带自己的 MAC） |
| `PKT_PAIR_ACK` | 0x03 | 单播应答配对 |
| `PKT_PING` | 0x04 | 心跳，用于在线检测 |
| `PKT_INFO` | 0x05 | 文本信息（`AT+TEST`） |

### 常用可调参数（`include/config.h`）

```c
#define PIN_LED            8     // 板载 LED
#define ENABLE_UART0_BRIDGE 0    // UART0 默认关：GPIO20/21 贴天线，且 CH340 板要开
#define ENABLE_UART1       1     // 可选的第三个口，PIN_UART1_RX/TX 决定在哪两个脚
#define PIN_UART1_RX       4     // UART1 没有默认引脚，必须自己指定
#define PIN_UART1_TX       3
#define DEFAULT_BAUD       921600 // 只影响 UART0/UART1，USB 口忽略波特率
#define DEFAULT_CHANNEL    1
#define ESPNOW_CHUNK       240   // 每帧承载的用户字节数（1 字节帧头）
#define PEER_TIMEOUT_MS    4000  // 多久收不到心跳算掉线
#define CONSOLE_RX_BUFFER  4096  // 串口收发环形缓冲（核心默认只有 256）
#define CONSOLE_RX_BUDGET  256   // 每个口每轮最多处理多少字节，防止自激时饿死主循环
```

---

## 7. 常见问题

**串口监视器里没有输出？**
固件平时是**静默透传**的，没有数据流过就什么都不打印，这是正常的。想看状态直接敲 `+++`
（先静默 1 秒），它会回一行版本号、本机 MAC、对端 MAC。

还有一点：**打开串口监视器会把板子复位一次。** Linux 的 `cdc_acm` 驱动在 open 时会先把
DTR/RTS 拉起来，ESP32-C3 把它当成 USB 复位请求（`rst:0x15 USB_UART_CHIP_RESET`），
所以打开监视器**会**看到开机横幅。配对信息和设置都存在 flash 里，重启后自动恢复，不影响使用。

**两板一直慢闪（配不上对）？**
- 检查是不是两块板都在「未配对」状态（配好一对之后，第三块板不会插进来抢）。
- 用 `AT+UNPAIR` 清掉之后等几秒。
- 也可以打开 `AT+VERBOSE=1`，然后手动配对：在 A 上 `AT+MAC` 抄下地址，在 B 上 `AT+PAIR=<A的MAC>`，再在 A 上 `AT+PAIR=<B的MAC>`。
- 确认两端 `AT+SYSINFO` 里的 `radio channel` 一致。

**LED 快闪？**
已配对但对端没有心跳，通常是距离太远、被金属遮挡或对端断电。

**传文件/二进制数据出错？**

分两种情况：

- **丢内容**：先关掉状态信息 `AT+VERBOSE=0`，否则链路状态变化时的提示文字会
  插进透传数据里。
- **丢字节**：看 `AT+SYSINFO` 的计数器定位
  （`tx errors` / `rx overflow` / `console drop`）。实测 921600 以内零丢包；
  再快就会丢在「主机 → 发送板」那一段。ESP-NOW 本身不丢。

**GPIO20/21 上接着别的东西会不会打架？**
本固件默认已经把 `ENABLE_UART0_BRIDGE` 设为 `0`，GPIO20/21 完全空着，可以直接当普通 IO 用。
如果你手动把它打开了，UART0 就会一直驱动 GPIO21 并在 GPIO20 上接收，那两脚另有用途时要改回 `0`。



# Buffer 接口设计

- [x] 阅读现有接口，确定仅设计声明与契约。
- [x] 补齐初始化、位置访问、空间查询、偏移推进、空间保证和清空接口。
- [x] 编译检查接口类型并检查差异。

## 设计约定

长度使用 std::size_t；读指针只读；推进越界抛出异常；扩容后重新获取指针。
本次不实现方法体，语法检查不代表运行或链接验证。

## Review

C++11 严格警告模式下语法检查通过，覆盖重复包含、返回类型及典型读写流程；git diff --check 通过。当前为接口声明，方法体尚未实现，未做链接或运行验证。

## 继续补充读写接口

- [x] 确认新增的写入和读取接口占位。
- [x] 声明 Write / Read，明确偏移推进、边界和指针契约。
- [x] 编译检查新声明及典型调用。

验证结果：C++11 严格警告模式语法检查通过，覆盖接口签名、二进制数据调用和零长度调用；git diff --check 通过。尚无方法实现，未验证运行行为。

## ReadAsString 指定长度读取改造

- [x] 确认按传入长度读取并消费，越界保持状态不变。
- [x] 修复实现并补齐标准库头文件。
- [x] 运行边界和数据完整性测试。

验证：C++11 严格警告编译及 ASan/UBSan 运行通过，覆盖零容量、零长度、越界状态不变、部分消费、内嵌空字符、全部消费后复用及返回字符串独立性。git diff --check 通过。

## Buffer 头文件与实现分离

- [x] 检查当前实现，确定保留简单内联函数、迁移复杂实现的范围。
- [x] 迁移函数并检查构建规则，保留现有接口和行为。
- [x] 对比分离前后运行结果，验证多翻译单元链接和项目构建。

### Review

已将构造函数、偏移检查、空间调整、Write、Peek、PeekAsString 移入 Buffer.cc，保留简单访问和短包装函数。迁移函数体去除空白后与原实现一致，接口签名保持不变。修正字符串分配失败的注释。Makefile 保留头文件依赖，仅编译 .cc 输入。

分离前后均通过相同 C++11 严格警告及 ASan/UBSan 测试，覆盖搬移、扩容、二进制数据、查看与消费、异常及空缓冲区；多翻译单元链接通过。临时副本中的项目构建和运行通过，git diff --check 通过。

## FindCRLF / GetLine 查找与取行

- [x] 确定 FindCRLF 的返回语义、const/noexcept 与指针有效期契约。
- [x] 实现跨边界安全的 CRLF 查找并实现 GetLine 消费整行。
- [x] 编译并运行边界测试。

### 设计约定

- FindCRLF 在可读区内查找首个 "\r\n"，返回指向 '\r' 的指针，未找到返回 nullptr；不消费、不改偏移。
- 可读长度小于 2，或 '\r' 为可读区末字节（'\n' 未到达）时视为未找到，等待后续数据。
- 返回指针指向内部存储，Write/EnsureWritableSize 后失效。
- GetLine 返回含结尾 CRLF 的整行并消费；无完整 CRLF 时返回空串且不消费。空行返回 "\r\n"，故空串唯一表示“尚未收到完整行”。
- 实现用 memchr 定位 '\r'，以 `p + 1 < end` 保证 `p[1]` 合法，避免越界。

### Review

C++11 `-Wall -Wextra -Werror -Wpedantic` 加 ASan/UBSan 编译运行通过，覆盖空缓冲区、长度不足、跨两次写入拼出的 CRLF、连续多行、空行、孤立 '\r'、相邻 "\r\r\n"、无 CRLF 不消费、内嵌空字符、消费后复用及部分消费后继续查找。

首轮测试失败源于用例期望值写错：CheckFound 的偏移是相对当前读位置的偏移，而非行首绝对下标，已修正用例 4、10、11。

## PeekLine / GetLine 消费语义拆分

- [x] 明确 PeekLine 不消费、GetLine 消费的职责划分。
- [x] 重写 GetLine 复用 PeekLine 并按返回长度消费。
- [x] 补充对照测试并运行。

### 设计约定

- PeekLine 返回含结尾 CRLF 的整行，不消费，可重复调用。
- GetLine 返回同样内容并消费；直接以 PeekLine 的返回长度作为消费字节数，未找到时为空串即消费 0 字节。
- 真正的空行返回 "\r\n"，故空串唯一表示“未找到完整行”，按长度消费无歧义。
- PeekAsString 构造失败抛异常时尚未推进偏移，不会误消费。

### Review

C++11 严格警告加 ASan/UBSan 通过，新增用例覆盖 PeekLine 不消费与可重复、无完整行时不消费、空行、以及 PeekLine 结果与 GetLine 消费内容一致。

## 行接口只读约束与返回值优化

- [x] 确认调用位置与现有边界测试。
- [x] 为查看接口添加 const，统一 ReadLine 命名，移除返回局部量的 const，修正契约注释。
- [x] 同步测试并验证严格编译、禁用返回值优化及 ASan/UBSan。

### Review

FindCRLF 返回 const char* 并标记 const；PeekAsString、PeekLine 支持 const 对象。GetLine 改名 ReadLine，仓库源码调用同步迁移；返回局部字符串不再加 const。修正末尾孤立回车契约及 PeekAsString 异常定位。临时副本中使用 C++11 严格警告、-fno-elide-constructors 和 ASan/UBSan 构建运行全部测试通过，新增 const 查看、完整行后孤立回车及零容量测试。git diff --check 通过。未模拟内存分配失败。

## Buffer 全面测试与缺陷排查

- [x] 阅读全部接口并启动独立只读审查。
- [x] 添加初始化、空间回收/扩容、全部读写接口、异常状态、复制/移动、自追加和随机模型测试。
- [x] 在修复前复现缺陷，添加分配失败注入测试。
- [x] 最小范围修复根因并提供可重复运行的测试入口。
- [x] 运行原有测试、新增测试、严格编译和 ASan/UBSan，记录覆盖与限制。

### 验证规格

以独立 std::string 模型比较每次操作后的可读数据；固定随机种子保证复现。
异常发生后检查数据和下标不变；移动后的源对象应有效、为空并可复用。
WriteBuffer 对其他对象保留源数据，对自身追加应得到未读数据的两份副本。
使用独立可执行程序注入分配失败，避免影响正常和 sanitizer 测试。
保留原有 CRLF 测试；测试构建产物使用独立目录，不覆盖已跟踪二进制。

### Review

已完成。发现并修复两处缺陷：隐式移动后源对象保留旧下标；WriteBuffer 自追加扩容后读取已释放内存。新增综合测试、分配失败注入及 make check/check-sanitize 入口。原有测试、80,000 步随机模型、11 项分配失败注入全部通过，ASan/UBSan 无报告。独立复审通过。详见 tasks/buffer-test-report.md。

## LF 查找接口

- [x] 确认新增 FindLF，保留 CRLF 与现有行读取语义。
- [x] 添加 const/noexcept 声明和有界查找实现。
- [x] 验证空缓冲区、LF/CRLF、二进制数据和已消费区域。

### LF Review

新增 FindLF，只查找可读区首个换行，不消费数据。综合测试使用当前 include/src/Test 路径直接构建，C++11 严格警告与 ASan/UBSan 通过，git diff --check 通过。现有 PeekLine/ReadLine 仍按 CRLF 分行。

## 双分行模式

- [x] 确认默认 CRLF，LF 保留全部字节及换行，不自动删除回车。
- [x] 添加 LineMode 参数并同步接口契约。
- [x] 验证两种模式及原有回归测试。

### 双模式 Review

已应用 LineMode::CRLF/LF，PeekLine 和 ReadLine 默认保持 CRLF。综合测试覆盖混合结束符、默认调用兼容、const 查看、空行、未完整行、跨写入结束符及内嵌空字符；C++11 严格编译、ASan/UBSan 和原有 11 项分配失败测试通过。git diff --check 通过。

## spdlog 日志模块

- [x] 确认 C++11、系统 spdlog 1.12 与现有目录结构。
- [x] 实现 Logger、级别过滤、源码位置、控制台及滚动文件、刷新与关闭。
- [x] 接入示例与构建，补充使用文档和独立行为测试。
- [x] 运行严格编译、日志测试和 Buffer 回归，记录 Review。

设计：同步、多线程安全；默认控制台 info；文件按需启用，默认 5 MiB、3 个备份；error 自动刷新。配置与写入由互斥锁串行化。初始化失败保留原日志器；关闭后静默，Init 可重启；不修改 spdlog 全局注册表。宏保留源码位置，过滤时不计算参数。

### 日志模块 Review

已新增 Logger.hpp/Logger.cc 和 LOGGING.md，将 TestBasic 接入封装，修复 Http/Makefile 迁移后失效的源文件路径，提供统一 check 和独立 check-logger。

验证通过：示例编译运行；C++11 -Wall -Wextra -Werror -pedantic 日志测试；六种级别、源码行号、过滤不求值、自动/手动刷新、非法配置和文件打开失败保持旧配置、关闭重启、4 线程 1000 条记录完整性及滚动备份。make -C Http check 通过，日志及 Buffer ASan/UBSan 通过，Buffer 11 项分配失败测试通过，git diff --check 通过。

实现采用同步互斥输出，未实现异步队列；未使用 TSan。spdlog/fmt 系统动态库自身未以 sanitizer 重新构建。保留工作区原有未跟踪内容，未创建提交。

## 独立日志等级封装

- [x] 确认现有 Level 是底层类型别名，确定独立枚举与显式双向映射方案。
- [x] 修改等级、默认配置、宏与内部转换，同步示例和文档。
- [x] 验证全部等级设置/读取/过滤及现有回归测试。

### 独立等级 Review

Level 已改为独立 enum class，内部显式双向映射，不依赖底层枚举数值；配置、宏、示例及文档同步更新。新增七种等级设置/读取及49组过滤组合验证，非法 SetLevel 保持旧状态。make -C Http all check、示例运行及 git diff --check 通过。修复首轮宏转换位置错误和示例原有重复配置声明。

## 可选异步日志

- [x] 核对本机异步队列与销毁接口，确定独立单线程池方案。
- [x] 增加默认关闭的 async、队列容量和独立溢出策略，完成等待式 Flush/Shutdown。
- [x] 验证默认同步、异步多线程、关闭排空、同路径模式切换、失败保留配置和覆盖策略。
- [x] 完成严格编译、回归、sanitizer 与独立审查，更新文档。

设计：async 默认 false；queue_size 默认 8192；策略默认 Block，可选 OverrunOldest。线程池独立且仅一个worker。Flush 通过同队列的阻塞刷新请求及末尾完成通知，等待先前消息处理并刷新；外层锁防止通知被后续覆盖策略消息淘汰。Init 先等待旧日志完成后构造新配置，成功才交换；Shutdown 等待并销毁线程池。覆盖策略已丢弃的消息无法恢复。

### 可选异步日志 Review

已完成：默认 async=false，新增 queue_size=8192 与独立 OverflowPolicy::Block/OverrunOldest；专用单后台线程，阻塞式 Flush/Shutdown，正常析构刷新并等待退出。Init 等待旧队列刷新后才建立新文件 sink，构造失败保留旧配置，不使用全局线程池。文档已同步，宏调用方式不变。

验证：make -C Http all check 通过。原有日志用例分别运行同步/异步模式；容量1时2000条记录关闭后完整有序；重复Flush后立即读取；非法容量/策略及文件失败保留旧配置；4线程1000条记录同时进行16次模式切换；同路径滚动与同步/异步切换；覆盖策略保留消息有序且最后消息存在；独立子进程正常退出后512条日志完整。最终日志测试ASan/UBSan通过，Buffer综合与11项分配失败回归通过，git diff --check通过。独立源码审查未发现可证实死锁或生命周期缺陷。

限制：未运行TSan，系统spdlog/fmt动态库未重新插桩；未注入底层sink I/O异常；覆盖策略测试不依赖具体丢弃数量（受线程调度影响）。异常终止及被覆盖的消息无法保证保留。

## 恢复纯同步日志

- [x] 确认移除异步实现，保留独立等级、线程安全、滚动文件和已有宏。
- [x] 删除异步配置、线程池、完成通知与专用刷新辅助层，更新文档和测试。
- [x] 验证同步日志、正常退出、Buffer回归与严格编译。

### 恢复纯同步 Review

删除 CompletionSink、AsyncState、线程池、条件变量、溢出策略及异步配置；Init 直接构造 spdlog::logger，Flush/Shutdown 直接在锁内刷新。保留独立日志等级、多线程安全、滚动输出、初始化失败保持旧配置和正常析构刷新。清理异步测试与文档，保留同步测试和子进程正常退出检查。

make -C Http all check 通过：C++11 严格警告日志测试、正常退出刷新、Buffer综合及11项分配失败回归通过。git diff --check 通过；源码、示例、测试与文档均无异步接口残留。本次未重新运行sanitizer。

## Bind 命名调整

- [x] 查看选中实现，确定保留 Bind，仅调整参数和局部变量名称。
- [x] 统一为 ip_address、socket_address、ipv4_address 和 address_length，修正 addr4 引用及二进制地址注释。
- [x] 读取结果检查引用和修改范围。

### 命名 Review

仅调整 Bind 内命名及注释；保留原有未完成的绑定逻辑。函数末尾表达式和参数尚未补齐，未进行编译或运行验证。

## Accept 完善

- [x] 确认范围和契约：可选对端地址，调用者关闭新描述符，失败保留 errno。
- [x] 实现 EINTR 重试、非阻塞正常返回和错误日志。
- [x] 验证真实连接与错误分支，记录结果。

### Accept Review

从实际源码提取 Accept 方法体，以 C++11 -Wall -Wextra -Werror -pedantic 编译专项测试通过。真实本机 TCP 测试覆盖可选地址、对端地址与端口、数据收发及非阻塞暂无连接；模拟覆盖 EINTR 重试、有效描述符 0、EWOULDBLOCK 静默返回和日志修改 errno 后恢复。git diff --check 通过。其他 Socket 方法仍有空实现，本次未验证整个 Socket 类或完整项目构建。临时测试位于 /tmp/http_accept_test.cc。

## Socket 完整测试与修复

- [x] 检查全部实现、项目经验与构建配置，分配真实连接和故障注入测试。
- [x] 保留原始版本并复现编译及行为缺陷。
- [x] 最小修复编译、地址转换、创建流程、资源所有权及发送信号问题。
- [x] 接入可重复运行的真实连接与故障注入测试。
- [x] 运行严格编译、ASan/UBSan、原有回归，记录覆盖范围及限制。

测试约定：仅本机回环/本地套接字，TCP 使用系统分配临时端口，测试设超时。覆盖所有公开方法、成功与失败、部分收发、非阻塞、EOF、描述符生命周期及关键系统调用故障；不声称穷尽所有内核错误。

### Socket 完整测试 Review

make -C Http all check check-sanitize 退出码 0。Socket 7 组真实测试及 114 项故障检查在严格 C++11 与 ASan/UBSan 下全部通过；Logger 同步/正常退出、Buffer 综合及 11 项分配失败回归通过。原始编译错误与三项前后行为对照已记录；git diff --check 及新增文件尾随空白检查通过。独立源码审查未发现新增阻断问题。详细覆盖、接口兼容变化和未覆盖范围见 tasks/socket-test-report.md。

## 按官方方法编译并接入 spdlog

- [x] 检查现有Makefile和工具，确定使用与原系统库一致的v1.12.0。
- [x] 从官方仓库克隆源码，通过CMake构建静态库。
- [x] 统一日志示例及Socket测试的本地头文件和库路径，提供deps入口和说明。
- [x] 编译运行全部测试并检查链接依赖，记录结果。

方案：源码放Http/third_party/spdlog，使用库自带fmt，静态链接明确路径，不安装到系统目录；保持C++11和同步日志实现。

### 源码编译 spdlog Review

官方仓库v1.12.0已克隆，提交7e635fca68d014934b4af8a1cf874f63989352b7。CMake Release/C++11构建完成，产出Http/third_party/spdlog/build/libspdlog.a及官方example；使用bundled fmt。新增Http/spdlog.mk共享构建规则及make deps入口；日志和Socket测试明确使用本地include、SPDLOG_COMPILED_LIB和静态库绝对路径。未安装或覆盖系统目录。

make -C Http all check全部通过，日志正常退出、Buffer综合及11项分配失败、Socket全部场景和114项故障注入通过；示例运行正常。readelf确认main、TestLogger、TestSocket不依赖libspdlog.so或libfmt.so；git diff --check通过。源码与构建产物已忽略，复现说明位于Http/third_party/README.md。本次未运行sanitizer。

## Channel 简要接口测试

- [x] 检查已实现接口及测试边界。
- [x] 添加独立测试，覆盖事件开关、回调组合及空回调。
- [x] 严格编译运行并记录结果。

### Channel Review

新增 Http/Test/TestChannel.cc；C++11 -Wall -Wextra -Werror -pedantic -UNDEBUG 编译运行通过。覆盖初始状态、读写开关及重复操作、DisableAll、16种事件组合与回调顺序、监听事件与就绪事件区分、回调替换/清空、空回调和事件快照。未修改 Channel.hpp；Remove及内核注册尚未实现，不在本次测试范围。未验证回调销毁对象或真实网络事件。

## Epoller 添加与修改分离

- [x] 拆分 AddEvent / ModifyEvent，Control 仅封装系统调用。
- [x] 检查参数与对象身份，新增失败回滚映射，修改/删除失败保留映射。
- [x] 使用真实 epoll 验证成功及失败路径，记录 Review。

设计：Channel 为非拥有指针；重复新增和未登记修改/删除抛逻辑异常。新增先登记映射，内核失败则回滚，避免内核成功后分配映射失败。

### Epoller 分离 Review

新增 AddEvent；ModifyEvent 只修改已登记对象；Control 仅调用 epoll_ctl 并抛系统异常。删除成功后才移除映射。新增先 emplace，失败回滚；校验空指针、重复 fd 和对象身份。补齐标准头文件并修复 WaitEvent 缺失 throw。

Http/Test/TestEpoller.cc 使用真实 eventfd/epoll 验证事件返回与写回调、删除后重新添加、空参数、重复新增、未登记修改/删除、错误对象、ADD 失败回滚及 MOD/DEL 失败保留映射。C++11 严格警告编译运行通过，git diff --check 通过。未注入 bad_alloc，未运行整个项目回归；已有 Epoller 缺少析构释放的问题不在本次修改范围。测试进程退出时由系统回收 epoll 句柄。

## Epoller 基本功能测试接入

- [x] 检查已有测试与构建配置，确认 TestEpoller 未接入 Makefile。
- [x] 新增 check-epoller / check-epoller-sanitize 目标并纳入 check 与 clean-tests。
- [x] 补齐可读就绪路径与多描述符同时就绪覆盖。
- [x] 变异验证新断言有效性，运行完整回归。
- [x] 实测析构句柄泄漏并记录。

### Epoller 基本功能 Review

Http/Makefile 新增 EPOLLER_DEPS 及 TestEpoller、TestEpoller-sanitize 显式规则（显式规则优先于 Test/% 通用模式规则），check 与 check-sanitize 均纳入 Epoller，clean-tests 同步清理产物。

TestEpoller.cc 原本只验证可写路径，WaitEvent 从未在真实可读事件下被调用，多描述符分支也始终只有 1 个事件。补充两项：向 eventfd 写入后 WaitEvent 返回该 Channel 且 Handle 触发读回调；第二个 eventfd 与前者同时就绪时一次返回 2 个 Channel（顺序不敏感），并验证可正常注销与关闭。

变异验证：注释 SetREvents 后新增断言 `readable` 失败；将 WaitEvent 循环改为只取首个事件后断言 `ready.size() == 2` 失败（旧测试无法捕获后者）。清理临时变异文件。

make -C Http all check check-sanitize 退出码 0；严格 C++11 无告警。已知未修复缺陷：Epoller 无析构函数且未禁用拷贝，实测构造销毁 200 个对象泄漏 200 个 epoll 句柄（/proc/self/fd 计数 before=6 after=206）。ASan 不检测 fd 泄漏，故 sanitizer 全绿不代表该问题不存在。未注入 bad_alloc。

## Epoller 句柄泄漏修复

- [x] 确认无其他代码依赖 Epoller 可拷贝。
- [x] 添加析构函数释放 epoll 句柄，并显式删除拷贝构造与拷贝赋值。
- [x] 补充句柄泄漏回归与编译期不可拷贝断言。
- [x] 变异验证新断言，复测泄漏归零，运行完整回归。

### Epoller 句柄泄漏 Review

Epoller 持有 _epfd 独占资源却既无析构也未禁用拷贝，析构不关闭句柄；一旦后续按 Rule of Five 只补析构，拷贝对象还会重复 close 他人句柄。修复：新增 ~Epoller 调用 close(_epfd)，失败仅记录日志；显式 delete 拷贝构造与拷贝赋值（用户声明删除的拷贝构造会抑制隐式移动构造，故对象同时不可移动，符合当前用法）。补 <unistd.h>。

TestEpoller.cc 新增 CountOpenFds（读取 /proc/self/fd）与 64 次构造销毁的句柄计数断言，以及 is_copy_constructible / is_copy_assignable 的 static_assert。

变异验证：析构改为不关闭后句柄计数断言失败；移除头文件 delete 声明后两条 static_assert 失败。构造销毁 200 个对象的探针由 leaked=200 变为 leaked=0。make -C Http all check check-sanitize 退出码 0，严格 C++11 无告警。全项目仅 TestEpoller.cc 使用 Epoller，无其他拷贝或按值存放入容器的用法。

## Channel 与 Epoller 联合调试

- [x] 复现并说明 TestBasic 联调逻辑不成立之处。
- [x] 修复联合路径上的 Channel 生命周期隐患（析构自动注销 + 禁止拷贝）。
- [x] 修复 fd 被 close 后残留映射导致该 fd 号永久无法登记。
- [x] 将 TestChannel 对齐当前 API，TestBasic 重写为真实 TCP 回环 reactor。
- [x] 接入构建、修复失效的 check-logger，跑通全量与 sanitizer 回归。
- [x] 变异验证新断言有效，并修正其中一条无效断言。

### 联调 Review

联调入口 TestBasic.cc 原本把 fd=1（stdout）注册进 epoll，新连接 Channel 从未 EnableRead，回调设在监听 Channel 上却读新连接 fd，且没有事件循环，因此完全无法验证两模块协同。

发现并修复两处真实缺陷：

1. Epoller 用非拥有裸指针 map 持有 Channel，而 Channel 无析构函数。Channel 先销毁则 map 残留悬垂指针，下次 WaitEvent 访问即 use-after-free。修复：Epoller 新增幂等 `Detach`（noexcept，含 `it->second != channel` 身份校验），Channel 析构自动调用；Channel 同时禁止拷贝，否则拷贝对象析构会注销原对象的登记。
2. close 后 DeleteEvent 因 EBADF 失败并保留映射，内核把同一 fd 号复用给新描述符后，新 Channel 命中陈旧表项被判身份不符，该 fd 号永久无法登记，失败方式是未捕获异常终止进程。修复：DeleteEvent 仅在 EBADF 时照常清理映射，其它错误仍保留并抛出。

测试改动：TestChannel.cc 由真实 Epoller + eventfd 驱动，保留原有事件状态机与 16 种组合覆盖，新增析构自动注销、身份校验、同 fd 重新登记用例；TestEpoller.cc 新增 fd 号复用回归并同步 DeleteEvent 语义变更后的断言；TestBasic.cc 重写为监听套接字 + 事件循环 + 回显 + 对端半关闭回收的 reactor，并断言连接已回收、句柄数回到基准。

构建改动：全部测试收敛到 Http/Makefile，新增 check-channel / check-integration 及 sanitizer 变体；check-logger 不再委托已失效的 Http/Test/Makefile（该文件无此目标，make check 原本直接失败），删除该文件与 spdlog.mk 中的过期说明。

变异验证：移除析构注销、去掉身份校验、恢复保留陈旧映射三项变异均被检出。首轮发现 TestChannel 中"析构后可重新登记"断言未检出变异 1——栈上两个同类型对象析构后复用同一地址，身份比较把陈旧表项误判成本人；改用只看 fd 占用、与地址无关的 AddEvent 探测后检出。

make -C Http all check check-sanitize 退出码 0，严格 C++11 无告警，真实回环 TCP 路径在 ASan/UBSan 下无报告；git diff --check 通过。未运行 TSan，system spdlog/fmt 动态库未重新插桩。

## Socket 声明与实现分离

- [x] 将 Socket.hpp 缩减为纯声明，保留接口契约注释。
- [x] 新建 src/Socket.cc 承载全部实现，日志依赖下沉。
- [x] 更新 Makefile 依赖与源文件列表（SOCKET_SOURCES / REACTOR_SOURCES）。
- [x] 补全测试文件此前依赖传递包含的头文件。
- [x] 头文件自包含检查 + 全量回归 + sanitizer 回归。

### Socket 拆分 Review

头文件此前内联了全部实现，且经 `<arpa/inet.h>`/`<netinet/in.h>`/`<cerrno>`/`Logger.hpp` 向所有包含者传递依赖。拆分后头文件只保留类声明、`= delete` 的拷贝语义与接口契约注释，仅依赖 `<sys/socket.h>`/`<sys/types.h>`（声明中用到 `sockaddr`/`socklen_t`/`ssize_t`）与 `<cstddef>`/`<cstdint>`/`<string>`；实现侧的头文件依赖全部移入 src/Socket.cc。默认实参（`MAX_LISTEN_SIZE`、`= 0`、`nullptr`、`"0.0.0.0"`）按 C++ 规则留在头文件。

机械比对（`git show HEAD:Http/include/Socket.hpp` 去空白后与新实现对比）确认所有函数体逐字一致，差异仅为类框架行、默认实参与注释位置。

测试文件补依赖：TestSocket.cc 此前借 Socket.hpp 取得 ntohs/htonl/INADDR_ANY/sockaddr_in/errno，补 `<arpa/inet.h>`/`<netinet/in.h>`/`<sys/socket.h>`/`<sys/types.h>`/`<cerrno>`；TestSocketFaults.cc 补 `<cerrno>`/`<sys/socket.h>`/`<sys/types.h>` 与 `Logger.hpp`。

构建改动：新增 `SOCKET_SOURCES = src/Socket.cc src/Logger.cc` 并用于四个 Socket 目标，`SOCKET_DEPS`、`REACTOR_DEPS`、`REACTOR_SOURCES` 全部补入 src/Socket.cc，同步修正"纯头文件实现"的过期注释。`--wrap` 链接选项对所有目标文件生效，故 TestSocketFaults 的故障注入仍覆盖搬迁后的实现。

验证：头文件单测（临时 TU 仅包含 Socket.hpp）编译通过；check-logger / check-buffer / check-socket / check-socket-sanitize / check-integration 全部 PASS，严格 C++11 无告警。

遗留（本次未处理，与拆分无关）：check-epoller / check-channel 因 TestEpoller.cc、TestChannel.cc 仍调用已不存在的 `Epoller::DeleteEvent` 与 `Channel(int, Epoller&)` 而编译失败。已用不含 Socket 的编译命令复现同样错误，确认是 EventLoop 重构（Channel 改走 EventLoop 转发）后测试未同步的既有问题。

## Channel 声明与实现分离

- [x] Channel.hpp 缩减为纯声明，用前向声明断开与 EventLoop.hpp 的循环包含。
- [x] 新建 src/Channel.cc，实现体逐字搬迁。
- [x] Makefile 纳入 src/Channel.cc，并因新链接依赖纳入 src/EventLoop.cc。
- [x] 修复传递包含消失后暴露的编译错误（Epoller.cc 缺 Logger、EventLoop.cc 缺 <unistd.h> 等）。
- [x] 差异探针比对新旧实现行为，跑全量回归。

### Channel 拆分 Review

拆分前 Channel.hpp 内联全部实现，并与 EventLoop.hpp 互为循环包含（Channel.hpp 包含 EventLoop.hpp，EventLoop.hpp 又包含 Channel.hpp），当前之所以能编译，是靠 Epoller.hpp 里的 `class Channel;` 前向声明兜住 EventLoop.hpp 中的 `unique_ptr<Channel>`。拆分后 Channel.hpp 只保留类声明、私有 `EventCallback` 别名、`= delete` 的拷贝语义与前向声明 `class EventLoop;`，依赖收敛为 `<cstdint>` 与 `<functional>`，循环包含随之消失。

实现侧 src/Channel.cc 需要 `<sys/epoll.h>`（事件位）与 `EventLoop.hpp`（`_loop->UpdateEvent/RemoveEvent`，非虚调用，必须完整类型）。

机械比对（旧内联头文件去空白后与新实现对比）确认函数体逐字一致，差异仅为注释位置与类框架行。

构建改动：`REACTOR_SOURCES` 增加 src/Channel.cc 与 src/EventLoop.cc，`REACTOR_DEPS` 同步。src/EventLoop.cc 此前从未参与任何编译，为实现下沉后新引入的链接依赖，纳入后暴露并修复三处阻塞问题：`std::make_unique` 属 C++14（改为 `std::unique_ptr<Channel>(new Channel(...))`）、缺 `<unistd.h>` 导致 read/write 未声明、缺 `<thread>`/`<vector>`/`<system_error>`/`<stdexcept>` 等直接依赖。另为 src/Epoller.cc 补 `Logger.hpp`（原先靠 Channel.hpp 间接获得）。

验证：头文件自包含检查通过；差异探针覆盖 fd/事件状态访问器与 Handle 的全部 16 种事件位组合，旧内联实现与新实现的输出逐字节一致；make all 退出码 0，check-logger / check-buffer / check-socket / check-socket-sanitize / check-integration 全部 PASS。

遗留（本次未处理）：check-epoller / check-channel 仍因测试停留在 `Channel(int, Epoller&)` 与 `Epoller::DeleteEvent` 旧接口而失败，与拆分无关，失败集合与拆分前完全一致。

另发现一处待确认缺陷（未改动）：EventLoop 构造函数的初始化列表漏掉 `_ep`，`_ep` 保持空指针，而构造函数内 `_event_channel->EnableRead()` 会经 `Channel::Update()` 调回 `EventLoop::UpdateEvent` 解引用 `_ep`；且成员声明顺序把 `_ep` 排在 `_event_channel` 之后，即便补上初始化，Channel 构造期间回调仍会用到尚未构造的 `_ep`。需同时调整成员顺序或延后注册。

## 重建 reactor 测试覆盖

- [x] 提高测试 C++ 标准至 C++17（Makefile TEST_FLAGS），让 EventLoop.cc 的 make_unique 合法。
- [x] 修复 C++17 暴露的 TestBufferAllocation.cc sized-deallocation 编译失败。
- [x] 迁移 TestEpoller.cc：裸 Epoller 契约 + EventLoop 混合驱动的就绪路径。
- [x] 迁移 TestChannel.cc：新 API、转发链路探针、缺陷 3 的确定性探测。
- [x] 新建 TestEventLoop.cc：线程归属、任务队列、唤醒、事件优先于任务、Channel 登记端到端。
- [x] Makefile 新增 check-eventloop 与 sanitize 变体。
- [x] 全量验证与变异验证，结果写入下方 Review。

### reactor 测试覆盖 Review

前置改动：Makefile 的 TEST_FLAGS 由 `-std=c++11` 提升为 `-std=c++17`（src/EventLoop.cc 使用了 `std::make_unique`，属 C++14）。提升后全量重建，产品源码零警告；唯一的新失败在测试侧——TestBufferAllocation.cc 自定义了非 sized 的 `operator delete`，C++14 起编译器优先调用 sized 版本，GCC 报 `-Wsized-deallocation`，已补上两个 sized 版本（sized 版本转调非 sized 版本，写成 `::operator delete[](p, size)` 会自递归）。

TestEpoller.cc 采用混合驱动。新架构下 Channel 的监听位没有 public setter，没有 EventLoop 就无法让 Channel 带上监听位、进入内核就绪路径；而 EventLoop 的 `_ep` 是私有的，纯 `Loop()` 驱动会永久丢掉 `active` 向量的 size、元素身份、多描述符同时就绪这三类断言。因此：Group 1 用 `Channel(fd, nullptr)` 作可登记 token，测裸 Epoller 的边界契约（nullptr 参数、重复登记、未登记即修改/移除、同 fd 身份校验、Add 失败回滚、EBADF 静默清理，并新增原测试没有的 EINTR 分支）；Group 2 用真实 EventLoop 只负责设置监听位，登记与等待仍由独立的裸 Epoller 承担。原测试中依赖"析构自动注销"的 fd 复用回归段因该契约未实现而删除。

TestChannel.cc 按是否触碰 `_loop` 分成两类 Channel。新增四条转发链路探针（Update→Add 生效、Update→Modify 生效、Remove 生效、Remove 二次抛错），因为 `_ep` 私有不可见，只能用行为间接证明转发确实到位。缺陷 3（`~Channel` 不注销）改为确定性探测：先分配 b 再释放 a，保证两者同时存活过、地址必不相同，从而身份比较确定走到 mismatch，不依赖地址复用；ASan 下运行通过，说明该比较只读取失效指针值而未解引用被销毁对象。

TestEventLoop.cc 为新建，覆盖构造、线程归属、同线程立即执行、入队延迟执行、跨线程唤醒、批量执行、队列换出语义、事件优先于任务、Channel 登记/修改/移除端到端。看门狗用自定义 SIGALRM 处理器（`write` + `_exit`）而非裸 `alarm(5)`，因为默认动作只留下 "Alarm clock"，无法定位挂住的位置。每次 `Loop()` 前都保证有唤醒源（挂起任务，或已登记描述符就绪），`Remove` 之后补一个 `QueueInLoop` 作为独立唤醒源。

Makefile 新增 check-eventloop 与 check-eventloop-sanitize，并入 check 与 check-sanitize；clean-tests 同步纳入两个新产物。

验证：`make -k check` 全绿（logger / buffer / socket / epoller / channel / eventloop / integration）；`make -k check-sanitize` 在 ASan+UBSan 下全绿、无报告；变异验证 5/5 全部被捕获——`_ep` 置空导致段错误；`ev.events` 置 0 使就绪事件永不产生，`WaitEvent` 阻塞后由看门狗终止；去掉 AddEvent 失败回滚后抛 `fd already registered`；去掉身份比较后 `ExpectThrow` 断言失败；`Channel::Update` 改为空体后抛 `Channel is not registered`。每项变异均已还原并复跑确认。

遗留缺陷（本次未修，均已记录在案）：`Epoller::Detach` 有声明无定义，引用即链接失败，该契约零覆盖；`Channel::~Channel` 为空，不注销登记，与头文件注释及 `Detach` 注释宣称的行为不符；`Epoller::UpdateEvent(nullptr)` 与 `EventLoop::UpdateEvent(nullptr)` 解引用空指针；EventLoop 无析构函数，每次构造泄漏一个 eventfd；`Loop()` 只跑一轮，TestBasic.cc 仍是空壳，check-integration 不验证行为。

## 把 TestBasic.cc 变成 echo 服务器 + 客户端的自动化测试

- [ ] 重写 `Http/Test/TestBasic.cc`：EchoServer（accept/echo/延迟回收）+ 客户端场景
- [ ] 编译零警告（`-std=c++17 -Wall -Wextra -Werror -pedantic`）
- [ ] 单独运行退出码 0，stderr 无 `TIMEOUT:`
- [ ] `make -C Http -k check` 全绿
- [ ] `make -C Http -k check-sanitize` 全绿
- [ ] 修正 TestEventLoop.cc 中已失效的 `KNOWN DEFECT #2` 断言
- [ ] 修正 TestEpoller.cc 中关于 `Detach` 的过时措辞
- [ ] 变异验证 4 项
- [ ] 写入 Review 小节

## 当前执行框架分析（2026-09-17）

- [x] 阅读目录、历史经验、核心 Reactor 实现与构建入口。
- [x] 核对测试入口、网络收发、任务唤醒及定时器接入情况。
- [x] 通过针对性现有测试验证执行路径，并完成独立审查。
- [x] 给出当前实现的调用链、线程模型与边界说明。

分析范围：以工作区当前代码为准，仅追加本任务记录，不修改业务实现；区分已实现框架、测试示例和未接入模块。

### Review

已按工作区当前源码核对 Reactor、TCP 回显测试入口、跨线程任务和时间轮模块，并完成独立只读审查。EventLoop 为单轮分发，测试外层 while 驱动；TimerWheel 尚未被主流程实例化，Buffer 尚未用于回显连接，HTTP 请求示例仅在 Preknowledge。

验证：make -C Http check-eventloop check-integration；check-eventloop 通过；check-integration 在 TestBasic.cc:487 的 server.ConnCount() == 0 断言失败。当前 TestBasic.cc:224 的 EOF 分支含 MUTATION 4 注释且直接返回，未请求关闭连接。本次为框架分析，未修改业务源码或修复测试。

## TimerQueue 头文件与源文件分离 + 测试（2026-09-17）

- [x] 复现编译断裂并留证：单独包含 TimerQueue.hpp 报 `task_t`/`TimerWheel` 未声明、`read` 未声明；src/EventLoop.cc 报 `invalid use of incomplete type 'class EventLoop'`。
- [x] TimerQueue.hpp 只留声明：删掉 `#include "EventLoop.hpp"`，改为前向声明 EventLoop 与 Channel，打破循环包含；仅保留 `<cstdint> <functional> <memory> <unordered_map> <vector>`。
- [x] 新建 src/TimerQueue.cc 承载实现，补齐 `<unistd.h>`（read）与 `<stdexcept>`（runtime_error）等直接依赖。
- [x] 归一 timeout 边界：0 → 1 个 tick；超过 capacity → 夹紧到 capacity（不再取模）。
- [x] ReadTimerfd 返回到期次数，读回调按次数推进等量 tick。
- [x] ~TimerWheel 补齐 `_timer_channel->Remove()` 与 `close(_timerfd)`。
- [x] `Concel` 更名 `Cancel`（TimerTask / TimerWheel / EventLoop 三处）。
- [x] 修正 EventLoop 的 TimerRefresh / TimerCancel 转发互换。
- [x] 新建 Test/TestTimerQueue.cc（17 个用例）并接入 Makefile（REACTOR_SOURCES 与 check-timerqueue 及 sanitize 变体）。
- [x] 验证 check、check-sanitize、变异验证 6 项、git diff --check。
- [x] 记录验证结果与经验。

### Review

分离本身是修复而非美化：`TimerQueue.hpp` 内联实现并向 `EventLoop.hpp` 回指，形成循环包含，导致整个 reactor 测试套件（TestEpoller / TestChannel / TestEventLoop / TestBasic）连同 `make all` 都无法构建。分离后头文件自包含，`-fsyntax-only` 对「单独包含 TimerQueue.hpp」「单独包含 EventLoop.hpp」「src/TimerQueue.cc」「src/EventLoop.cc」四项全部通过。

搬移忠实度用机械比对保证：把原头文件与新 .cc 各自做去首尾空白归一后取行集合做差，剩余差异全部是声明行、`TimerTask::`/`TimerWheel::` 限定符、`Concel`→`Cancel` 改名以及刻意叠加的修复行，没有任何函数体被漏搬或改写。

叠加的修复：`NormalizeTimeout` 在 `TimerAddInLoop` 构造任务前归一（存进 TimerTask 的是归一后的值，`TimerRefreshInLoop` 重算槽位因此天然一致）；`ReadTimerfd` 返回到期次数；`~TimerWheel` 补 `Remove()` 与 `close()`。此外修正了 `EventLoop` 的 Refresh/Cancel 转发互换。

验证：`make -k check` 除 check-integration 外全绿（logger / buffer / socket 8 组 / socketfaults 114 项 / epoller / channel / eventloop / timerqueue）；`make -k check-sanitize` 同样全绿且 ASan+UBSan 无任何报告。check-integration 的失败是 TestBasic.cc:224 里上轮变异验证残留的 `MUTATION 4`（EOF 分支不回收连接）导致的，与本次改动无关，未触碰。

修复的旁证：`TimerWheel` 加进 `EventLoop` 后，TestEventLoop 的 `DestructorReleasesFd`（反复构造 64 个 EventLoop 并断言 fd 数不变）本应因每次泄漏一个 timerfd 而失败，只是它同时被编译断裂挡住。变异验证把它单独放出来证实了两点——它确实覆盖该缺陷，且修复后确实转绿。

变异验证 6/6 全部被捕获：去掉 0→1 钳位 → `TimeoutZeroFiresOnNextTick`；去掉 capacity 夹紧 → `TimeoutAboveCapacityIsClamped`；析构不 close → `DestructorReleasesTimerfd`（同时打红 TestEventLoop 的 `DestructorReleasesFd`）；析构不摘除登记 → `DestructorUnregistersChannel`；读回调只推进一格 → `CatchUpAfterStall`；交换 EventLoop 转发 → `LoopDrivenCancel` 与 `LoopDrivenRefresh`。每项变异均已还原并复跑确认。

两处需要记录的执行细节：M2 首次变异写成 `return timeout;` 后 `capacity` 变成未使用参数，被 `-Werror=unused-parameter` 拦下——变异验证中的「编译失败」必须换一种保留参数使用的等价变异，不能当成通过。M4 的实际捕获路径与预期不同：析构释放的 fd 号被复用给新 timerfd，新 Channel 由 `make_unique` 分配到与旧对象相同的地址，`RequireRegistered` 的身份比较因此通过，真正拦下的是内核返回 `ENOENT`（`epoll_ctl failed, op=2`），即陈旧表项最终由内核兜住而非身份校验。

当时的遗留缺陷已在下面一节修掉。另需注意 `TestBasic.cc` 中的 `MUTATION 4` 未还原，`check-integration` 当前为红。

## 同 id 重复添加：索引错删与替换语义（2026-09-17）

- [x] 论证删除索引为什么只能挂在 `~TimerTask` 上，而不能改在 `Tick()` 里做。
- [x] 索引清理改为按控制块比对身份（`owner_before` 双向比较）。
- [x] 同 id 重复添加改为替换语义：旧任务取消并摘除记录，只有新任务触发。
- [x] 新增 `ReAddSameIdReplacesOldTask` 与 `IndexSurvivesOldTaskCleanup` 两条用例（19 个用例）。
- [x] 变异验证 M7、M8 两项。
- [x] 复跑 check 与 check-sanitize 确认无回归。
- [x] 记录验证结果与经验。

### Review

先纠正一个错误：上一节写的修法「把任务自身的 `TimerTask*` 一并捕获进 lambda 做身份比对」按字面写不成立。`_timers` 存的是 `weak_ptr`，而 `_release()` 在 `~TimerTask` 里执行，此刻强引用计数已经是 0，`weak_ptr::lock()` 必然返回空，拿不到可比较的裸指针。

看似更干净的改法是在 `Tick()` 里按 id 删索引——那时任务还活着，`lock()` 可用，身份比较一目了然。但它会改变语义：刷新过的任务同时挂在旧槽与新槽上，旧槽先到期就把索引摘了，此后到新槽到期之前它既不能刷新也不能取消，却照样触发。对「收到数据就刷新空闲超时、连接关闭就取消」的服务器而言，取消静默失效是最坏的一类缺陷。析构才是「最后一根强引用消失」的唯一时刻，所以删除只能挂在析构上，于是问题收敛为：use_count 已为 0 时如何判断索引里那条记录还是不是我。

三种写法权衡后选 `owner_before`：标准库为「两个 weak_ptr 是否指向同一控制块」准备的正是它，零额外状态、不用改 map 值类型，且是正面按身份判断、不依赖调用时机。（备选是世代计数器，可读性更好但要多一份状态；`expired()` 反向判断代码最短，却依赖「`_release()` 只在析构里被调用」这个上下文做反推，将来多一个调用点就静默失效。）

替换语义在 `TimerAddInLoop` 里实现：找到同 id 的旧记录后先 `Cancel()` 再 `erase`，然后才写入新记录。旧任务此刻仍挂在自己的槽位上，要到该槽到期才析构；它析构时执行的清理会因身份不符而跳过，不会删掉新任务的登记。顺带在头文件的 `@file` 注释里写明重复添加等于重启，并指出「只想重新计时、不换回调」时用 `TimerRefresh` 更省。

新增两条用例：`ReAddSameIdReplacesOldTask` 断言旧任务在其原定到期点不触发、只有新任务触发；`IndexSurvivesOldTaskCleanup` 让两个旧任务在 `_tick == 1` 销毁并各执行一次清理，再断言对 id=1 的取消仍然生效、对 id=2 的刷新仍然生效（刷新后原定 slot 5 不触发、slot 6 才触发）。

验证：`make -k check` 与 `make -k check-sanitize` 除既有的 check-integration（`MUTATION 4`）外全绿，ASan+UBSan 无报告；变异验证 M7（索引清理不校验身份）在 `IndexSurvivesOldTaskCleanup:293 new1_fired == 0` 处被捕获，M8（重复添加不做替换）在 `ReAddSameIdReplacesOldTask:263 old_fired == 0` 处被捕获；`git diff --check` 通过；头文件独立严格语法检查通过。

一次未复现的构建异常：某次 `make -k check-sanitize` 在 `.build/TestChannel-sanitize`、`.build/TestEventLoop-sanitize`、`.build/TestTimerQueue-sanitize` 三个目标上报 Error 1，当时被自身的 grep 过滤掉了原始报错。随后同一命令重跑全绿，`touch` 三个源文件后强制从零重建这三个目标也全绿（退出码 0，磁盘 21G 可用），三次均无法复现，故按瞬时故障记录，未查出原因。

## 简单服务端示例

- [x] 补齐 TestServer：8080 监听、多连接非阻塞回显、短写续发和延迟回收。
- [x] 添加独立 make server 入口，不改变默认测试目标。
- [x] 编译并用回环客户端验证回显、半关闭和重复连接，记录结果。

实现约定：单文件示例，沿用现有 EventLoop/Channel/Socket；有待发送数据时暂停读取，避免无限累积输出。

### Review

TestServer 为单文件 TCP 回显示例，监听 8080，使用监听 Channel 接收连接；每连接暂存最多 4096 字节待发送数据，短写/EAGAIN 通过写事件续发；队列延迟注销和销毁连接。新增 make server 入口并纳入既有清理规则。严格 C++17 警告构建通过；本机验证通过 8 路并发、24 次 256 KiB 二进制回显与半关闭、10 次空连接，fd 数量恢复基线。测试进程已停止。

## Buffer 完整头源分离（2026-09-18）

- [x] 确认迁移全部带函数体的头文件定义，保留枚举、默认参数和默认拷贝声明。
- [x] 迁移实现，核对函数体与接口签名保持一致。
- [x] 运行综合测试、分配失败测试及多编译单元链接检查。

### Review

已迁移 13 个带函数体的定义到 Http/src/Buffer.cc，函数体归一化核对一致；头文件保留声明、枚举、默认参数、默认拷贝声明和成员初始化。现有构建规则无需调整。make -C Http check-buffer、Buffer 综合 sanitizer 测试、多编译单元重复包含/链接及默认参数测试均通过；git diff --check 通过。未改动其他模块。

## Connection 异常清理实现（2026-09-18）

- [x] 核实事件分发与定时器清理约束。
- [x] 实现异常后的延迟释放及必要的连接生命周期基础。
- [x] 添加 socketpair 测试，验证异常隔离、延迟销毁、正常读写。
- [x] 运行构建和相关测试，记录结果与限制。

设计：HandleError 在所属循环线程安排一次释放；ReleaseInLoop 先注销事件、取消定时器、关闭 fd，再通知业务与服务器。关闭回调异常不能跳过服务器通知。bad_alloc 向上传播。连接表必须持有已登记连接，EventLoop 必须比连接活得更久。

### Review

实现 HandleRead 普通/未知异常后的延迟清理；bad_alloc 向上传播。HandleError 通过 QueueInLoop 保活并推迟释放，ReleaseInLoop 先注销 Channel、取消已启用定时器、关闭 Socket，再分别通知业务和服务器。通知异常不能跳过另一个通知，重复关闭不会重复通知。补齐可运行验证所需的构造、建立连接、收发、正常关闭及基础访问器。构造函数调整为 (EventLoop*, uint64_t id, int fd)，新增 SetServerClosedCallback；ID 返回类型与字段一致。

验证：make -C Http all 通过；check-connection、check-connection-sanitize、check-channel、check-eventloop 通过。真实 socketpair 验证普通与非标准回调异常、Buffer::Write length_error/bad_alloc 注入、消息 bad_alloc、关闭回调异常、同循环健康连接回显、延迟销毁和弱引用释放、128KiB 响应半关闭排空。ASan/UBSan 无报告。临时副本变异：删除 catch 中 HandleError 被 !bad->IsConnected() 断言捕获；QueueInLoop 改 RunInLoop 被 before_release 断言捕获。正式源码未参与变异。本次 Connection 文件空白检查通过；仓库整体 diff --check 报既有 Buffer.hpp:114 行尾空格，未修改。

边界：EnableInactiveRelease/CancelInactiveRelease/SwitchProtocol 等独立接口仍为原有声明，本次未实现，不能据此认为整个 Connection 模块已完工。定时器取消路径未作集成验证。bad_alloc、任务投递及 epoll 基础设施错误向上传播，上层须停止使用故障连接或关停循环；不保证资源耗尽后可恢复。连接表应持有已登记连接到关闭通知；EventLoop 必须比连接活得更久。未运行全仓 check；TestBasic 仍存在既有 MUTATION 4。

## Connection 统一日志输出

- [x] 确认使用项目 LOG_ERROR，日志异常不得打断清理。
- [x] 修改统一错误报告函数并运行已有连接测试。

### Review

ReportFailure 改为 LOG_ERROR，直接包含 Logger.hpp 并删除 cstdio。保留 noexcept 与内部 catch(...)，日志失败仍返回并继续清理。make -C Http check-connection 通过；输出确认带项目统一 error 等级、时间与源文件位置。

## 发送结果分支可读性

- [x] 确认按负数、零、正数拆分，保持原有行为。
- [x] 修改 HandleWrite 并运行连接测试。

### Review

HandleWrite 分开处理 sent < 0、sent == 0、sent > 0；只在负数分支读取 errno，处理行为保持不变。make -C Http check-connection 通过。

## 取消超时与协议切换（2026-09-19）

- [x] 明确接口语义、线程与所有权约定。
- [x] 实现真实定时器取消与协议配置切换。
- [x] 验证跨线程参数寿命、回调内切换、缓冲保留与超时取消/重新启用。
- [x] 运行普通及 sanitizer 测试、反向验证并记录。

协议切换在 CONNECTING/CONNECTED 生效，不重复调用建立回调，不重新处理已有输入；业务按需处理未读数据。新参数按值持有，准备完成后无抛移动替换，服务器关闭回调保持不变。执行业务回调前保留局部副本，以支持回调内切换；旧上下文引用在切换后不可继续使用。

### Review

CancelInactiveRelease 通过 RunInLoop 保活并调度，InLoop 调用 TimerCancel 后关闭刷新标志；重复取消无副作用，但不能撤销已经发生的超时关闭。SwitchProtocol 按值接收并移动捕获参数，在所属线程检查 CONNECTING/CONNECTED 后替换上下文和四个业务回调，保留服务器关闭回调与缓冲区，不重复执行连接建立通知，不自动消费既有输入。回调调用点持有 std::function 局部副本，支持回调内立即切换；协议状态应放在 context 或共享业务对象中，而非依赖可变 Lambda 按值捕获状态跨调用累积。

验证：严格 C++17 编译、check-connection、check-connection-sanitize 全部通过。新增测试覆盖跨线程取消/重复取消/重新启用到期、跨线程临时参数生命周期、新协议各回调与服务器关闭通知、回调内切换后原捕获有效及未读输入保留。临时源码变异取消 TimerCancel 被连接存活断言捕获；省略 context 替换被新上下文断言捕获。正式源码未用于变异，未更改 TimerWheel 实现。

## Connection 未覆盖分支补测（2026-09-19）

面向 Connection.cc 逐分支核对现有 9 个用例，补齐未被任何断言触及的路径。目标是让每条新增断言都对应一个可执行的变异验证，避免"看起来测了、实际抓不到"的用例。

- [x] 补测试脚手架：看门狗（自定义 SIGALRM 处理器）、CountOpenFds 前后比较、main 的 try/catch。
- [x] 构造函数三条失败路径：loop 为空、fd<0 抛 invalid_argument；SetNonBlock 失败抛 runtime_error。
- [x] EnableInactiveRelease 的 timeout<=0 参数校验。
- [x] EstablishedInLoop 的 `_status != CONNECTING` 守卫：重复建立与关闭后建立。
- [x] 连接建立回调抛异常（普通异常清理、bad_alloc 传播两条）。
- [x] HandleEvent 的任意事件回调抛异常。
- [x] SendInLoop 的非 CONNECTED 守卫：CONNECTING 阶段与已释放阶段。
- [x] ~Connection 的兜底 `_channel->Remove()`：用同 fd 号重登记探针验证登记确已清除。
- [x] 从未 Established 直接 ShutDown：两处 `_registered` 判断的假分支。
- [x] 两个关闭回调都抛异常时的聚合与隔离；server_closed 抛 bad_alloc 的重抛。
- [x] SwitchProtocolInLoop 的生命周期守卫：CONNECTING 允许、释放后忽略。
- [x] 运行普通与 sanitizer 两套测试，逐条做临时源码变异验证，并更新本文档的 Review。

已核对但决定不投入的分支（记录理由，避免后续重复评估）：
- `EnableInactiveReleaseInLoop` 的 `_release_pending || _released` 守卫：删掉后 `HandleError` 自身的同名守卫会兜住，关闭通知仍只有一次，黑盒无可观测差异，变异验证必然存活。
- `HandleWrite` 的 `sent == 0`、`HandleRead` 的 `_status != CONNECTED`：黑盒不可稳定触发，或需为 Connection 目标新增 `--wrap=send`，性价比低于收益。
- `HandleEvent` 的 `TimerRefresh`：可观测但需要与 timerfd 的秒级节奏对齐的等待，预算明显高于本次其余用例之和。
- `EnableInactiveReleaseInLoop` 中 `weak.lock()` 失败与 `catch(...) -> terminate`：前者要求构造"任务存活但连接已销毁"的时序，后者一旦触发即终止进程，均不适合放进常规用例。

### Review

新增 12 条用例（共 21 条），全部使用项目既有风格：`assert` 断言、`Step(loop)` 驱动、socketpair 造真实对端。同时补齐测试脚手架：自定义 SIGALRM 处理器的看门狗（预算 30 秒，照 TestTimerQueue/TestEventLoop 的写法，不用裸 alarm）、`CountOpenFds` 前后相等比较、`main` 的 try/catch（把异常逃逸型失败变成可定位的 "tests failed" 而不是 std::terminate）。

用例清单：ConstructorRejectsInvalidArguments、ConstructorPropagatesNonBlockFailure、EnableInactiveReleaseRejectsZeroTimeout、EstablishedIsIdempotent、ConnectedCallbackFailureClosesConnection、ConnectedCallbackBadAllocPropagates、AnyEventCallbackFailureClosesConnection、SendBeforeEstablishedIsDropped、DestructorUnregistersUnclosedConnection、ShutDownBeforeEstablishedCloses、CloseCallbackFailuresAreAggregated、ServerClosedBadAllocPropagates、SwitchProtocolRespectsLifecycle。

两处构造上的取舍值得记录：

- `SetNonBlock` 失败路径用 `O_PATH` 描述符触发（`F_GETFL` 正常、`F_SETFL` 返回 EBADF），前提已实测并写进用例断言。这避免了伪造陈旧 fd 号（可能误关被复用的无关描述符）与为 Connection 目标新增 `--wrap=fcntl` 两种更重的做法。
- `~Connection` 兜底注销用同 fd 号重登记探针验证。此前设想的"对端 EOF + fd 已关闭 + 后续 Step 不崩溃"三条断言全部由 `~Socket` 满足，**无法**区分该分支是否被执行。

验证：严格 C++17 编译通过；`make check-connection` 与 `make check-connection-sanitize` 全部通过，ASan/UBSan 无报告（含 128 KiB 排空与两次 1.2 秒空闲超时等待）。

变异验证共 16 项，全部在 `/tmp` 的源码副本上执行，正式源码哈希前后一致。逐项结果：M1 去掉 loop 为空校验、M2 忽略 SetNonBlock 失败、M3 去掉 timeout<=0 校验、M4 去掉重复建立守卫、M5/M5b 建立回调异常不清理/向外逃逸、M6/M6b 事件回调异常不清理/向外逃逸、M7 去掉非连接态发送守卫、M8 析构不兜底注销、M9 释放时无条件注销、M11/M13 关闭回调 bad_alloc 不重抛/只报告第一个异常、M12a/M12b 协议切换守卫删除/收窄——均被新增断言或 `main` 的异常捕获拦下。其中 M9 另做了归因实验：取消 `ShutDownBeforeEstablishedCloses` 的调用（保留引用以避开 `-Werror=unused-function`）后变异转为存活，确认它是唯一捕获者。M10（不执行服务器关闭通知）被既有用例 `MessageFailureIsIsolated` 捕获，新增的聚合用例在该点上不增加检测力，仅固定"两个关闭回调同时抛异常"这一场景。

已知限制：关闭回调"两个异常都被报告"只体现在日志文本上，无可断言的可观测差异，因此本轮不覆盖该点（M13 覆盖的是 bad_alloc 的重抛语义）。`HandleEvent` 的 `TimerRefresh`、`HandleWrite` 的 `sent == 0`、`HandleRead` 的状态守卫仍未覆盖，理由见上文"决定不投入"清单。

未改动 `Connection.cc` 及其他模块源码。

## Acceptor 补全与 Connection 超时校验修复（2026-09-19）

起因是上一轮审查提出的「有没有实质性的错误」。结论是 Acceptor 有实质性缺陷，Connection 有一处类型变更引入的静默回归。

### 计划

- [x] 用探针程序实测确认 Acceptor 的双次创建缺陷（探针放 `/tmp`，不涉及仓库文件）
- [x] 重写 `include/Acceptor.hpp`：补 `<cstdint>`、回调签名改为 `void(int fd)`、补析构与 `GetListenFd`/`GetPort`
- [x] 重写 `src/Acceptor.cc`：只调一次 `CreateServer` 并检查返回值、构造 `_channel` 并登记、实现 `HandleRead`
- [x] `Connection::EnableInactiveRelease` 参数由 `uint32_t` 改回 `int`
- [x] 新增 `Test/TestAcceptor.cc`
- [x] `TestConnection.cc` 补负数超时用例
- [x] Makefile 接线 `check-acceptor` / `check-acceptor-sanitize` 并并入 `check`
- [x] 变异验证
- [x] 回归与源码完整性核对

### Review

**Acceptor 的原始缺陷（已实测确认）**：构造函数先调 `Socket::Create()` 再调 `Socket::CreateServer()`，而 `CreateServer` 内部第一句就是 `if (!Create()) return false;`，`Create()` 在描述符已存在时置 `errno = EALREADY` 并返回 false，于是 `ReuseAddress`/`SetNonBlock`/`Bind`/`Listen` 被整体跳过。探针实测：`Create()=1`，`CreateServer()=0 (EALREADY)`，本地端口 `0`，`SO_ACCEPTCONN=0`；单独调用 `CreateServer()` 则端口 44361、`SO_ACCEPTCONN=1`。再加上两个返回值都被丢弃，失败完全静默。此外 `SetAcceptCallback()` 无参数、`_channel` 从未构造、`HandleRead` 为空，且 `Acceptor.cc` 不在任何构建目标里，因此从未被编译过一次——我用项目同款严格参数单独跑 `-fsyntax-only` 是通过的，也就是说这些缺陷连 `-Werror` 都不会触发。

**最终落地的实现**在我初稿基础上又被扩展过：`AcceptedFd` RAII 守卫负责「回调抛出前先交出所有权，避免重复关闭已复用的描述符」、构造/析构/`SetAcceptCallback`/`HandleRead` 增加线程归属校验、`QueryBoundPort` 失败改为抛 `std::system_error` 而不是回退到请求值。这些扩展的用例与接线也一并到位，本次变异验证是针对这个最终状态重跑的。

**Connection 的回归**：`git diff` 确认 `EnableInactiveRelease` 的参数在本会话中从 `int` 被改成 `uint32_t`，而校验条件 `timeout <= 0` 原样保留。`int` 时代它能拦住负数，改成无符号后负数回绕成 4294967295，`TimerQueue.cc:20` 的 `return timeout > capacity ? capacity : timeout;` 把它钳到容量 60——传 `-1` 不报错，而是静默变成 60 秒超时。修法是参数改回 `int`（对外边界用有符号、保留负数校验），私有的 `EnableInactiveReleaseInLoop` 继续用 `uint32_t` 并在调用处显式 `static_cast`。全仓只有测试调用该函数且都是 int 字面量，改签名不影响调用方。

**测试**：新增 `Test/TestAcceptor.cc`，14 条顶层用例（另有 2 个内部辅助用例由它们调用），沿用项目既有风格——`assert`、自定义 SIGALRM 看门狗（sa_flags=0，只做异步信号安全的 write + `_exit`）、`CountOpenFds` 只做前后相等、`Step(loop)` 驱动、真实回环端口 + 真实 `connect`。覆盖：构造失败路径（空 loop、端口被占）、监听套接字配置（`SO_ACCEPTCONN`、非阻塞、端口可连）、单连接与 8 连接批量 accept、空闲不触发与 EAGAIN 终止、空回调释放描述符、回调异常隔离、`bad_alloc` 传播、回调替换、同一 fd 被复用后异常路径不误关、`getsockname` 失败回滚、错误线程被拒、析构注销登记。`TestConnection.cc` 补 `EnableInactiveReleaseRejectsNegativeTimeout`。

**两处构造上的取舍**：

- 端口被占用用例里的占用方必须用裸 `socket` + `bind` 且不设 `SO_REUSEPORT`。`Socket::ReuseAddress` 会同时打开 `SO_REUSEADDR` 与 `SO_REUSEPORT`，而 `SO_REUSEPORT` 要求所有绑定方都设置它；占用方若也开了，`bind` 就不会失败，用例会静默失效。
- 析构注销仍用「同号描述符重新登记是否成功」探测，而不是「端口不可连」——后者只能证明描述符已关闭，由 `~Socket` 即可满足，无法区分 `Remove()` 是否执行。

**变异验证共 12 项**，全部在 `/tmp` 的源码副本上执行，正式源码哈希前后一致（仅 `Acceptor.hpp`/`Acceptor.cc`/`Connection.hpp`/`Connection.cc` 四个文件变化，其余逐字节相同）。逐项结果：M1 删空指针校验（段错误）、M2 忽略 `CreateServer` 返回值（断言中止）、M3 恢复先 `Create` 再 `CreateServer`（即原始缺陷，异常逃逸到 `main`）、M4 `HandleRead` 不 accept、M5 只 accept 一次、M6 EAGAIN 后不停继续空转（看门狗超时）、M7 空回调泄漏描述符、M8 去掉回调异常隔离（异常逃逸到 `main`）、M9 吞掉 `bad_alloc`、M10 回调 setter 不赋值、M11 析构不注销登记、M12 `EnableInactiveRelease` 改回 `uint32_t`——均被捕获。

其中两项在首轮验证中暴露了问题，已修正后重验：

- **M2 首轮存活**。原因是 `std::system_error` 派生自 `std::runtime_error`：忽略 `CreateServer` 返回值后，构造会带着已置 -1 的描述符继续走到 `EnableRead`，在内核 `epoll_ctl` 处抛 `system_error`，被 `catch (const std::runtime_error &)` 一并接住，「在 `CreateServer` 处拒绝」的契约其实没被验证。改为单独 `catch (const std::system_error &)` 并将其判为失败后，该项被捕获。这正是既有经验「断言必须落在能区分变异的观测点上」的又一次应验。
- **M8 与 M12 首轮是编译失败**，按既有经验不算法被捕获。M8 删掉 try/catch 后 `ReportFailure` 变成未使用函数，被 `-Werror=unused-function` 拦下，改用 `(void)&ReportFailure;` 保留引用后重验。M12 需要同时改头文件，改用 `-I/tmp/mut/include` 优先命中副本，失败点直接打出 `TestConnection.cc:504: Assertion 'rejected' failed`。

**回归结论**：`make check` 与 `make check-sanitize` 中除 `check-integration`（`.build/TestBasic`）外全部通过，ASan/UBSan 无报告。`TestBasic` 的失败 `RunIntegration:487 server.ConnCount() == 0 errno=11` 在**改动之前**就已稳定复现三次，且 `TestBasic.cc` 未被改动，属既有问题，与本次修改无关，未纳入本次范围。本轮的回归判断标准因此是「与改动前基线逐项一致」而非「全绿」。

**遗留**：`Http/src/TestServer.cc` 是早期的随手 demo，用的是旧版 `Connection` 接口（`Connection(fd, loop)`、`conn->socket`、`conn->channel`），与当前接口已经不匹配，也没有被 `make check` 覆盖。它不影响本轮结论，但作为「服务器模块」的雏形已经过时。

## 日志异常兜底统一到宏入口

- [x] 把安全兜底并入 `HTTP_LOG_AT`，所有 `LOG_*` 一律不抛。
- [x] 删除 `LOG_ERROR_SAFE`，替换 Acceptor 与 TestLogger 的调用点。
- [x] `Connection::ReportFailure` 去掉自建 try/catch。
- [x] 改写 TestLogger 中依赖旧契约的断言，用例改名。
- [x] 验证构建、日志/Acceptor/Connection 回归、ASan/UBSan、头文件自包含。
- [x] 变异验证两项，并核对正式源码未被波及。
- [x] 记录验证结果。

### Review

**本轮取代上面「Logger 安全日志宏」一节的结论**：`LOG_ERROR_SAFE` 已删除，原 `LOG_*` 宏的异常语义从「会抛」改为「一律不抛」，全项目只剩一种写法。

**为什么要统一**：改前有三套写法在做同一件事——裸 `LOG_ERROR`（多数模块，会抛）、`LOG_ERROR_SAFE`（只有 `Acceptor.cc` 两处在用，不抛）、`Connection.cc` 自建的 `ReportFailure(where, message) noexcept` 里包 try/catch（不抛）。语义不一致，且分散在三处。

**兜底为什么必须写在宏里**：宏实参在调用方求值，`Logger::Log` 成员函数包不住它。所以 try 覆盖单例获取、等级判断、参数求值与后端写入四处。

**与 spdlog 的分工**（读源码确认，不是推测）：`logger.h:30-50` 的 `SPDLOG_LOGGER_CATCH` 已经在 `log_` 内部接住 `std::exception` 并转给 `err_handler_`，后端那一段本来就安全。但它对**非 std 异常**在调用 `err_handler_` 之后会重新抛出，且参数求值、单例获取、`ShouldLog` 都在它的保护范围之外——宏里补的就是这三处。因此 catch 必须是 `...`，收窄成 `std::exception` 就退化。

**明确不做**：不启用 `SPDLOG_NO_EXCEPTIONS`，它的名字看着正是「不抛异常」，实现是 `common.h:97-104` 的 `printf` + `std::abort()`，对服务器是从「可能抛异常」退化成「直接终止进程」；不安装自定义 `set_error_handler`，因为 `err_handler_` 是从 `logger::log_` 内部被调用的，自定义 handler 回写 `LOG_ERROR` 会递归，只写 stderr 又与默认等价，而默认实现已带 1 条/秒限速；不动 `src/TestServer.cc`（旧接口 demo，不在任何 `check` 目标里，改了无法验证）。

**测试改写**：`TestLogger.cc` 的 `TestSafeErrorLogger` 里那段 `assert(propagated && ...)` 断言的正是本轮要推翻的旧契约，已改为 `assert(!propagated && evaluated == 1)`，三档异常（`runtime_error`／`bad_alloc`／`throw 42`）合并到同一次调用上，用例改名 `TestNonThrowingLogger`。过滤与关闭时不求值参数的两条断言不受影响。

**验证结果**：`make -j2 all check-logger check-acceptor check-connection` 全绿。`make -k check` 与 `make -k check-sanitize` 除 `check-integration`／`check-integration-sanitize` 外全部通过，ASan/UBSan 无报告；失败原文仍是 `RunIntegration:487 server.ConnCount() == 0 errno=11`，与改动前基线逐字相同，属既有问题。`Logger.hpp` 单独 include 的 `-fsyntax-only` 通过。`grep LOG_ERROR_SAFE` 在 `src/include/Test` 下归零；`src/` 里剩余的 catch 逐条确认都是业务异常处理（回调隔离、`bad_alloc` 传播、Epoller 回滚、`~Logger` 兜底、超时回调里的 `std::terminate`），没有残留的日志包装。

**变异验证**（只在 `/tmp/mutlog` 的副本上做，正式源码未被写入；结束后副本还原与正式 `Logger.hpp` 逐字节一致）：M1 删除宏内 try/catch → 捕获，退出码 134；M2 把 `catch (...)` 收窄为 `catch (const std::exception &)` → 捕获，退出码 134。两项都精确死在 `TestLogger.cc:228` 的 `Assertion '!propagated && evaluated == 1' failed`，即新契约那条断言，不是别处。

**未覆盖的路径（如实记录）**：单例初始化失败与底层 sink 写入失败这两条无法在不改生产代码的前提下注入，本轮不声称覆盖，与上一轮口径一致。另外「try 只包住后端、把 `ShouldLog` 留在 try 外」这种写法**不可观测**（`ShouldLog` 只在等级枚举非法时抛，而枚举值被 `Level`／`ToSpdlogLevel` 全量映射），没有为它编造用例。**遗留**：`src/TestServer.cc` 仍用旧接口，未并入本轮统一。

## Acceptor 单层异常处理（2026-09-20）

- [x] 用 handed_off 标记交接，删除嵌套异常处理。
- [x] 验证构建、普通及 sanitizer 测试、交接标记变异。
- [x] 记录结果。

### Review

DispatchAcceptedFd 使用一层 try/catch 与 handed_off 标记：复制回调失败时关闭 fd，空回调关闭后返回，调用前标记交接。交接后不重复关闭，bad_alloc 继续传播，其余异常保留原日志行为。未修改公共接口和日志模块。

make -C Http -j2 all check-acceptor check-acceptor-sanitize 全部通过，ASan/UBSan 无报告，本次文件 diff --check 通过。临时副本 /tmp/acceptor-handoff.o5kMXP 删除 handed_off = true 后编译成功，CallbackExceptionDoesNotCloseReusedFd 在 fd 有效性断言处失败（退出码 134）；正式源码未参与变异。

## Acceptor 移除端口状态（2026-09-20）

- [x] 删除端口成员及查询接口，直接使用构造参数。
- [x] 迁移测试并移除查询故障注入及链接参数。
- [x] 验证默认构建、Acceptor 普通与 sanitizer 测试，记录结果。

### Review

构造参数 port 直接传入 CreateServer，移除 _port、GetPort、QueryBoundPort 及专用头文件和查询异常说明。测试使用已有 AddressPort(GetListenFd())，重复使用端口的用例保存为局部变量；继续传入 0 使用随机端口。删除查询失败测试、getsockname 包装及对应链接参数，连接分发逻辑未改动。

make -C Http -j2 all check-acceptor check-acceptor-sanitize 全部通过，ASan/UBSan 无报告。本次修改文件的 git diff --check 通过；全仓检查另有既存 Logger.hpp 末尾空行提示，本次未修改。源码及构建规则已无旧端口接口和故障注入残留。

## Logger 安全日志宏

- [x] 新增 LOG_ERROR_SAFE 并替换 Acceptor 的局部包装。
- [x] 补充参数异常、原宏传播、正常输出、源位置和过滤测试。
- [x] 验证构建、Logger/Acceptor 回归、sanitizer 与临时副本变异。
- [x] 记录验证结果。

### Review

Logger.hpp 新增 LOG_ERROR_SAFE，以 try/catch(...) 包住整个 LOG_ERROR 表达式，不递归记录失败；原日志宏及配置接口保持原行为。Acceptor 删除 ReportFailure，两个处理位置直接调用安全宏，日志文本保持不变。

make -C Http -j2 all check-logger check-acceptor check-acceptor-sanitize 全部通过，ASan/UBSan 无报告，git diff --check 通过。新增测试覆盖三类参数异常隔离、原宏传播、正常输出和源行号、if/else 宏使用、过滤及关闭时不求值。没有单独注入单例初始化或底层输出故障。

临时副本 /tmp/logger-safe.o5ccIK 将安全宏改为直接调用 LOG_ERROR，编译成功，测试因 log argument failure 异常逃逸退出 134，证明新增测试检测到保护缺失；正式源码未参与变异。

## Acceptor 显式清理重构

- [x] 删除 AcceptedFd，提取 DispatchAcceptedFd 并精简注释与日志参数。
- [x] 验证默认构建、相关回归、ASan/UBSan 和泄漏变异。
- [x] 记录验证结果。

### Review

删除 AcceptedFd；HandleRead 只接收并分发，DispatchAcceptedFd 在复制失败和空回调两处显式关闭 fd，调用开始后不再代为关闭。保持异常传播、逐连接替换、线程和端口查询契约。简化固定日志参数及历史注释。

make -C Http -j2 all check-acceptor check-acceptor-sanitize check-socket check-connection check-channel check-epoller 全部通过，ASan/UBSan 无报告。临时副本 /tmp/acceptor-explicit.kF161E 删除复制失败分支 close 后编译成功，被 CopyFailureClosesFd 的 fd 数量断言捕获（退出码 134）；正式源码未参与变异。

## Acceptor 兼容性优化（2026-09-19）

- [x] 完成交接前 fd 守卫、查询报错及线程检查。
- [x] 补充异常、替换、复用、查询失败和线程测试。
- [x] 完成构建、回归、sanitizer、变异验证并记录 Review。

### Review

Acceptor 在 accept 成功后用内部 RAII 守卫管理 fd，复制回调失败和空回调自动关闭，调用开始后交给回调管理。保留逐连接回调副本和阻塞 fd 契约；普通复制异常隔离、bad_alloc 传播。端口查询失败抛带 errno 的 system_error；构造和设置回调检查线程，析构及读事件增加线程断言。未改变注销失败策略，资源耗尽自动退避仍不在本轮范围。

新增 7 个场景调用：普通复制异常、bad_alloc 复制异常、回调内替换、回调内清空、关闭后 fd 复用再抛异常、getsockname 故障回退、错误线程拒绝。getsockname 注入仅通过测试链接器 wrap 实现。

验证：make -C Http -j2 all check-acceptor check-socket check-connection check-channel check-epoller check-acceptor-sanitize 全部通过；ASan/UBSan 无报告。git diff --check 通过。未运行全仓 check。

临时副本 /tmp/acceptor-mutation.gYhTmo 中的两项变异均编译成功并被断言捕获（退出码 134）：省略守卫 close 被 CopyFailureClosesFd 的 fd 数量断言捕获；Release 不清空持有状态被 CallbackExceptionDoesNotCloseReusedFd 捕获。正式源码未用于变异。

## TestBasic 编译错误与增量构建

- [x] 复现当前链接错误。
- [x] 补齐链接源码、解除循环包含并拆分目标文件。
- [x] 验证完整构建、增量构建、独立头文件和 Epoller 回归。
- [x] 记录验证结果与经验。

### TestBasic 修复 Review

实际 make 复现 Logger/Epoller undefined reference。补齐两个实现文件，分离 .o 并使用 -MMD -MP；Epoller.hpp 使用 Channel 前向声明并显式包含 cstddef。同步 TestEpoller 到 Channel(fd, poller) 和 EnableRead 自动注册语义。make -C Http/Test -j4 完整编译链接通过，重复 make 无任务，模拟源码及头文件变更的依赖规则正确；头文件独立严格语法检查、Epoller 严格编译与运行、git diff --check 通过。未运行会监听端口的 TestBasic，未执行顶层全套测试。

## Acceptor EMFILE 恢复用例（2026-09-20）

Acceptor 现有 17 个用例已全部通过（普通与 ASan/UBSan），`tasks/todo.md` 记录过 12 项变异验证。审查后确认剩余一个实质缺口：`HandleRead` 在 `Socket::Accept` 返回 -1 时无条件 `return`，其中 EAGAIN 分支已有 `IdleReadDoesNotInvokeCallback` 钉住，但 **EMFILE（描述符耗尽）分支从未被测过**。该分支是 accept 唯一会「暂时性」失败的场景，且源码注释明确写有「资源耗尽时这里不提供退避」——属于已知取舍，但取舍之后能否自愈没有任何回归保护。

### 目标

钉住三件事：

1. EMFILE 期间回调不被调用，监听端不崩溃、不挂死。
2. 描述符供给恢复后，此前挂起的连接能在下一轮 `Loop()` 被接受（自愈）。
3. 全过程无描述符泄漏。

### 实现要点

- 用真实 `setrlimit(RLIMIT_NOFILE)` 把上限压到当前用量，再用 `open("/dev/null")` 填满，触发**真实** EMFILE，而非 wrap 伪造返回值。理由：`Socket::Accept` 在 EMFILE 下会走 `LOG_ERROR` 并保留 errno，真实耗尽才能验证这条路径；且「释放描述符后恢复」本身就需要物理语义。
- 已用探针 `/tmp/probe_emfile.cc` 在普通构建与 `-fsanitize=address,undefined` 构建下各跑一次确认：`setrlimit` 到「当前 fd 数 + 2」可稳定触发 `accept` 返回 `-1/EMFILE`，恢复上限后 `accept` 立即成功且返回值为 5，退出时 fd 数无增长。两种构建行为一致。
- 已排除两个干扰项：Logger 默认 sink 是 `stdout_color_sink_mt`（写 fd 2，不申请新描述符）；`EventLoop::QueueInLoop` 只写已存在的 eventfd，不申请新描述符。两者在耗尽状态下均安全。
- 顺序约束：`EventLoop`、`Acceptor` 构造与客户端 `connect` 都需要描述符，必须全部在降低上限**之前**完成；`CountOpenFds()` 用 `opendir`，耗尽状态下会失败，故所有 fd 计数断言只能放在恢复之后。

### 步骤

- [x] 在 `Test/TestAcceptor.cc` 新增 `FdLimitGuard`：构造时 `getrlimit` 存档并 `setrlimit` 压低，析构时恢复；恢复失败不得吞掉（低上限会静默污染后续用例），走 `write` + `_exit`，与看门狗同风格。
- [x] 新增 `FillFdsUntilEmfile`：循环 `open("/dev/null", O_RDONLY)` 直到失败，断言 `errno == EMFILE`，返回填充数供调用方释放。
- [x] 新增用例 `AcceptEmfileThenRecovers`：建连 → 记录基线 → 压低上限并填满 → `Step(loop)` 断言回调计数为 0 → 恢复上限并释放填充描述符 → `Step(loop)` 断言回调计数为 1、拿到的 fd 可收发 → 断言 fd 数回到基线。
- [x] 更新 `OnAlarm` 的超时消息。当前文本写死「疑似 HandleRead 未在 EAGAIN 处停止」，EMFILE 用例若因重试挂死会得到误导性提示，改为同时覆盖两种情形。
- [x] 运行 `make check-acceptor` 与 `make check-acceptor-sanitize`。
- [x] 变异验证（源码副本放 `/tmp`，正式源码不参与）。

### Review（2026-09-20）

`Test/TestAcceptor.cc` 新增 1 个用例与 2 个辅助设施（`FdLimitGuard`、`FillFdsUntilEmfile`），顶层用例由 17 增至 18。`OnAlarm` 消息改为「疑似 HandleRead 未在 EAGAIN/EMFILE 处停止」。`Makefile`、`Acceptor.cc`、`Acceptor.hpp` 均未改动。`src/Acceptor.cc` 与 `include/Acceptor.hpp` 的 SHA-256 在变异前后逐字节一致。

**用例形态**：构造 `EventLoop` 与 `Acceptor`、客户端 `connect` 全部在压低上限之前完成（三者都要占用描述符）；随后 `CountOpenFds` 取基线（`opendir` 自身需要描述符，耗尽后无法再取），再用 `FdLimitGuard` 把 `RLIMIT_NOFILE` 压到基线 + 2 并填满。此时 `Step(loop)` 断言回调计数为 0；离开守卫作用域恢复上限、释放填充描述符后，第二次 `Step(loop)` 断言回调计数为 1、拿到的描述符能 `send` 成功，最后断言 `CountOpenFds()` 回到基线。`assert(filled > 0 && filled < 64)` 用于区分「真的被 EMFILE 挡下」与「数组先满、用例前提根本不成立」。

**运行结果**：`make check-acceptor` 与 `make check-acceptor-sanitize` 均通过，ASan/UBSan 无报告。运行日志新增一行 `[Socket.cc:159] Failed to accept connection: errno=24`，即 EMFILE，确认真实走到了该路径而非被其他分支拦下。

**变异验证 3 项**（副本 `/tmp/acceptor-emfile-mut`，用完删除）：M1 在 `HandleRead` 中对 `errno == EMFILE` 走 `continue` 重试而非退避，被看门狗以 30 秒超时捕获（退出码 2，输出新的超时消息）；M2 在 EMFILE 分支把失败码 `-1` 误当有效描述符分发一次，被 `TestAcceptor.cc:506` 的 `assert(count == 0)` 捕获（SIGABRT）；M3 用文件作用域的 `g_dead` 让 EMFILE 之后永久失效、不再自愈，被 `TestAcceptor.cc:514` 的 `assert(count == 1)` 捕获（SIGABRT）。三项均直指新用例，且分别对应「不挂死」「不误触发回调」「能自愈」三个目标。

**计划偏差**：原计划 M2 设计为「EMFILE 分支误 `close` 一个描述符」，实际不成立——EMFILE 时根本没有新描述符被创建，误 close 只能去关无关的 fd，语义牵强；改为误分发负值，直接验证回调不该被调用的断言。原计划 M3 设计为「删掉 `FdLimitGuard` 的恢复动作」，实测也不成立——该用例本就是最后一个，且填充描述符已释放，恢复后的计数断言在低上限下照样成立，删除恢复动作不会有任何用例失败。这恰好说明 `FdLimitGuard` 的恢复动作在当前用例顺序下**没有测试覆盖**，它的价值是防御性的（用例被重排或后续追加时会暴露），已如实保留在「风险」一节而没有假装它被验证过。M3 遂改为针对自愈断言，证明力更强。

**回归**：`make -j2 all check-acceptor check-acceptor-sanitize check-connection check-channel check-epoller check-eventloop` 全部通过（Acceptor 普通与 sanitizer 各输出一次通过信息，Connection/Channel/Epoller/EventLoop 各一次）。

**未通过项**：`git diff --check` 报 `Http/include/Logger.hpp:126: new blank line at EOF.`。该文件在本次会话开始前已是修改状态（`M Http/include/Logger.hpp`），本次改动只涉及 `Test/TestAcceptor.cc`，未触碰 Logger 任何一行，属既有遗留问题，未擅自修改。

### 风险

- 该用例会临时压低**整个进程**的描述符上限。必须由 RAII 保证任何退出路径都恢复；若 `assert` 在耗尽状态下失败，错误信息可能无法完整打印，需接受。
- 测试期间 `Socket::Accept` 会输出一行 `Failed to accept connection: errno=24` 的 ERROR 日志，属预期输出，不是失败。
- `Makefile` 无需改动，不引入 `--wrap=accept`。忙循环的死循环形态已由既有看门狗兜住，加 wrap 只为精确计数，收益不抵复杂度。

## Connection 日志去掉 ReportFailure 中间层（2026-09-20）

- [x] 删除匿名命名空间的 `ReportFailure`，7 处调用点直接用 `LOG_ERROR`。
- [x] 用改动前后的对照二进制确认日志文本逐字不变。
- [x] 跑通普通与 ASan/UBSan 的 Connection 测试。
- [x] 记录既有阻塞问题与本次未覆盖范围。

### Review

用户指出 `Connection.cc` 里为转发日志自建的 `ReportFailure(const char *, const char *) noexcept` 不必要，应统一到 `LOG_*`。改动只涉及 `Http/src/Connection.cc`，`+7/-16`：删除匿名命名空间的 8 行函数体及其注释，7 处调用点改写为 `LOG_ERROR("Connection {}: {}", ...)`。格式串与参数顺序原样保留，因此输出文本与改动前逐字相同；`Logger.hpp` 仍在包含列表中（`LOG_ERROR` 的来源），`Connection.hpp`、`Makefile`、测试文件均未改动。

**与 2026-09-19「日志异常兜底统一到宏入口」的关系**：上一轮把兜底并入 `HTTP_LOG_AT` 后，`ReportFailure` 的 `noexcept` 与其中的 try/catch 已成历史残留——不抛契约由宏本身兑现，与是否有这层转发函数无关。删除它不改变异常语义，反而让「全项目只有 `LOG_*` 一个日志出口」这句话在代码里真正成立。`Logger::Init` 的配置（等级、格式、文件 sink）照旧生效，这一点是选择保留 `LOG_*` 而不是直连 `spdlog::error` 的原因：后者会落到 registry 构造函数建的默认日志器（`registry-inl.h:46`），绕过本模块配置。

**验证**：`make -C Http -j2 all` 编译通过（`-Werror` 无告警）。改动前后各建一个二进制对照运行，注入失败路径打出的日志文本逐字一致，唯一差别是 `file:line` 由 `Connection.cc:19` 变为各调用点行号——这正是去掉中间层的目的，不是回归。`TestConnection` 24 条用例在普通构建与 ASan/UBSan 构建下均通过（约 2.4 秒，无 sanitizer 报告）。`git diff --check` 对本次文件无告警。

**未通过项（既有问题，不在本轮范围）**：`make -C Http check-connection` 在本轮**无法观测到通过**，30 秒看门狗必然超时。已用改动前的 `Connection.cc` 重建对照二进制复现，同样是 30 秒超时且日志输出逐字相同，故与本次改动无关。`gdb` 抓到的栈是 `MessageFailureIsIsolated(false)` → `Step(loop)` → `EventLoop::Loop()` 阻塞在 `epoll_wait(-1)`；根因是工作区中**未提交**的 `src/EventLoop.cc` 把单轮循环体包成了 `while (true)`（见 `git diff src/EventLoop.cc`），而测试的 `Step()` 依赖 `Loop()` 跑完一轮返回（`Test/TestConnection.cc:94-99`）。该 `while (true)` 属于 `LoopThread`/`EventLoop` 正在进行的工作，修法与取舍需单独定，本轮未触碰。

**为取得可用回归结论所采用的临时手段**：把 `Loop()` 还原成单轮版本放进 `/tmp/one/EventLoop.cc`（未写入仓库），与改动后的 `Connection.cc` 一起编译，普通与 sanitizer 两个版本均 `Connection tests passed`。也就是说，上述「24 条用例通过」是在这个单轮副本下取得的，仓库当前状态的 `check-connection` 仍然是红的。正式源码未参与任何变异，临时副本仅在 `/tmp`。

**未覆盖路径（如实记录）**：本次为纯日志调用点改写，没有新增断言，也没有做变异验证——没有可观测的行为差异可供变异，硬编一个变异只能证明「删掉日志不影响断言」，价值不足。真正的等价性证据是上面那份前后对照二进制的逐字日志比对。全仓 `make check` 未运行：`check-connection` 已知超时，跑全仓只会得到相同的既有红项。

## EventLoop 协作式停止（2026-09-20）

用户已确认原子标志、eventfd 唤醒、完成当前轮后退出的设计。

- [x] 添加 Quit，明确生命周期、异常、不保证排空后续任务的契约。
- [x] 独立测试提前退出、重复退出、跨线程唤醒、当前批次完成。
- [x] 实际 Makefile 构建、普通及 sanitizer 验证，记录结果。

范围：EventLoop 停止能力；LoopThread 析构和上层连接关闭尚未接入。既有测试依赖单轮 Loop 的兼容问题不计作本轮通过项。

### Review

EventLoop 新增 atomic<bool> 停止标志和 Quit()：先设置标志，再通过现有 eventfd 唤醒；Loop 每轮入口检查，不重置标志。当前轮事件和已取出的任务批次继续完成，后来入队的任务不承诺执行。Quit 保持现有唤醒函数的异常传播语义，调用者必须保证对象存活。

新增 TestEventLoopQuit.cc 及普通/sanitizer Makefile 目标，并接入 check/check-sanitize/clean-tests。测试验证提前退出、重复退出、不重新启动、当前批次完成、后续批次不执行、跨线程唤醒。

验证：make -C Http -j2 all check-eventloop-quit check-eventloop-quit-sanitize 全部通过，ASan/UBSan 无报告。本次跟踪文件 git diff --check 通过。临时副本 /tmp/eventloop-quit-y7ugiwzg 中，两项变异均编译成功后被捕获：去掉唤醒报 Quit did not wake epoll_wait；重置停止标志触发十秒看门狗。正式源码未参与变异。

限制：未运行全套旧测试；此前记录的单轮 Loop 测试与持续循环不兼容仍存在。LoopThread 析构目前仍为空，本轮不代表线程池已具备完整的安全关闭能力。Quit 不等待线程、不关闭业务连接，LoopThread 的 join 和上层停收/清理需要后续接入。


## 普通定时任务 ID 与取消（2026-09-21）

用户已批准按讨论方案执行。

- [x] 原子分配统一 ID，RunAfter 返回 ID，添加与取消按队列顺序执行。
- [x] 补充接口契约，修正模块接入构建所需的声明问题。
- [x] 验证执行、取消顺序、并发唯一性、参数和 ID 耗尽。
- [x] 记录验证结果与现有服务器未完成部分。

### Review

RunAfter 返回统一原子分配的 ID；CancelTask 使用同一主循环队列，避免主线程取消越过工作线程尚未登记的任务。ID 耗尽抛异常且不回绕，非法延迟和空回调在提交前拒绝。连接创建仍为空，已在接口注明后续必须用 AllocateId；测试通过模拟连接取号和连接超时登记验证不冲突。

验证：make -C Http -j2 check-tcpserver-timers check-tcpserver-timers-sanitize 通过。覆盖普通任务恰好执行一次、跨线程添加后主线程立即取消、重复/不存在/已执行任务取消、4 线程 400 个任务 ID 唯一与取消、参数拒绝、ID 耗尽不回绕；ASan/UBSan 无报告。临时副本 /tmp/http-timer-order-check 把取消从 QueueInLoop 改回 RunInLoop，编译成功后 canceled_calls == 0 断言失败，证实顺序回归可被捕获。

编译接入所需修正：移除 include 尾部多余分号，补 EventLoop 的 thread 依赖；LoopThreadPool 构造定义移除非法 explicit 并修正初始化顺序，析构下沉以支持前置声明；补 TcpServer 默认析构。测试加入 Makefile 独立目标及 check/check-sanitize/clean-tests。

限制：TcpServer::Start、NewConnection 和连接移除仍为空；LoopThread 尚无完整退出/join 流程。测试直接驱动主循环，不启动工作线程池，不代表完整服务器生命周期已实现。停止/析构不得与提交任务并发。未运行有已知兼容问题的全套旧测试。本次涉及文件格式检查通过；全局 git diff --check 仍报告用户已有 TimerQueue.cc 注释尾随空格，未修改该文件。

## TcpServer 端到端测试（2026-09-21）

用户已确认本轮范围为「只加测试」，生产代码不改。

- [x] 编写 `Http/Test/TestTcpServer.cc`：看门狗、客户端工具、主循环同步读取辅助、关停辅助。
- [x] 用例：单线程端到端回显与回调线程归属。
- [x] 用例：`SetThreadCount(3)` + 6 连接轮转分配。
- [x] 用例：空闲超时关闭连接。
- [x] 用例：有数据往来时刷新空闲计时、不被关闭。
- [x] 用例：未设置任何回调时的连接建立与回收。
- [x] 用例：256 KiB 负载回显完整性。
- [x] 用例：`SetThreadCount(-1)` 参数校验。
- [x] 用例：全部场景结束后 fd 数回到基准。
- [x] Makefile 接入 `check-tcpserver` / `check-tcpserver-sanitize`，并入 check/check-sanitize/clean-tests。
- [x] 运行普通与 ASan/UBSan 两套测试并记录结果。
- [x] 变异验证 4 项（删除 `_conns.erase`、`NextLoop` 不轮转、不启用空闲销毁、不调用 `Established`）逐项确认被捕获。
- [x] 写入 Review，含未覆盖范围与已发现缺口。

### Review（2026-09-21）

新增 `Http/Test/TestTcpServer.cc`（7 个场景）与 Makefile 的 `check-tcpserver` / `check-tcpserver-sanitize` 目标，并行并入 `check` / `check-sanitize` / `clean-tests`。生产源码一行未改。

**结构约束（决定了测试的形状）**：`TcpServer` 必须在构造它的线程上 `Start()`——`_baseloop` 在构造函数里记录线程 id，`Acceptor::SetAcceptCallback` 会校验线程，而 `Start()` 又阻塞在主循环上。因此主线程负责「构造 + `Start()`」，客户端驱动逻辑跑在另一个线程里。读取 `_conns` 这类主循环独占状态时，一律用 `QueueInLoop` 回到主循环线程读取（`RunInLoopSync`），而不是从驱动线程直接读容器，避免与事件循环并发访问同一 `unordered_map`。

**关停方式**：`TcpServer` 没有停止接口，`LoopThread::~LoopThread` 也不 join，所以测试用 `#define private public`（沿用 `TestTcpServerTimers.cc` 的做法）先等连接全部回收，再逐个 `Quit()` 工作线程并 join，最后 `Quit()` 主循环让 `Start()` 返回。这只是测试的关停手段，不代表生产代码具备该能力。

**用例**：单线程回显与回调线程归属（含 any-event 回调）；`SetThreadCount(3)` + 6 连接轮转分配；空闲超时关闭；活动刷新空闲计时；无任何业务回调时的建立与回收；256 KiB 负载回显；`SetThreadCount(-1)` 参数校验且被拒后仍能正常启动。

**验证**：`make -C Http -j2 all check-tcpserver check-tcpserver-sanitize` 全部通过，普通与 ASan/UBSan 各约 5 秒，无 sanitizer 报告。全部场景结束后 fd 数回到基准。日志中一行 `线程数量不能为负数` 的 error 属预期输出。代码格式检查（`git diff --check`）对新增文件无告警。

**变异验证 4 项**（副本在 `/tmp`，正式源码未参与，基线 exit=0）：M1 删除 `_conns.erase` → `Assertion 'WaitFor(... ConnCount(server) == 0)' failed`；M2 `NextLoop` 固定返回首个线程 → `Assertion 'handler_threads.size() == kWorkers' failed`；M3 `EnableInactiveRelease` 不置启用标志 → `Assertion 'RecvEof(client)' failed`；M4 `NewConnection` 不调用 `Established()` → `Assertion 'received > 0' failed`。四项均死在各自针对的断言上。

**发现的问题（均未修，生产代码改动不在本轮范围）**：

1. **既有回归：构造过但从未 `Start()` 的 `TcpServer` 无法析构**。`Acceptor` 构造函数只建了监听 Channel 并不登记，登记发生在 `StartAccepting()`；而 `~Acceptor` 无条件调用 `_channel->Remove()`，`Epoller::RequireRegistered` 对未登记对象抛 `std::logic_error`，异常逃出析构函数即 `std::terminate`。已用 gdb 确认栈为 `~Acceptor → Channel::Remove → EventLoop::RemoveEvent → Epoller::RemoveEvent`。**现有的 `check-tcpserver-timers` 现在也因此崩溃**（该测试构造了 `TcpServer(0)` 但从不 `Start()`）。git 时间线显示 `TestTcpServerTimers.cc` 写于 14:25、当时通过，而 `src/Epoller.cc`/`src/Channel.cc` 在 16:29 才改成「未登记即抛」的语义，即回归来自那次改动。我的参数校验用例为此改为「先 `SetThreadCount(-1)` 被拒、再 `SetThreadCount(0)` 并真正启动」，既保留了校验点又避开了这条已存在的崩溃路径。
2. **工作线程存在时析构 `TcpServer` 直接 `std::terminate`**。`LoopThread::~LoopThread` 为空，`_thread` 始终可 join，`std::thread` 析构即终止进程；`TcpServer` 也没有「先停止线程池再析构」的接口。探针实测：启动 1 个工作线程、停掉主循环后让 `server` 出作用域，得到 `terminate called without an active exception`。这是测试必须自行关停的根本原因，属于 `LoopThread`/`TcpServer` 尚未收尾的部分。
3. **`TcpServer::EnableInactiveRelease` 没有参数校验，且校验点来得太晚**。服务器侧直接存下 `timeout` 并置启用标志，真正的 `timeout <= 0` 校验在 `Connection::EnableInactiveRelease` 里，而调用发生在 `_conns.emplace` 之后、`Established()` 之前；异常被 `Acceptor::DispatchAcceptedFd` 的 `catch (const std::exception &)` 吞掉，`handed_off` 已为 true 于是既不关 fd 也不回滚连接表。探针实测 `EnableInactiveRelease(0)`：客户端连接后 `_conns` 恒为 1，客户端关闭 800 毫秒后仍是 1（僵尸项），描述符比基线多出恰好 1 个被接受的 fd，日志只有一行 `Acceptor accept callback: 空闲超时时间必须大于 0`。

**全量回归现状（与本轮改动无关）**：`make -k check` 除 logger / buffer / socket / epoller / eventloop-quit / 新增的 tcpserver 外均为红：`check-channel`（看门狗超时）、`check-eventloop`（`TIMEOUT: 5 秒内未完成，疑似阻塞在 Loop() 的 epoll_wait`）、`check-timerqueue`、`check-connection`（同样卡在 `epoll_wait`）、`check-acceptor`、`check-integration`、`check-tcpserver-timers`。这些测试仍按「单轮 `Loop()`」的语义用 `Step(loop)` 驱动，而 `Loop()` 现在是 `while (!_quit)` 的持续循环，两者不兼容；本轮只新增了 `Test/TestTcpServer.cc` 与 Makefile 目标，未触碰这些测试或其构建规则，失败原因与既有记录一致。

**未覆盖范围（如实记录）**：连接表与工作线程分配的断言只在 0 与 3 个线程下取样；未覆盖 `Start()` 期间工作线程创建失败、`Acceptor` 的 EMFILE 分支（另有专用用例）、`RunAfter` 与连接超时共享 ID 空间（由 `TestTcpServerTimers` 覆盖）；空闲超时用例依赖时间轮的 1 秒真实节奏，未做边界抖动统计；未运行 TSan，「主循环与工作线程共享状态」的并发正确性靠结构约束（只在主循环线程读写 `_conns`）而非工具保证。

## LoopOnce 单轮驱动改造（2026-09-21）

用户批准：把循环体抽成不循环的 `LoopOnce()`，`Loop()` 退化为 `while (!_quit) LoopOnce();`，测试改回单轮驱动；并一并修掉 Acceptor 析构缺陷与失效的 `make server` 示例目标。

- [x] `include/EventLoop.hpp` + `src/EventLoop.cc`：抽出 `LoopOnce()`，`Loop()` 改为前置判断的 `while (!_quit.load()) LoopOnce();`，补契约注释。
- [x] `Test/TestAcceptor.cc`、`Test/TestConnection.cc` 的 `Step()` 改调 `LoopOnce()`。
- [x] `Test/TestChannel.cc`、`Test/TestEventLoop.cc`、`Test/TestTimerQueue.cc`、`Test/TestBasic.cc` 的直接调用改调 `LoopOnce()`。
- [x] 同步各测试文件里「单轮 Loop()」的注释措辞。
- [x] `Acceptor` 记录监听是否已启动，析构仅在已登记时注销（修 `check-tcpserver-timers` 崩溃）。
- [x] `Makefile` 的 `server` 前置改为 `Test/TestServer.cc`，示例改单次 `Loop()` 并编译验证。
- [x] 分目标验证 6 个原红目标 + 3 个回归保护目标。
- [x] `make -k check` 与 `make -k check-sanitize` 全量。
- [x] 变异验证 5 项（删 ExecuteTasks / 删分发 / Loop 不判停止 / Loop 重置停止标志 / Acceptor 恢复无条件注销）。
- [x] 写入 Review 与仍未修的缺口。

### Review（2026-09-21）

用户批准方案后执行。除计划内的三项（LoopOnce 抽取、Acceptor 析构、server 示例目标）外，落地过程中在切换后首次真正执行到的断言处又暴露两处**既有**问题，均已在本次一并处理，理由见下。

**改动清单**：

- `include/EventLoop.hpp` / `src/EventLoop.cc`：循环体抽成 `LoopOnce()`（等待一次就绪事件 → 分发 → 执行一批任务），`Loop()` 退化为前置判断的 `while (!_quit.load()) LoopOnce();`。`LoopOnce()` 不读也不重置 `_quit`，停止判定只由 `Loop()` 的循环条件负责；不加 `IsInLoopThread()` 断言，保持纯抽取。头文件补了两处契约注释与 `@file` 里「这三步构成一轮」的说明。
- 测试改回单轮驱动：`TestAcceptor.cc` 与 `TestConnection.cc` 的 `Step()` 内改调 `LoopOnce()`（各一处，全部调用点随之生效），`TestChannel.cc`（3 处）、`TestEventLoop.cc`（8 处）、`TestTimerQueue.cc`（8 处）、`TestBasic.cc`（1 处）改直接调用，并同步了各文件里「单轮 Loop()」的注释与看门狗超时文案。`TestEventLoopQuit.cc`、`TestTcpServerTimers.cc`、`TestTcpServer.cc` 保持使用 `Loop()`。
- `include/Acceptor.hpp` / `src/Acceptor.cc`：新增 `_accepting`，`StartAccepting()` 置位，`~Acceptor()` 仅在已启动监听时注销。
- `Makefile` + `Test/TestServer.cc`：`server` 目标前置由已删除的 `src/TestServer.cc` 改为 `Test/TestServer.cc`；示例里的 `while (true) loop.Loop();` 改为单次 `loop.Loop();`（持续循环下外层 while 永不第二次进入），注释同步。

**切换后暴露的两处既有问题（计划外，已处理）**：

1. **`TestAcceptor.cc` 的 18 个用例从未调用 `StartAccepting()`**。它们全部按「构造即登记读事件」编写，而实现把登记放在 `StartAccepting()`（`TcpServer::Start()` 正是「先设 accept 回调、再 `StartAccepting()`」），`include/Acceptor.hpp` 的注释也还写着「构造成功即意味着已完成绑定、监听与读事件登记」——三者互不一致。改 Loop 之前所有用例都卡在第一个 `Step()`，这个矛盾从未被执行到。已与用户确认**以现有实现为准**：13 个用例补 `acceptor.StartAccepting();`，头文件注释改为「构造只完成绑定与监听，登记发生在 `StartAccepting()`」。其中 `DestructorUnregistersChannel` 的补充尤其必要——没有它，「析构是否注销」这条断言会因为「从未登记」而恒真，退化成空断言。
2. **`TestBasic.cc:226` 残留着上一轮变异验证未还原的代码**（`// MUTATION 4：对端关闭不回收` 后直接 `return`），导致对端关闭的连接永不回收，`RunIntegration:487` 的 `ConnCount() == 0` 必然失败。这正是 `tasks/todo.md` 从 2026-09-17 起多次记录的「既有红项」。已还原为 `RequestClose(fd); // 对端关闭：回收本侧连接`，与同文件错误分支的写法一致。

**验证**：

- 分目标：`check-channel`、`check-eventloop`、`check-acceptor`、`check-timerqueue`、`check-connection`、`check-integration` 六个原红目标全部通过；回归保护目标 `check-eventloop-quit`、`check-tcpserver-timers`、`check-tcpserver` 保持通过（其中 `check-tcpserver-timers` 正是靠本轮 Acceptor 修复才从崩溃转绿）。
- 全量：`make -j2 -k check` 退出码 0；`make -j2 -k check-sanitize` 退出码 0，13 项测试全绿，日志中 `AddressSanitizer`/`runtime error`/`LeakSanitizer` 出现次数为 0。这是本仓近期第一次全量双绿。
- 示例：`make server` 编译通过，并用真实回环连接验证回显（`hello-demo` 原样返回）后终止进程。
- `git diff --check` 通过。

**变异验证 5 项**（副本只在 `/tmp`，正式源码未参与；编译带 `-UNDEBUG` 保证 assert 生效）：M1 `LoopOnce()` 删掉 `ExecuteTasks()` → `QueueInLoopDefersUntilLoop:150 ran` 失败；M2 `LoopOnce()` 删掉 `Channel::Handle()` 分发 → `TestChannel.cc:233 Assertion 'readable' failed`；M3 `Loop()` 改成 `while (true)` → 看门狗超时（exit 2）；M4 `Loop()` 开头加 `_quit.store(false)` → 看门狗超时（exit 2）；M5 `~Acceptor` 恢复无条件注销 → `terminate called after throwing an instance of 'std::logic_error'`。

M4 的捕获方式与预期不同，如实记录：原以为会死在 `QuitBeforeLoop` 的 `assert(!called)`，实际是「标志被重置 → 第一轮跑完后 `while` 条件仍为假 → 第二轮 `epoll_wait` 永久阻塞 → 看门狗」，即**死锁也是一种被捕获**，但捕获点是看门狗而非该断言。M3 同理。这两项说明 `Quit` 语义目前只有「看门狗能发现」这个量级的保护，没有精确到断言的钉点。

**仍未修的缺口（与上一节记录一致，本轮未动）**：

1. `LoopThread::~LoopThread` 为空、不 join 工作线程，`TcpServer` 也没有停止接口——工作线程存在时析构 `TcpServer` 仍会 `std::terminate`；测试 `TestTcpServer.cc` 只能靠私有访问自行关停。
2. `TcpServer::EnableInactiveRelease` 不做参数校验，且校验点在 `_conns.emplace` 之后，`timeout <= 0` 时每个新连接都会失败并留下僵尸项与 fd 泄漏。
3. `Epoller::Detach` 有声明无定义；`Channel::~Channel` 不注销登记（`TestChannel` 的 `DestructorDoesNotDetach` 正是钉住这一点）。

**范围外但已核实的事实**：`Test/Server.cc` 仍是一个只构造 `TcpServer` 就返回的空壳，不在任何构建目标里；`Test/TestServer.cc` 是 9 月 17 日的旧示例（自带一套简易 Connection/Channel 用法），本轮只做了 `Loop()` 调用方式的同步。

## EchoServer 编译目标（2026-09-21）

用户确认添加 `echoserver`。

- [x] 添加独立编译目标，复用 TcpServer 依赖，并加入 clean。
- [x] 补充现有入口的 Start 调用。
- [x] 编译并验证回显，检查增量构建与清理规则。

### Review

编译通过；本机 8080 端口二进制回显通过（含空字节）；make -q 确认无需重复构建；make -n clean 确认清理产物；git diff --check 通过。测试进程已结束。

## include 目录分类方案（2026-09-22）

用户提出对 `Http/include` 做分类，并指明分类维度是「功能 + 协议两个层次」。本轮**只列方案，不动代码**，供审阅后再决定。

### 现状事实（已核实）

- `Http/include/` 下 12 个头文件，全部平铺，无子目录。
- 全仓 93 处本地头文件引用，分布在 30 个文件，写法均为 `#include "EventLoop.hpp"` 这类平铺路径；唯一例外是 `Test/TestSocket.cc:1` 的 `#include "../include/Socket.hpp"`。
- 引用次数排序：`EventLoop.hpp` 19、`Channel.hpp` 14、`Logger.hpp` 12、`TimerQueue.hpp` 8、`Socket.hpp` 8、`Buffer.hpp` 8、`Connection.hpp` 6、`TcpServer.hpp` 4、`Acceptor.hpp` 4、`LoopThreadPool.hpp` 3、`LoopThread.hpp` 3、`Epoller.hpp` 3。
- Makefile 中与头文件路径绑定的位置共 10 处：7 处显式依赖（第 19、22、28、53、80、154、172 行），3 处 `$(wildcard include/*.hpp)`（第 209、229、251 行）。
- `spdlog.mk`、`.gitignore` 均不涉及 `include/` 路径，无需改动。

### 候选目录树

**候选 A：功能层 + 协议层并列（我倾向此方案）**

按「reactor 核心是否知道 TCP 存在」切分：`base`/`reactor`/`thread` 协议无关，`tcp` 单向依赖前三者。

```
Http/include/
├── base/     Logger.hpp  Buffer.hpp
├── reactor/  Epoller.hpp  Channel.hpp  EventLoop.hpp  TimerQueue.hpp
├── thread/   LoopThread.hpp  LoopThreadPool.hpp
└── tcp/      Socket.hpp  Acceptor.hpp  Connection.hpp  TcpServer.hpp
```

**候选 B：第一层协议、第二层功能**

```
Http/include/
├── common/
│   ├── base/    Logger.hpp  Buffer.hpp
│   ├── reactor/ Epoller.hpp  Channel.hpp  EventLoop.hpp  TimerQueue.hpp
│   └── thread/  LoopThread.hpp  LoopThreadPool.hpp
└── tcp/     Socket.hpp  Acceptor.hpp  Connection.hpp  TcpServer.hpp
```

好处是新增 HTTP 层时顶层会自然出现 `http/`；代价是多一层目录，当前 `common/` 下只有一份实现，暂时看不出收益。

**候选 C：候选 A 再套项目顶层目录**

```
Http/include/http/
├── base/  reactor/  thread/  tcp/    （同候选 A）
```

为后续扩展预留，但会让引用变成 `#include "http/reactor/EventLoop.hpp"`，冗余感较强。

### 引用路径的两种改法及各自效果

| 维度 | 全量改写路径（改写 93 处） | Makefile 多加 `-I`（源码零改动） |
| --- | --- | --- |
| 引用写法 | `#include "reactor/EventLoop.hpp"` | `#include "EventLoop.hpp"` 不变 |
| 编译参数 | 保持唯一 `-Iinclude` | 变为 `-Iinclude/base -Iinclude/reactor ...` |
| 漏改后果 | Makefile 显式依赖不匹配 → 立即 `No rule to make target` 报错 | 不适用 |
| 同名头文件 | 路径唯一，不存在歧义 | 按 `-I` 顺序取首个匹配，静默选错、不报错 |
| 可读性 | 路径即层级，IDE 跳转可靠 | 需靠索引推断归属 |

倾向全量改写：93 处是机械替换，且失败方式是「大声报错」而非静默错误。若想降低单次风险，可先做候选 B 的建目录 + 加 `-I` 过渡，编译通过后再逐个改写引用。

### 若选候选 A + 全量改写，实施清单

- [x] 新建 `include/base`、`include/reactor`、`include/thread`、`include/tcp`，`git mv` 12 个头文件（保留历史）。
- [x] 改写 30 个文件的 93 处引用为子目录路径。
- [x] 统一 `Test/TestSocket.cc:1` 的 `"../include/Socket.hpp"` 为 `"tcp/Socket.hpp"`。
- [x] Makefile 第 19、22、28、53、80、154、172 行改为子目录路径。
- [x] Makefile 第 209、229、251 行的 `$(wildcard include/*.hpp)` 改为 `$(wildcard include/*/*.hpp)`（GNU make 的 `wildcard` 不递归，需显式写两层）。
- [x] 同步 `Http/Makefile` 中的依赖变量，确保 `-Iinclude` 唯一且不再出现旧路径。
- [x] 全量验证：`make -j2 -k check`、`make -j2 -k check-sanitize` 双绿，`make echoserver` 编译通过。
- [x] 确认 `git diff --check` 通过，且无遗漏的旧路径残留。

### 用户裁决

用户选择候选 A + 全量改写路径，第 3 项（`src/` 是否对齐）本轮未做，`src/` 维持平铺。

### Review

**实际改动量**：`git mv` 11 个已跟踪头文件（`TcpServer.hpp` 本就未纳入版本控制，用普通 `mv`），`git status` 全部识别为 `R` 重命名，历史保留。改写 92 处引用（第 93 处是 `TestSocket.cc` 的相对路径，单独手工改为 `tcp/Socket.hpp`），Makefile 10 处（7 处显式依赖 + 3 处通配符）。`src/`、`Makefile` 之外无任何文件引用这些路径，`spdlog.mk` 与 `.gitignore` 不受影响。

**最终目录结构**：

```
Http/include/
├── base/     Buffer.hpp  Logger.hpp
├── reactor/  Channel.hpp  Epoller.hpp  EventLoop.hpp  TimerQueue.hpp
├── thread/   LoopThread.hpp  LoopThreadPool.hpp
└── tcp/      Acceptor.hpp  Connection.hpp  Socket.hpp  TcpServer.hpp
```

**验证**：

- 通配符展开：独立 makefile 探针确认 `$(wildcard include/*/*.hpp)` 展开到全部 12 个头文件，未因不递归而漏配依赖。
- 功能测试：`make -j4 -k check` 全部通过，包括 Socket、Epoller、Channel、EventLoop、TimerQueue、Connection、Acceptor、TcpServer 端到端与计时器用例。
- Sanitizer：`make -j4 -k check-sanitize` 退出码 0，日志中 `AddressSanitizer`/`runtime error`/`LeakSanitizer`/`UndefinedBehaviorSanitizer` 计数为 0。
- 冷构建：`make clean && make -j4 echoserver` 从零编译成功，产物 `.build/EchoServer` 1458752 字节，确认无增量构建掩盖的依赖路径错误。
- `git diff --check` 通过。

**残留检查**：`grep` 确认 `src`、`include`、`Test` 下已无 `#include "<头文件名>.hpp"` 形式的平铺路径，Makefile 中已无 `include/<头文件名>.hpp` 形式的旧路径。

**本轮未做、与上一节记录一致的既有缺口**（与目录分类无关，未触碰）：`LoopThread` 析构不 join、`TcpServer::EnableInactiveRelease` 缺少参数校验与校验点位置、`Epoller::Detach` 有声明无定义、`Channel` 析构不注销登记。

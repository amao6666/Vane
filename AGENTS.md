# Vane 项目开发约束

## 项目概述
Vane 是一个跨平台视频编码抽象库，为 Windows (Media Foundation)、macOS (VideoToolbox)、Linux (VA-API) 提供统一的 C++ 编码接口。项目采用 MIT 许可证。

## 思考与语言规范
- 所有思考过程使用中文
- 代码注释使用中文
- 对外 API 文档使用中文
- 变量名、函数名、类名使用英文，遵循 C++ 命名规范

## 核心行为规则

### 1. 先想后问，不要默默假设
- 遇到不确定的技术细节（API 参数、平台差异、编码器配置），必须先明确提出疑问，不要凭猜测写代码
- 对 VideoToolbox、Media Foundation、VA-API 的具体 API 调用，如果不确定参数含义，先查阅官方文档或明确提出
- 不确定时主动打断并提问，宁可多确认，不要写猜的代码

### 2. 极简优先，不做多余设计
- 第一版只实现核心接口：Initialize / StartRecording / EncodeFrame / StopRecording / IsRecording
- 不添加任何非必需的功能（如自定义编码器参数、多路复用、流媒体推送等）
- 每个平台的实现类，只写最少量的代码让测试用例跑通
- 不要过度抽象：接口够用就行，不做"将来可能需要"的扩展
- 用最少代码解决问题，不写没人要求的功能

### 3. 精准修改，不碰无关代码
- 修改某个平台的实现时，不要顺手"优化"其他平台的代码
- 修改接口定义时，必须同步更新所有平台的实现，但仅限于适配新接口
- 不要做任何"顺手重构"，哪怕看到了可以改进的地方，除非明确要求
- 只改必须改的，不顺手"完善"别处

### 4. 目标驱动，自我验证
- 每个任务的交付标准：代码能编译通过，且测试程序能生成有效的视频文件
- 完成编码后，主动说明如何验证
- 如果测试程序跑不通，主动分析原因并修复
- 定义清晰的成功标准，让 AI 自己循环验证直到通过

## 项目结构
Vane/
├── AGENTS.md
├── README.md
├── LICENSE
├── CMakeLists.txt
├── include/
│ └── Vane/
│ └── IVideoEncoder.h
├── src/
│ ├── CMakeLists.txt
│ ├── FVTEncoder.cpp # macOS VideoToolbox
│ ├── FVTEncoder.h
│ ├── FMFEncoder.cpp # Windows Media Foundation
│ ├── FMFEncoder.h
│ ├── FVAEncoder.cpp # Linux VA-API
│ └── FVAEncoder.h
└── test/
├── CMakeLists.txt
└── main.cpp

## 编码规范

### C++ 标准
- 使用 C++17
- 不引入任何第三方依赖，仅使用各平台系统框架

### 命名约定
- 类名：`F` 前缀加 PascalCase，如 `FVTEncoder`
- 接口：`I` 前缀加 PascalCase，如 `IVideoEncoder`
- 函数：PascalCase，如 `EncodeFrame()`
- 成员变量：`b` 前缀表示 bool，如 `bIsRecording`
- 常量：`k` 前缀，如 `kDefaultBitRate`

### 平台隔离
使用预处理器宏隔离平台代码：

#if PLATFORM_MAC
    // VideoToolbox 实现
#elif PLATFORM_WINDOWS
    // Media Foundation 实现
#elif PLATFORM_LINUX
    // VA-API 实现
#endif

### 错误处理
- 所有系统 API 调用必须检查返回值
- VideoToolbox：检查 OSStatus
- Media Foundation：检查 HRESULT
- VA-API：检查 VAStatus
- 错误信息通过 GetLastError() 方法返回

## 当前开发阶段
**第一阶段：macOS 平台先行验证**
- 只实现 FVTEncoder（VideoToolbox）
- 测试程序生成 1 秒纯蓝色 60fps 1920x1080 视频
- 验证通过后再扩展到其他平台

## 验证标准
测试程序成功运行的标准：
1. 编译零警告
2. 运行后生成 test_output.mp4 文件
3. 文件大小大于 0
4. 使用系统播放器能正常打开并播放纯蓝色画面

## 禁止事项
- 不要引入 FFmpeg 或其他第三方视频库
- 不要实现网络流媒体功能
- 不要添加 GUI 界面
- 不要提前实现未要求的平台
- 不要修改 CMakeLists.txt 中的最低 C++ 标准
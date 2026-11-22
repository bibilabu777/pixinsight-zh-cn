# 更新日志

## v1.2.1 — 2026-11-22

### 变更
- **仓库迁移**至 <https://github.com/bibilabu777/pixinsight-zh-cn>，原地址不再更新。
- README 增加项目主页、下载方式与 `SHA256SUMS.txt` 校验步骤。

### 说明
- 本版**不含代码与词典内容变化**：`bin/`、`src/`、`zh_cn.tsv` 与 v1.2.0 逐字节一致
  （`zh_cn.tsv` SHA256 `c69bd7bc…27f4`，共 24,603 条）。功能、界面效果与 v1.2.0 完全相同，
  只是发布地址与文档更新。

## v1.2.0 — 2026-11-22

### 新增
- **PI 原生模块 tooltip 汉化补齐至 99.90%**：可组装的富文本提示从 1,735 条增至 **2,074 / 2,076 条**。
  本轮新完成 23 个模块，全部从 0 覆盖到 100%：
  MultiscaleProcessing、ColorCalibration、EphemerisGeneration、SubframeSelector、
  BackgroundModelization、FastIntegration、INDIClient、Debayer、CometAlignment、
  Compatibility（ATrousWaveletTransform）、GeneralizedHyperbolicStretch、Image（DynamicPSF）、
  Convolution、FITS、XISF、IntensityTransformations、APASS、Deconvolution、
  CosmeticCorrection、MachineLearning、NSGXnml、RAW、MultiscaleAdaptiveStretch。
- 词典新增 **338 条**，从 24,265 条增至 **24,603 条**（5.05 MB）。

### 改进
- 建立**段落级翻译记忆**（`zh_paras.tsv`，1,710 条）：同一段落在多个 tooltip 中复用译文，
  避免重复翻译，也保证跨模块术语一致。
- 组装脚本改为「扫描全部 tooltip → 逐段匹配 → 整串写入」，对已存在键幂等跳过，
  可反复安全执行。

### 已知限制
本轮后仍有 **2 条**无法汉化，属合理例外：
- `INDIClient` 文件名模板说明中含示例文件名 `LIGHT_B2x2_E300.00_002.fits`，该段是纯文件名，不宜翻译。
- `SubframeSelector` 属性列表（`Altitude | Azimuth | FWHM | …`）本身是表达式里使用的标识符，
  译成中文反而会让用户无法书写审批/加权表达式。

## v1.1.0 — 2026-11-22

### 新增
- **参数悬停提示（tooltip）汉化层**：补入 PI 原生模块的富文本 tooltip 译文，覆盖
  PixelMath、Global（全局参数）、ImageIntegration、ImageCalibration、ImageRegistration
  等核心模块，共新增 **2,220 条**词条。
- **`docs/screenshots/`**：四张实机效果图，README 内嵌展示。

### 修复
- **1,744 条已译 tooltip 完全失效**：原词典存的是剥掉 `<p>` 标签的纯文本键，而运行期 PI 传入的是
  `<p>…</p>` 富文本，两者永远匹配不上。已批量补齐标签，译文直接复用。
- **`Cosmic Clarity - SASpro` 菜单项不翻译**：词典第 1011 行是一条自映射占位，
  词典重复键取**先入先出**，后追加的译条永不生效。已就地改写。
- **`src/build.py` 在本机必然失败**：原脚本把 VS 路径、SDK 版本、工程根写成作者本机绝对路径。
  已改为 `vswhere` / 注册表探测 + 按文件位置推导根目录，缺编译器时给可读报错。
- 删除 6 条尾随空格的冗余键（加载时被裁空格，永远查不到且与前面的键冲突）。
- 对齐 2 条译文的分行数（AutoDBE 参数提示、Perfect Palette Picker 帮助）。

### 改进
- 新增 `.gitattributes`：`* -text` 禁止 git 做行尾转换，避免 `zh_cn.tsv` 的 CRLF 被改坏。
- 词典从 22,045 条增至 **24,265 条**（4.98 MB）。

### 已知限制
- 左侧资源管理器栏（`Process Explorer` 等 6 项）未汉化：PI 核心经私有函数指针表调用 Qt，
  现有 DLL 钩子覆盖不到，需重编 DLL。
- 两侧带空格的按钮文本、由变量拼接的长段落无法用词典命中。

## v1.0.0 — 2026-11-21

首个公开版本：外挂式 DLL 注入 + 22,045 条词典，覆盖 PI 主界面与 50+ 第三方脚本界面文案。

# 更新日志

## v1.3.1 — 2026-11-24

### 新增
- **界面文案补充 98 条**，词典从 25,817 条增至 **25,915 条**：
  - 对话框标题/文件名框 **27 条**：`Catalog Downloader`、`Vizier Servers`、`Object Configuration`、
    `Add Layer`、`Preview Annotation`、`Tile Coordinates`、`Benchmark Report`、`Manual Entry`、
    `Select Libraries`、`Launch Scriptlet`、`Save Ephemerides` 等。
  - 参数标签/复选框/枚举项 **53 条**：HR 图与星点数据面板的 `Distance:`、`Magnitude:`、
    `Proper motion (RA|Dec|Direction):`、`G/BP/RP magnitude:`、`Red/Green/Blue signal:`，
    BPP 的 `Optimize Master Dark`、`no weighting`、`Resolution weights`、`not applicable`、
    `canceling...` / `running` / `success` / `failed` / `canceled`，
    AnnotateImage 的 `Write objects to a text file`，以及 `Presets`、`Smart Report`、
    `Export CSV`、`Save Plot`、`Update Worklist` 等。
  - 短富文本提示 **9 条**：`<p>Execute.</p>`、`<p>Accept new parameters</p>`、
    `<b>Gaia data</b>`、`<b>Image data</b>`、`<p>Select colour dialog</p>` 等。
- **`src/tools/scan_js.py`**：界面文案扫描工具。遍历 PI 的 JS 脚本目录抽取界面字符串，
  按 DLL 的匹配链（精确 → 去尾空白 → 去尾冒号 → 去 `&` → 剥富文本标签 → 真换行转字面 `\n`）
  核对词典并输出未命中清单，用于每次 PI 升级或新装脚本后查漏。

### 修复
- **原文带真实换行的标签永远不翻译**（本次主要问题）。ContinuumSubtraction 的
  `RGB view or\nred channel view:`、`Narrowband view or\nHa view:` 与
  CombineRGBAndNarrowband 的 `Narrowband Red\nor NB Color:` 在脚本里是
  **真换行**（`"RGB view or\nred channel view:"`），而词典里只有空格版本
  （`RGB view or red channel view:`），二者永远不相等，界面上一直是英文。
  已按词典规则以字面 `\n` 建键补齐。
- **3 条键把转义写成了字面量**：`Right Ascension\nh\u2003m\u2003s`、
  `Declination\n\u00B0\u2003\u2032\u2003\u2033`、`Pixel Size\n\u03BCm` 的键里存的是
  反斜杠加 `u2003` 六个字符，而运行期是真正的 U+2003 全角空格，因此从未命中。
  已改为真实字符，`Observation Time\nUTC` / `Focal Length\nmm` 保持原键。
- **9 条多行标签的译文不再写 `\n`**：DLL 只把键里的字面 `\n` 还原用于匹配，
  不会还原译文里的 `\n`（`qcache_str` 直接 `fromUtf8`），译文里写 `\n` 会在界面
  显示成反斜杠加 n。这批标签（含 BPP 多行表头 `焦距\nmm` 等 4 条 v1.3.0 遗留）
  统一改为单行译法，中文均能在原列宽内排下。

### 校验
- 格式：UTF-8 BOM 保留 · 25,915 行全 CRLF · 每行 1 个制表符 · 空键空值 0 · 重复键 0 ·
  行首 `#` 0 · 前导空格 0 · 键/值均不含真实换行。
- 按 `builddict()` 规则模拟加载：**25,915 / 25,915 全部可加载**；
  最长键 3,873 字符，未触及 8,000 上限。
- 逐条模拟 `T()` 匹配链验证：本轮涉及的 19 个运行期字符串（含 12 条换行类）
  **全部命中**。
- 实机加载日志（PixInsight 1.9.6）：`dict loaded: 25817 entries` →
  本版应为 `25915 entries, 0 failed, skipped_long=0`。

### 说明
- 本轮未改代码，`bin/` 与 `src/*.cpp` 逐字节不变，可直接覆盖安装。
- 扫描结论（`src/scripts` 全量未命中 1,642 条，工具口径同 `src/tools/scan_js.py`）：
  - 本轮补齐 **98 条**。
  - 剩余约 **240 条**仍需人工判断，主要集中在：`AutoPaletteStudio` 的约 30 条
    英文长 tooltip（该脚本已有部分段落为中文，剩下的未译）、部分脚本的表格表头
    与状态列文本（`Overscan\nInfo` 一类已处理，其余为运行时拼接）、以及
    `* HA +`、`"/px`、`*master*` 这类拼接片段与单位符号——后者整串无法预知，
    补了也不会被匹配，属噪声。
  - 另有作者署名、版权、网址、算法标识符、控制台日志，按项目约定保留英文。


## v1.3.0 — 2026-11-22

### 新增
- **错误与校验消息汉化 907 条**，词典从 24,604 条增至 **25,817 条**（5.14 MB）。
  这批词条是 PI 出错时真正弹给用户看的文本，此前几乎全是英文：
  - **PCL 核心错误池 861 条**：`Unable to open file:`、`Cannot read TIFF header`、
    `No space for LZW code table`、`Missing or invalid master dark frame.`、
    `Invalid image width:`、`Corrupt JPEG data: bad Huffman code`、
    `Settings: Error writing global integer data`、`Tile length is zero` 等。
  - **文件格式库补充 43 条**：`The FITS file contains no readable image`、
    `This implementation does not support complex FITS images`、`Old-style JPEG`、
    `GDAL Metadata`、`PixInsight Standard TIFF File Format Module` 等。
  - **格式提示表头 3 条**：`FITS / TIFF / JPEG Format Hints … Description`，按原空格数对齐。

### 修复
- **36 条历史无效条目**，它们在 v1.2.1 里从未生效：
  - **15 条键带前导空格**（`L-band 546.0 nm`、`Scale rejection`、`~300 nm L-bandwidth` 等）。
    加载器 `builddict()` 会剥掉行首空白，这些键永远查不到。已去掉前导空格。
  - **19 条尾随空格与不带空格版本重复**（`Same as source ` / `Same as source` 等）。
    加载器剥尾空格后第二条被静默丢弃。已统一保留不带空格的版本——loader 本身有
    尾部空白回退，带空格的那份是多余的。
  - **1 条键以 `#` 开头**（`# | Session`），被加载器当注释行跳过，已删除。

### 校验
- 格式：UTF-8 BOM 保留 · 25,817 行全 CRLF · 制表符每行 1 个 · 空键空值 0 · 重复键 0 ·
  行首 `#` 0 · 前导空格 0。
- 按 `src/pi_zh.cpp` 中 `builddict()` 的解析规则做模拟加载：**25,817 / 25,817 全部可加载，
  冲突丢弃 0**（覆盖率 100%）。
- 61 条短键（`Not enough`、`Cannot read`、`Read error` 等）逐一核对：`findkey()` 是
  整串精确匹配（长度相等 + `memcmp`），不存在把长句拆坏的风险。

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

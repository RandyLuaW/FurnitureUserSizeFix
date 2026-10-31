#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

// SKSEMenuFramework.h 内部 include <windows.h>，必须放在 CommonLib 之后
// （CommonLib 的 REX/W32/BASE.h 有 #error 拦截先 include Windows.h 的情况）
// 仅在定义了 FUSF_WITH_MENU 时引入（该头为 GPL-3.0，不随本仓库分发）。
#if defined(FUSF_WITH_MENU)
#	include "SKSEMenuFramework.h"
#endif

namespace logger = SKSE::log;
using namespace std::literals;

namespace
{
    // =====================================================================
    //  偏移量模型
    //    三个轴都相对【角色自身朝向】：
    //      fwd  = 沿角色正前方（+ 向前，- 向后）
    //      lat  = 角色右手方向
    //      vert = 世界垂直
    //    家具类型不同取值也不同：椅子基本只用 vert；磨刀石/附魔台需要
    //    vert + 向正后方（fwd 为负）。
    // =====================================================================
    struct Offset
    {
        float fwd = 0.0f;
        float lat = 0.0f;
        float vert = 0.0f;
    };

    // 偏移的取值来源，优先级从高到低。用于菜单显示与日志判读。
    enum class OffsetSource
    {
        kNone,          // 谁都没有 → not fixed
        kMarker,        // [Markers]   模型@坐点（最精确，同款同坐点共享）
        kModel,         // [Models]    纯模型兜底
        kFormID,        // [Furniture] 单件覆盖
        kBenchDefault,  // [Defaults]  按 TESFurniture::workBenchData.benchType 的默认值表
        kAnimType,      // [AnimType]  按 NIF 里 BSFurnitureMarker.animationType 的档位
        kFormula        // built-in formula（fGlobalScale=0 时结果恒为 0，等于not fixed）
    };

    const char* SourceName(OffsetSource a_src)
    {
        switch (a_src) {
        case OffsetSource::kMarker:
            return "[Markers] model@marker";
        case OffsetSource::kModel:
            return "[Models] model";
        case OffsetSource::kFormID:
            return "[Furniture] per-ref";
        case OffsetSource::kBenchDefault:
            return "[Defaults] benchType";
        case OffsetSource::kAnimType:
            return "[AnimType] anim tier";
        case OffsetSource::kFormula:
            return "built-in formula";
        default:
            return "none (not fixed)";
        }
    }

    struct Config
    {
        float seatHeight = 30.0f;   // 默认公式用（仅在没手动覆盖时生效）
        float globalScale = 0.0f;   // 0 = 关闭默认修复，完全由手动覆盖决定
        // ★ v0.10.2 体型补偿：FHSF 的公式骨架是 lift = (1-s) * Adjust，抬升量与 (1-s) 成正比。
        //   v0.5 之后我们把原始世界偏移直接套给所有 actor，等于假设所有人都是同一个体型 ——
        //   实测日志里 scale=1.010 的 NPC 也拿到了 5.00 的抬升，这是错的（1.0 体型应该为 0）。
        //   这里把 INI 里存的数值解释成"在 fCalibrationScale 体型下标定出来的量"，
        //   再按 (1-s)/(1-标定体型) 换算到当前 actor。玩家(0.65)系数恒为 1，手感不变。
        bool scaleCompensate = true;
        // ★ 2026-11-01 用户决定：体型 >= 1.0 的角色**默认完全不动**。
        //   (0.65→1.00) 有锚点，但 s>1 是线性外推、从未实测；s=1.5 会让进深反向 36 单位。
        bool clampAboveOne = true;
        // ★ 2026-11-01 新需求：允许对体型 > 1.0 的角色也生效（默认关闭）。
        //   打开后 clampAboveOne 被覆盖，走线性外推（负向系数 = 把角色往下压）。
        //   ⚠️ 该路径**未经实机测试**，菜单里必须显示警告。
        bool allowAboveOne = false;
        float calibrationScale = 0.65f;  // 用户所有实测值都是在 0.65 的玩家身上调的
        // 探针：坐上去时把骨架各关键节点相对根节点的高度打进日志（只记录，不改行为）。
        // 目的：验证"h 能不能在运行时直接量出来"——量得出来就能做真正的自动初值。
        bool probeBones = true;

        // ---- 上肢 IK（v0.13.0 从 FurnitureHandIK 合并进来）----
        // 站立式工作台上"抬整个人"会让脚离地；改为**只绕肩旋转上臂**把手抬起来，
        // 根节点不动 ⇒ 脚踏实。这是 h 之外的第二套手段，两者可叠加。
        bool  useArmIK = true;        // false = 完全回到 v0.12.5 的老行为
        float maxArmDeg = 75.0f;      // 单件角度上限；超出部分自动转成整体抬升
        float defArmDeg = 45.0f;      // 未在 [ArmIK] 里列出、但属于站立式工作台时用这个
        float defArmLift = 0.0f;      // 同上，配套的残余整体抬升
    };
    Config g_config;

    // 手臂 IK 的单件参数（0.65 体型口径，与其余配置一致）
    struct ArmTune
    {
        float armDeg = 0.0f;
        float lift = 0.0f;
    };
    std::unordered_map<std::string, ArmTune> g_armTunes;


    // ---------------- 多语言（A 方案：INI [Menu] 段 + 独立语言包）----------------
    // 为什么不做"自动识别游戏语言"：本机（中文整合包）`Skyrim.ini` 里 sLanguage 恒为
    // ENGLISH —— 汉化是把中文塞进 `_english` 字符串文件 + DSD，游戏语言设置根本不改。
    // 所以"语言由文件决定"是唯一不会判错的做法：
    //   ① Data/SKSE/Plugins/FurnitureUserSizeFix_Lang.ini 的 [Menu]   （语言包，优先级最高）
    //   ② 主 INI 的 [Menu]                                            （用户就地覆盖）
    //   ③ 内置英文                                                     （兜底，永不空白）
    // 语言包独立成文件还有个好处：SaveConfig 只重写主 INI，永远不会把翻译抹掉。
    std::unordered_map<std::string, std::string> g_lang;

    // 统计格式占位符个数（跳过 %% 与 {{），用来校验译文没吃掉/多出占位符
    // 同时支持两种：printf 风格 %s/%u/%08X（Text）与 std::format 风格 {}（按钮标签）
    std::size_t CountFormatSpecs(std::string_view a_fmt)
    {
        std::size_t n = 0;
        for (std::size_t i = 0; i < a_fmt.size(); ++i) {
            const char c = a_fmt[i];
            if (c == '%') {
                if (i + 1 < a_fmt.size() && a_fmt[i + 1] == '%') {
                    ++i;
                    continue;
                }
                ++n;
            } else if (c == '{') {
                if (i + 1 < a_fmt.size() && a_fmt[i + 1] == '{') {
                    ++i;
                    continue;
                }
                ++n;
            }
        }
        return n;
    }

    void LoadLangFile(const char* a_path, bool a_override)
    {
        std::ifstream f(a_path);
        if (!f.is_open()) {
            if (a_override) {
                logger::info("Language pack not found (using built-in English): {}", a_path);
            }
            return;
        }
        std::string line;
        std::string section;
        auto trim = [](std::string& x) {
            const auto b = x.find_first_not_of(" \t\r\n");
            const auto e = x.find_last_not_of(" \t\r\n");
            x = (b == std::string::npos) ? std::string{} : x.substr(b, e - b + 1);
        };
        std::size_t loaded = 0;
        while (std::getline(f, line)) {
            // ★ 必须先剥掉 UTF-8 BOM：带 BOM 的文件首行是 "\xEF\xBB\xBF[Menu]"，
            //   若按 line.front()=='[' 判断段头就会整体失配 ⇒ 全部条目被静默跳过。
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            trim(line);
            if (line.empty() || line.front() == ';' || line.front() == '#') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            if (section != "Menu") {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            trim(k);
            trim(v);
            if (k.empty()) {
                continue;
            }
            if (a_override || g_lang.find(k) == g_lang.end()) {
                g_lang[k] = v;
                ++loaded;
            }
        }
        if (loaded) {
            logger::info("Language pack: {} -> {} menu strings", a_path, loaded);
        } else {
            // 有文件但一条都没解析出来，是最容易"静默失效"的情形，必须显式告警
            logger::warn("Language pack: {} opened but yielded 0 strings "
                         "(missing [Menu] header? UTF-16 encoding? wrong key names?)",
                a_path);
        }
    }

    void LoadLang()
    {
        // ★ 重载时不要"先清空": 万一语言包这次没读到（文件被占用/被 MO2 覆盖顺序影响），
        //   清空会让界面从中文瞬间变回英文。保留上一份，读失败就沿用。
        const auto backup = g_lang;
        g_lang.clear();
        // 主 INI 次之，语言包覆盖它
        LoadLangFile("Data/SKSE/Plugins/FurnitureUserSizeFix.ini", false);
        LoadLangFile("Data/SKSE/Plugins/FurnitureUserSizeFix_Lang.ini", true);
        if (g_lang.empty() && !backup.empty()) {
            g_lang = backup;
            logger::warn("Language pack reload yielded nothing; keeping the previously loaded table ({} strings)",
                g_lang.size());
        }
    }

    // 用运行时格式串填充 {}（std::format 要求格式串是编译期常量，而语言包是运行时的）
    std::string FillBraces(std::string a_tpl, const std::vector<std::string>& a_args)
    {
        for (const auto& arg : a_args) {
            const auto pos = a_tpl.find("{}");
            if (pos == std::string::npos) {
                break;
            }
            a_tpl.replace(pos, 2, arg);
        }
        return a_tpl;
    }

    // 取一条菜单文案：有译文用译文，否则用内置英文
    const char* L(const char* a_key, const char* a_english)    {
        const auto it = g_lang.find(a_key);
        if (it == g_lang.end() || it->second.empty()) {
            return a_english;
        }
        // 占位符数量必须一致，否则 fmt 会崩 —— 不一致就退回英文并告警
        if (CountFormatSpecs(it->second) != CountFormatSpecs(a_english)) {
            logger::warn("Menu string '{}' has a different format-specifier count than English; falling back", a_key);
            return a_english;
        }
        return it->second.c_str();
    }

    // 家具基对象 FormID → 手动偏移（旧键，保留兼容）
    std::unordered_map<RE::FormID, Offset> g_overrides;

    // 模型路径（小写规范化）→ 手动偏移（次级键，未带 marker 时兜底）
    std::unordered_map<std::string, Offset> g_modelOverrides;

    // 「模型路径@marker位掩码」→ 手动偏移（主键）
    // 同一个 NIF 可能被多个 FURN 记录引用，而每个记录可以启用不同的
    // ActiveMarker（NAM0 / TESFurniture::furnFlags），也就是 NIF 里不同的坐点。
    // 例：UpperChair01F   = Clutter\Upperclass\UpperChair01.nif @ 0xC0000 (kSit18|kSit19)
    //     UpperChair01FL  = Clutter\Upperclass\UpperChair01.nif @ 0x40000 (kSit18)
    // 坐点不同 ⇒ 需要的补偿也不同，所以主键必须带上 marker 位。
    std::unordered_map<std::string, Offset> g_markerOverrides;

    // 工作台类型（TESFurniture::workBenchData.benchType）→ 该类型的默认偏移
    // 段名 [Defaults]，key 是 benchType 的【十进制】值：
    //   0 = kNone             普通坐具/椅子（所有非工作台的 benchType 都是 0）
    //   2 = kSmithingWeapon   磨刀石
    //   7 = kSmithingArmor    护甲台
    // 只为【用户实测过】的类型给默认值。3(附魔)/5(炼金) 等没有实测数据，
    // 不设默认值，保持"只能手动"的现状，避免编造数字。
    std::unordered_map<std::uint32_t, Offset> g_benchDefaults;

    // 家具动画档（NIF 里 BSFurnitureMarker.animationType）→ 该档的默认偏移
    // 段名 [AnimType]，key 是 animationType 的【十进制】值（位掩码语义）：
    //   1 = kSit    坐（椅子/长凳/王座，以及全部工作台 —— 实测 159/159 都是 1）
    //   2 = kSleep  躺（床/铺盖 —— 实测 120/120 都是 2）
    //   4 = kLean   靠（本机 400 条 FURN 里一条都没有）
    // 为什么需要它：[Defaults] 的 benchType 对"床"和"椅子"都是 0，两者会撞在同一档，
    // 而它们的正确抬升量完全不是一回事（椅子坐、床躺）。animationType 是唯一能把
    // 这两者分开的、运行时可读的信号。
    std::unordered_map<std::uint32_t, Offset> g_animOverrides;

    // 某件家具的 marker 信息（按基对象 FormID 缓存，读一次就够）
    struct AnimInfo
    {
        bool valid = false;          // 是否成功从 NIF 读到 BSFurnitureMarkerNode
        std::uint32_t mask = 0;      // 该 NIF 全部坐点 animationType 的位掩码并集
        std::uint32_t firstType = 0; // 第一个坐点的 animationType（仅用于显示）
        std::uint32_t entryProps = 0; // 第一个坐点的 entryProperties（仅用于显示）
        std::uint32_t count = 0;     // 坐点数量
    };
    std::unordered_map<RE::FormID, AnimInfo> g_animCache;
    // 连续读失败计数：家具 3D 还没加载时读不到是正常的，但菜单每帧都重试会把
    // 一次失败变成每帧一次树遍历。累计 3 次就封存"读不到"，等下次读档或用户
    // 点「重新读取动画类型」再放开。
    std::unordered_map<RE::FormID, std::uint32_t> g_animFailCount;

    // actor FormID → 我们给它登记的那个根节点。
    // 【为什么需要】EXIT 时**不能**再用 a->Get3D() 反推键：第一人称/第三人称是两套 3D，
    // 家具退出瞬间 Get3D() 可能已经换成另一个（或为 null），erase 就落空 ⇒ 偏移残留 ⇒
    // 角色浮空，要再坐一次再退出才清掉（2026-10-31 用户实测 + 日志实证）。
    // 记一份"当初插的是哪个节点"，EXIT 时按记录删，才是可靠的。
    // actor FormID → 该 actor 登记过的**全部**根节点。
    // 【为什么是复数】角色有两套 3D：第三人称体 和 第一人称体（Improved Camera 之类
    // 会让第一人称也显示完整身体）。旧版一次 ENTER 只登记 `Get3D()` 返回的那一个，
    // 于是第一人称下看到的是**另一套没被抬升的身体** ⇒ 穿模。日志实证：同一件家具
    // 两次 ENTER 的 root 指针分别是 `0x26b9dc3c100` 与 `0x26b9dc3d300`（两个节点）。
    std::unordered_map<RE::FormID, std::vector<RE::NiAVObject*>> g_actorRoots;

    // 收集某 actor 当前存在的全部 3D 根节点（去重、去空）
    std::vector<RE::NiAVObject*> CollectActorRoots(RE::Actor* a_actor)
    {
        std::vector<RE::NiAVObject*> out;
        if (!a_actor) {
            return out;
        }
        for (const bool fp : { false, true }) {
            auto* n = a_actor->Get3D(fp);
            if (n && std::find(out.begin(), out.end(), n) == out.end()) {
                out.push_back(n);
            }
        }
        return out;
    }

    // 禁止修复的家具（基对象 FormID）。FHSF 的家具脚本**当年失败的直接原因之一**就在这里：
    // 它的 MCM 里有一张 `furnforbid` 名单（76 条），第一个就是 SexLab 的定位用家具
    // `0x45A93|SexLab.esm`（EDID = SexLabBaseMarker，模型 Furniture\WallLeanMarker.nif）。
    // SexLab 场景会把角色"传送"到这件家具上，若我们也去抬升，场景里的人就会浮起来。
    // 段名 [Forbid]，key 支持两种写法：`SexLab.esm:045A93`（按插件+本地 ID，跨 load order 稳）
    // 或 `0012ABCD`（运行时 FormID，直接比对）。
    std::unordered_set<RE::FormID> g_forbid;
    std::vector<std::string> g_forbidSpecs;  // 原样保留写回用

    // 路径规范化（小写 + 反斜杠），空路径返回空串
    std::string NormalizeModelPath(const char* a_path)
    {
        if (!a_path || !*a_path) {
            return {};
        }
        std::string s{ a_path };
        for (auto& c : s) {
            if (c == '/') {
                c = '\\';
            } else {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        return s;
    }

    // 取家具的模型路径并规范化，取不到返回空串
    std::string GetModelKey(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) {
            return {};
        }
        auto* base = a_ref->GetBaseObject();
        if (!base) {
            return {};
        }
        // TESFurniture -> TESObjectACTI -> TESModelTextureSwap -> TESModel::GetModel()
        if (auto* acti = base->As<RE::TESObjectACTI>()) {
            return NormalizeModelPath(acti->GetModel());
        }
        return {};
    }

    // 菜单里只有 FormID（家具对象可能已卸载，但基对象常驻）
    std::string GetModelKeyByFormID(RE::FormID a_id)
    {
        if (a_id == 0) {
            return {};
        }
        if (auto* base = RE::TESForm::LookupByID(a_id)) {
            if (auto* acti = base->As<RE::TESObjectACTI>()) {
                return NormalizeModelPath(acti->GetModel());
            }
        }
        return {};
    }

    // 取家具的 marker 位掩码（TESFurniture::furnFlags / NAM0）
    std::uint32_t GetMarkerFlags(const RE::TESForm* a_base)
    {
        if (!a_base) {
            return 0;
        }
        if (auto* furn = const_cast<RE::TESForm*>(a_base)->As<RE::TESFurniture>()) {
            return static_cast<std::uint32_t>(furn->furnFlags.get());
        }
        return 0;
    }

    // 取工作台类型（WBDT 子记录的 benchType）。椅子这类非工作台恒为 kNone = 0，
    // 所以 [Defaults] 里的 0 就是"普通坐具"这一档。
    std::uint32_t GetBenchType(const RE::TESForm* a_base)
    {
        if (!a_base) {
            return 0;
        }
        if (auto* furn = const_cast<RE::TESForm*>(a_base)->As<RE::TESFurniture>()) {
            return static_cast<std::uint32_t>(furn->workBenchData.benchType.get());
        }
        return 0;
    }

    // benchType 的可读名（只为已知类型给出，未知的就显示数字）
    const char* BenchTypeName(std::uint32_t a_type)
    {
        switch (a_type) {
        case 0:
            return "kNone (seat)";
        case 1:
            return "kCreateObject";
        case 2:
            return "kSmithingWeapon (grindstone)";
        case 3:
            return "kEnchanting (enchanting)";
        case 4:
            return "kEnchantingExperiment";
        case 5:
            return "kAlchemy (alchemy)";
        case 6:
            return "kAlchemyExperiment";
        case 7:
            return "kSmithingArmor (armor bench)";
        default:
            return "unknown";
        }
    }

    // animationType 的可读名。注意它是【位掩码】：一个坐点可以同时是 kSit|kLean。
    const char* AnimTypeName(std::uint32_t a_type)
    {
        switch (a_type) {
        case 1:
            return "kSit (sit)";
        case 2:
            return "kSleep (sleep)";
        case 4:
            return "kLean (lean)";
        case 0:
            return "none";
        default:
            return "combined";
        }
    }

    // 把一个位掩码渲染成可读串，例：mask=2 → "kSleep"，mask=3 → "kSit|kSleep"
    std::string AnimMaskName(std::uint32_t a_mask)
    {
        if (a_mask == 0) {
            return "none";
        }
        std::string out;
        auto add = [&out](const char* s) {
            if (!out.empty()) {
                out += '|';
            }
            out += s;
        };
        if (a_mask & 1) {
            add("kSit");
        }
        if (a_mask & 2) {
            add("kSleep");
        }
        if (a_mask & 4) {
            add("kLean");
        }
        if (out.empty()) {
            out = std::format("0x{:X}", a_mask);
        }
        return out;
    }

    // ---------------- 运行时读 NIF 的 BSFurnitureMarker ----------------
    // 家具的 marker 挂在模型根节点的 NiExtraData 上（NIF 里的名字是 "FRN"）。
    // 三级取法，任一级命中即可：
    //   ① 线性扫 extra data 比对名字 "FRN"（NIF 里的标准名，实测 5/5 件都是它）
    //   ② 线性扫 extra data 比对 NiRTTI（防别的作者换了名字）
    //   ③ 深度 ≤3 的 DFS 找子节点（防 marker 挂在子 Node 上）
    // 读不到就返回 nullptr，调用方按"没有动画档"处理 —— 绝不猜。
    //
    // ⚠️ 故意【不】用 CommonLib 的 NiObjectNET::GetExtraData(BSFixedString)：
    //    它是对 extra[] 数组做二分查找、且完全不判空（extra / extra[i] 都直接解引用），
    //    并且依赖数组按名字有序。自己线性扫一遍既无空指针风险，也不受排序前提影响。
    RE::BSFurnitureMarkerNode* FindFurnitureMarker(RE::NiAVObject* a_node, int a_depth)
    {
        if (!a_node || a_depth > 3) {
            return nullptr;
        }
        // NiAVObject 继承 NiObjectNET，extra data 就在这一层
        auto* net = static_cast<RE::NiObjectNET*>(a_node);
        const auto total = net->GetExtraDataSize();
        if (total > 0 && total < 4096) {
            // ① 按名字（实测 NIF 里就是 "FRN"）
            for (std::uint16_t i = 0; i < total; ++i) {
                auto* ed = net->GetExtraDataAt(i);
                if (ed && ed->GetName() == "FRN"sv) {
                    return static_cast<RE::BSFurnitureMarkerNode*>(ed);
                }
            }
            // ② 按 RTTI（RTTI 取不到或异常时安静跳过）
            try {
                const REL::Relocation<const RE::NiRTTI*> rtti{ RE::BSFurnitureMarkerNode::NiRTTI };
                const auto expected = rtti.get();
                if (expected) {
                    for (std::uint16_t i = 0; i < total; ++i) {
                        auto* ed = net->GetExtraDataAt(i);
                        if (ed && ed->GetRTTI() == expected) {
                            return static_cast<RE::BSFurnitureMarkerNode*>(ed);
                        }
                    }
                }
            } catch (...) {
                // 忽略：RTTI 不可用只是少一条兜底路径
            }
        }
        // ③ 子节点
        if (auto* asNode = a_node->AsNode()) {
            for (auto& child : asNode->GetChildren()) {
                if (auto* hit = FindFurnitureMarker(child.get(), a_depth + 1)) {
                    return hit;
                }
            }
        }
        return nullptr;
    }

    // 从已加载的 3D 里读出 marker 汇总信息
    AnimInfo ReadAnimInfoFrom3D(RE::NiAVObject* a_root)
    {
        AnimInfo out{};
        if (!a_root) {
            return out;
        }
        auto* marker = FindFurnitureMarker(a_root, 0);
        if (!marker) {
            return out;
        }
        const auto n = marker->markers.size();
        out.count = static_cast<std::uint32_t>(std::min<std::size_t>(n, 64));
        for (std::uint32_t i = 0; i < out.count; ++i) {
            const auto& m = marker->markers[i];
            out.mask |= static_cast<std::uint32_t>(m.animationType.underlying());
            if (i == 0) {
                out.firstType = static_cast<std::uint32_t>(m.animationType.underlying());
                out.entryProps = static_cast<std::uint32_t>(m.entryProperties.underlying());
            }
        }
        out.valid = (out.count > 0);
        return out;
    }

    // 取某件家具的 marker 信息（带缓存）。a_forceReread 用于菜单里的"重新读取"。
    AnimInfo GetAnimInfo(RE::TESObjectREFR* a_furn, bool a_forceReread = false)
    {
        if (!a_furn) {
            return {};
        }
        auto* base = a_furn->GetBaseObject();
        const auto baseID = base ? base->GetFormID() : 0;
        if (baseID == 0) {
            return {};
        }
        if (!a_forceReread) {
            if (const auto it = g_animCache.find(baseID); it != g_animCache.end()) {
                return it->second;
            }
            if (const auto fc = g_animFailCount.find(baseID);
                fc != g_animFailCount.end() && fc->second >= 3) {
                return {};
            }
        }
        const auto info = ReadAnimInfoFrom3D(a_furn->Get3D());
        if (info.valid) {
            g_animCache.insert_or_assign(baseID, info);
            g_animFailCount.erase(baseID);
        } else {
            ++g_animFailCount[baseID];
        }
        return info;
    }

    // 主键：模型路径@marker位（小写 16 进制，无 0x 前缀）
    std::string MakeMarkerKey(const std::string& a_model, std::uint32_t a_flags)
    {
        if (a_model.empty()) {
            return {};
        }
        return std::format("{}@{:X}", a_model, a_flags);
    }

    std::string GetMarkerKey(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) {
            return {};
        }
        auto* base = a_ref->GetBaseObject();
        return MakeMarkerKey(GetModelKey(a_ref), GetMarkerFlags(base));
    }

    std::string GetMarkerKeyByFormID(RE::FormID a_id)
    {
        if (a_id == 0) {
            return {};
        }
        if (auto* base = RE::TESForm::LookupByID(a_id)) {
            std::string model;
            if (auto* acti = base->As<RE::TESObjectACTI>()) {
                model = NormalizeModelPath(acti->GetModel());
            }
            return MakeMarkerKey(model, GetMarkerFlags(base));
        }
        return {};
    }

    // 把"角色局部三轴"换算成世界空间向量（用当前的 yaw）
    RE::NiPoint3 ToWorld(RE::Actor* a_actor, const Offset& a_off)
    {
        if (!a_actor) {
            return {};
        }
        const float yaw = a_actor->GetAngleZ();  // 弧度；0 = 面朝 +Y
        const float s = std::sin(yaw);
        const float c = std::cos(yaw);
        // 前方 = (sin, cos)；右方 = (cos, -sin)
        return RE::NiPoint3{
            a_off.fwd * s + a_off.lat * c,
            a_off.fwd * c - a_off.lat * s,
            a_off.vert
        };
    }

    // ★ 运行时目标表：根节点 → 该坐姿的【局部】偏移 + 是谁
    //
    // 【为什么存局部而不是世界向量】v0.12.0 及以前存的是 ENTER 那一刻算好的世界向量。
    // 但 `TESFurnitureEvent::kEnter` 是**在角色转向家具之前**发出的，那一刻
    // `GetAngleZ()` 还是坐下前的朝向 —— 烘焙出来的世界方向就是错的，而且会一直错到
    // 起身为止。用户实测：长椅/王座坐下偏移方向明显不对，在菜单里点一次「保存到共享值」
    // （那里会用**当前**朝向重算）就正常了；起身再坐又歪。
    // 现在改成**每帧用角色当前 yaw 重算**，顺带把"坐着时转身"也一并解决。
    struct SitEntry
    {
        Offset        local{};             // 已乘过体型系数 k 的局部三轴
        RE::FormID    actorID = 0;         // 用 FormID 而不是指针：无生命周期风险
        float         lastYaw = 0.0f;
        RE::NiPoint3  world{};
        bool          worldValid = false;
        std::uint32_t applies = 0;         // 诊断：这个目标被 hook 实际应用了几次
    };
    std::unordered_map<RE::NiAVObject*, SitEntry> g_sittingOffsets;

    // 菜单用：当前坐在什么家具上 / 当前编辑值 / 当前根节点
    std::atomic<RE::FormID> g_currentFurnID{ 0 };
    std::atomic<RE::NiAVObject*> g_currentRoot{ nullptr };
    Offset g_editOffset{};

    using UpdateWorldData_t = void (*)(RE::NiAVObject*, RE::NiUpdateData*);

    // ★ v0.13.0 起：vtable 是**按类分开的数组**。原来的单槽做法只能接管"根节点那个类"，
    //   骨骼（NiNode）是另一个类、另一个数组 ⇒ 骨头上的 UpdateWorldData 进不了 hook。
    //   手臂 IK 需要改骨骼，所以改成按槽位记录原函数，可接管多个类。
    // ★ 运行时相关：NiAVObject::UpdateWorldData 的 vtable 索引 SE = 0x30 / AE = 0x31
    //   （CommonLibSSE 源码里写作 REL::RelocateVirtual(0x30, 0x31, ...)，第一个参数就是 SE 索引）
    //   vtable 索引在同一运行时内是常量，但跨运行时不同 ⇒ 必须按运行时取，不能写死。
    //   用函数内静态量做惰性求值：REL::Module 需要 SKSE 初始化后才能查。
    inline std::size_t UwdIndexSE() { return 0x30; }
    inline std::size_t UwdIndexAE() { return 0x31; }

    std::size_t UwdIndex()
    {
        static const std::size_t idx = REL::Module::IsAE() ? UwdIndexAE() : UwdIndexSE();
        return idx;
    }

    struct SlotOrig
    {
        void**            slot = nullptr;   // &vtbl[UwdIndex()]
        UpdateWorldData_t orig = nullptr;
    };
    SlotOrig          g_slots[8]{};
    std::size_t       g_slotCount = 0;
    void**            g_cacheSlot = nullptr;     // 1 项缓存，热路径几乎零开销
    UpdateWorldData_t g_cacheOrig = nullptr;
    UpdateWorldData_t g_origUpdateWorldData = nullptr;   // 仅记录"root class"的原函数（兼容旧判断）

    void HookedUpdateWorldData(RE::NiAVObject* a_this, RE::NiUpdateData* a_data);

    UpdateWorldData_t OrigFor(RE::NiAVObject* a_this)
    {
        if (!a_this) {
            return nullptr;
        }
        auto** vtbl = *reinterpret_cast<void***>(a_this);
        if (!vtbl) {
            return nullptr;
        }
        void** slot = &vtbl[UwdIndex()];
        if (slot == g_cacheSlot) {
            return g_cacheOrig;
        }
        for (std::size_t i = 0; i < g_slotCount; ++i) {
            if (g_slots[i].slot == slot) {
                g_cacheSlot = slot;
                g_cacheOrig = g_slots[i].orig;
                return g_cacheOrig;
            }
        }
        return nullptr;
    }

    std::uint64_t g_applyCount = 0;

    // ---------------- 上肢 IK（从 FurnitureHandIK 合并）----------------
    bool EnsureHooked(RE::NiAVObject* a_node, const char* a_tag);   // 定义在后面
    float SizeFactor(float a_scale);          // 定义在后面（三轴/抬升的体型系数）
    float ArmAngleFactor(float a_scale);      // 定义在后面（手臂转角的体型系数）

    // ---------------- 有效体型（重要）----------------
    // 有些整合包把"矮"做在 **skeleton.nif 的节点缩放**上：race 默认身高 1.0、actor 的
    // GetScale() 也是 1.0，但骨架里 `NPC` / `NPC Root [Root]` 节点的 local.scale = 0.65。
    // 只看 GetScale() 会算出 (1-1.0)=0 的补偿 ⇒ 插件等于完全不工作。
    // FHSF 的 Papyrus 也是把 `GetNodeScale("NPC") × GetNodeScale("NPC Root [Root]")` 乘进去的。
    float NodeScaleOf(RE::NiAVObject* a_root, const char* a_name)
    {
        if (!a_root) {
            return 1.0f;
        }
        auto* n = a_root->GetObjectByName(RE::BSFixedString(a_name));
        if (!n) {
            return 1.0f;
        }
        const float s = n->local.scale;
        return (s > 0.001f && s < 100.0f) ? s : 1.0f;
    }

    float EffectiveScale(RE::Actor* a_actor)
    {
        if (!a_actor) {
            return 1.0f;
        }
        const float engine = a_actor->GetScale();
        const float skel = NodeScaleOf(a_actor->Get3D(false), "NPC") *
                           NodeScaleOf(a_actor->Get3D(false), "NPC Root [Root]");
        const float eff = engine * skel;
        if (eff <= 0.02f) {
            return engine;   // 骨架异常（还没加载/全 0）时退回纯引擎值
        }
        return eff;
    }

    struct ArmEntry
    {
        RE::FormID actorID = 0;
        float      armRad = 0.0f;   // 已乘体型系数
    };
    std::unordered_map<RE::NiAVObject*, ArmEntry>               g_armEntries;   // 手臂骨 → 参数
    std::unordered_map<RE::FormID, std::vector<RE::NiAVObject*>> g_actorArms;    // actor → 手臂骨

    // 骨骼候选名（不同骨架命名可能有差异，逐个试）
    const char* kArmLCands[] = { "NPC L UpperArm [LUar]", "NPC L UpperArm [LUp ]", "NPC L UpperArm" };
    const char* kArmRCands[] = { "NPC R UpperArm [RUar]", "NPC R UpperArm [RUp ]", "NPC R UpperArm" };

    RE::NiAVObject* FindArmBone(RE::NiAVObject* a_root, const char* const* a_c, std::size_t a_n, const char* a_tag)
    {
        if (!a_root) {
            return nullptr;
        }
        for (std::size_t i = 0; i < a_n; ++i) {
            if (auto* n = a_root->GetObjectByName(RE::BSFixedString(a_c[i]))) {
                if (i != 0) {
                    logger::warn("Upper-arm bone matched via fallback name: [{}] -> {}", a_tag, a_c[i]);
                }
                return n;
            }
        }
        logger::warn("Upper-arm bone not found: [{}]", a_tag);
        return nullptr;
    }

    RE::NiMatrix3 MakeRotAxis(const RE::NiPoint3& a_axis, float a_rad)
    {
        RE::NiPoint3 ax = a_axis;
        if (ax.Unitize() == 0.0f) {
            return RE::NiMatrix3{};
        }
        const float c = std::cos(a_rad), sn = std::sin(a_rad), t = 1.0f - c;
        const float x = ax.x, y = ax.y, z = ax.z;
        RE::NiMatrix3 m{};
        m.entry[0][0] = t * x * x + c;      m.entry[0][1] = t * x * y - sn * z; m.entry[0][2] = t * x * z + sn * y;
        m.entry[1][0] = t * x * y + sn * z; m.entry[1][1] = t * y * y + c;      m.entry[1][2] = t * y * z - sn * x;
        m.entry[2][0] = t * x * z - sn * y; m.entry[2][1] = t * y * z + sn * x; m.entry[2][2] = t * z * z + c;
        return m;
    }

    void ClearArms(RE::FormID a_actor)
    {
        const auto it = g_actorArms.find(a_actor);
        if (it == g_actorArms.end()) {
            return;
        }
        for (auto* n : it->second) {
            g_armEntries.erase(n);
        }
        g_actorArms.erase(it);
    }

    // 某件家具**实际生效**的手臂参数（显式条目 > 站立式默认值 > 0），已把超上限部分转成抬升。
    // 菜单显示 / ArmLiftFor / RegisterArmIK 三处共用 ⇒ 显示与实际永不脱节。
    ArmTune EffectiveArmTune(const std::string& a_model, std::uint32_t a_benchType)
    {
        ArmTune t{};
        if (const auto it = g_armTunes.find(a_model); it != g_armTunes.end()) {
            t = it->second;
        } else if (a_benchType == 1u || a_benchType == 3u || a_benchType == 5u || a_benchType == 7u) {
            t.armDeg = g_config.defArmDeg;
            t.lift = g_config.defArmLift;
        }
        if (t.armDeg > g_config.maxArmDeg) {
            t.lift += (t.armDeg - g_config.maxArmDeg) / 57.29578f * 35.0f;
            t.armDeg = g_config.maxArmDeg;
        } else if (t.armDeg < -g_config.maxArmDeg) {
            t.lift -= (-t.armDeg - g_config.maxArmDeg) / 57.29578f * 35.0f;
            t.armDeg = -g_config.maxArmDeg;
        }
        return t;
    }

    // 给某 actor 按"模型 + benchType"登记手臂 IK；返回登记到的骨骼数
    std::size_t RegisterArmIK(RE::Actor* a_actor, const std::string& a_model, std::uint32_t a_benchType)
    {
        if (!g_config.useArmIK || !a_actor) {
            return 0;
        }
        const auto actorID = a_actor->GetFormID();
        ClearArms(actorID);

        // ① 显式列在 [ArmIK] 里的以它为准（写 0,0 = 这件不做）
        // ② 否则只对站立式工作台套默认值；椅子/床(benchType 0)、磨刀石(2) 一律不动
        const ArmTune t = EffectiveArmTune(a_model, a_benchType);
        if (t.armDeg == 0.0f) {
            return 0;
        }

        // 体型换算：配置值是 0.65 口径；转角 ∝ (1−s)/s，整体抬升 ∝ (1−s)
        // ★ 用 EffectiveScale（含骨架节点缩放），否则"靠 skeleton.nif 缩放变矮"的角色会被当成 1.0 而不动
        const float sc = EffectiveScale(a_actor);
        const float armK = ArmAngleFactor(sc);
        const float rad = (t.armDeg * armK) / 57.29578f;
        if (rad == 0.0f) {
            return 0;
        }

        const auto roots = CollectActorRoots(a_actor);
        std::vector<RE::NiAVObject*> reg;
        for (auto* r : roots) {
            for (const auto [cands, n, tag] : { std::tuple{ kArmLCands, std::size(kArmLCands), "L" },
                                                   std::tuple{ kArmRCands, std::size(kArmRCands), "R" } }) {
                if (auto* bone = FindArmBone(r, cands, n, tag)) {
                    // ★ 骨骼是另一个类，必须单独接管它那个类的 vtable
                    EnsureHooked(bone, tag[0] == 'L' ? "UpperArmL" : "UpperArmR");
                    ArmEntry e{};
                    e.actorID = actorID;
                    e.armRad = rad;
                    g_armEntries[bone] = e;
                    reg.push_back(bone);
                }
            }
        }
        g_actorArms[actorID] = reg;
        return reg.size();
    }

    // 手臂抬升的"残余整体抬升"要叠到根节点上：查这件家具该给多少
    float ArmLiftFor(const std::string& a_model, std::uint32_t a_benchType, float a_scale)
    {
        if (!g_config.useArmIK) {
            return 0.0f;
        }
        const ArmTune t = EffectiveArmTune(a_model, a_benchType);
        if (t.lift == 0.0f) {
            return 0.0f;
        }
        const float k = SizeFactor(a_scale);
        return t.lift * k;
    }

    // ---------------- hook ----------------
    void HookedUpdateWorldData(RE::NiAVObject* a_this, RE::NiUpdateData* a_data)
    {
        // ★ 局部变量与全局同名 → 下面的函数体**完全不用改**，但取的是本节点所属类的原函数
        const UpdateWorldData_t g_origUpdateWorldData = OrigFor(a_this);
        if (!g_origUpdateWorldData) {
            return;
        }
        if (g_sittingOffsets.empty() && g_armEntries.empty()) {
            g_origUpdateWorldData(a_this, a_data);
            return;
        }
        const auto it = g_sittingOffsets.find(a_this);
        if (it == g_sittingOffsets.end()) {
            // ---- 不是根节点 ⇒ 看看是不是要旋转的上臂骨 ----
            const auto ait = g_armEntries.find(a_this);
            if (ait == g_armEntries.end()) {
                g_origUpdateWorldData(a_this, a_data);
                return;
            }
            auto* armActor = RE::TESForm::LookupByID<RE::Actor>(ait->second.actorID);
            auto* parent = a_this->parent;
            if (!armActor || !parent || ait->second.armRad == 0.0f) {
                if (!armActor) {
                    g_armEntries.erase(ait);
                }
                g_origUpdateWorldData(a_this, a_data);
                return;
            }
            // 轴 = 角色右向轴，换到父空间；以**动画原始 local** 为基准左乘 ⇒ 不累积
            const float yaw = armActor->GetAngleZ();
            const RE::NiPoint3 rightWorld{ std::cos(yaw), -std::sin(yaw), 0.0f };
            const RE::NiPoint3 axisP = parent->world.rotate.Transpose() * rightWorld;
            const RE::NiMatrix3 R = MakeRotAxis(axisP, ait->second.armRad);
            const auto savedArm = a_this->local.rotate;
            a_this->local.rotate = R * savedArm;
            g_origUpdateWorldData(a_this, a_data);
            a_this->local.rotate = savedArm;
            return;
        }
        // 每帧按角色**当前** yaw 重算世界向量（只在 yaw 变化时才算 sin/cos）
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->second.actorID);
        if (!actor) {
            // 角色没了（换 cell / 读档残留）⇒ 直接摘掉，别留悬挂目标
            g_sittingOffsets.erase(it);
            g_origUpdateWorldData(a_this, a_data);
            return;
        }
        const float yaw = actor->GetAngleZ();
        if (!it->second.worldValid || yaw != it->second.lastYaw) {
            it->second.world = ToWorld(actor, it->second.local);
            it->second.lastYaw = yaw;
            it->second.worldValid = true;
            // 注：坐下动画会让角色边转身边落座（实测 yaw 在 ~15 帧里从 1.094 连续降到 0.8），
            // 所以"朝向变化"是正常现象，这里不再打日志（早期当异常打过 warn，纯噪声）。
        }
        const auto& off = it->second.world;

        // ★ 诊断：确认 hook **真的**在应用（旧版从未验证过这一步）
        if (++it->second.applies <= 3) {
            logger::info("[APPLY] root={} apply#{} yaw={:.3f}rad local=({:.2f},{:.2f},{:.2f}) world=({:.2f},{:.2f},{:.2f})",
                static_cast<const void*>(a_this), it->second.applies, yaw,
                it->second.local.fwd, it->second.local.lat, it->second.local.vert,
                off.x, off.y, off.z);
        }

        const float sx = a_this->local.translate.x;
        const float sy = a_this->local.translate.y;
        const float sz = a_this->local.translate.z;
        a_this->local.translate.x = sx + off.x;
        a_this->local.translate.y = sy + off.y;
        a_this->local.translate.z = sz + off.z;
        g_origUpdateWorldData(a_this, a_data);
        a_this->local.translate.x = sx;
        a_this->local.translate.y = sy;
        a_this->local.translate.z = sz;
        ++g_applyCount;
    }

    // 把 [Forbid] 的 spec 解析成运行时 FormID。**必须等 kDataLoaded 之后调**：
    void ResolveForbid()
    {
        g_forbid.clear();
        auto* dh = RE::TESDataHandler::GetSingleton();
        for (const auto& spec : g_forbidSpecs) {
            RE::FormID fid = 0;
            const auto colon = spec.find(':');
            try {
                if (colon != std::string::npos && colon > 0) {
                    const std::string mod = spec.substr(0, colon);
                    const std::string local = spec.substr(colon + 1);
                    const auto localID = static_cast<RE::FormID>(std::stoul(local, nullptr, 16));
                    if (dh) {
                        fid = dh->LookupFormID(localID, mod);
                    }
                    if (fid == 0) {
                        logger::warn("[Forbid] resolve failed (plugin not enabled / wrong local ID / ESL index not ready): {}", spec);
                        continue;
                    }
                } else {
                    fid = static_cast<RE::FormID>(std::stoul(spec, nullptr, 16));
                }
            } catch (...) {
                logger::warn("[Forbid] unparsable entry: {}", spec);
                continue;
            }
            g_forbid.insert(fid);
            logger::info("[Forbid] excluded from fixing -> 0x{:08X}  ({})", fid, spec);
        }
        logger::info("[Forbid] {} entries active", g_forbid.size());
    }

    // ---------------- 配置读写 ----------------
    void SaveConfig()
    {
        std::ofstream file("Data/SKSE/Plugins/FurnitureUserSizeFix.ini");
        if (!file.is_open()) {
            logger::error("Cannot write FurnitureUserSizeFix.ini");
            return;
        }
        file << "[General]\n";
        file << "; Default formula: lift = (1-scale) * fSeatHeight * fGlobalScale\n";
        file << "; fGlobalScale=0 disables the built-in formula; only the manual values below are used\n";
        file << std::format("fSeatHeight={:.3f}\n", g_config.seatHeight);
        file << std::format("fGlobalScale={:.3f}\n", g_config.globalScale);
        file << "; Size compensation: lift = calibrated * (1-scale)/(1-fCalibrationScale); zero at scale 1.0\n";
        file << "; fCalibrationScale = the scale your values were calibrated at (0.65 for this profile)\n";
        file << std::format("bScaleCompensate={}\n", g_config.scaleCompensate ? 1 : 0);
        file << "; Leave characters at scale >= 1.0 untouched (1 = untouched, recommended). 0 = keep extrapolating downwards.\n";
        file << std::format("bClampAboveOne={}\n", g_config.clampAboveOne ? 1 : 0);
        file << "; Also apply the correction to characters at scale > 1.0 (0 = leave them untouched, default;\n";
        file << "; 1 = apply the linear extrapolation, which pushes them downwards).\n";
        file << "; !! This path has never been verified in game; results may not match expectations !!\n";
        file << std::format("bAllowAboveOne={}\n", g_config.allowAboveOne ? 1 : 0);
        file << std::format("fCalibrationScale={:.3f}\n", g_config.calibrationScale);
        file << "; Probe: log skeleton node heights relative to the root when seated (logging only, no behaviour change).\n";
        file << "; Used to test whether h can be measured at runtime. Set 0 when not collecting data.\n";
        file << std::format("bProbeBones={}\n", g_config.probeBones ? 1 : 0);
        file << "; ---- Upper-body IK ----\n";
        file << "; Rotates the upper arms to raise the hands; the root stays put, so the feet stay grounded. bUseArmIK=0 restores pure whole-body lifting.\n";
        file << std::format("bUseArmIK={}\n", g_config.useArmIK ? 1 : 0);
        file << std::format("fMaxArmDeg={:.2f}\n", g_config.maxArmDeg);
        file << std::format("fDefaultArmDeg={:.2f}\n", g_config.defArmDeg);
        file << std::format("fDefaultArmLift={:.2f}\n", g_config.defArmLift);

        file << "\n[ArmIK]\n";
        file << "; model = arm raise angle (deg), residual body lift (units); 0,0 disables this piece\n";
        file << "; Only benchType 1/3/5/7 (standing stations) use the [General] defaults; others must be listed here\n";
        {
            std::vector<std::pair<std::string, ArmTune>> armRows(g_armTunes.begin(), g_armTunes.end());
            std::sort(armRows.begin(), armRows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [m, at] : armRows) {
                file << std::format("{}={:.2f},{:.2f}\n", m, at.armDeg, at.lift);
            }
        }

        file << "\n[Forbid]\n";
        file << "; Furniture to leave alone, one per line. Two forms are accepted:\n";
        file << ";   SomePlugin.esm:045A93  <- plugin name : local FormID (load-order safe, recommended)\n";
        file << ";   0012ABCD              <- runtime FormID\n";
        for (const auto& spec : g_forbidSpecs) {
            file << spec << "\n";
        }

        file << "\n[Defaults]\n";
        file << "; benchType = forward, lateral, vertical\n";
        file << "; benchType comes from TESFurniture WBDT: 0=seat 1=anvil/forge/tanning 2=grindstone 3=enchanting 5=alchemy 7=armor bench\n";
        file << "; Order: benchType!=0 value > [AnimType] tier > benchType==0 value > built-in formula\n";
        file << "; Only fill in types that were actually measured; leave the rest empty\n";
        for (const auto& [type, off] : g_benchDefaults) {
            file << std::format("{}={:.2f},{:.2f},{:.2f}\n", type, off.fwd, off.lat, off.vert);
        }

        file << "\n[AnimType]\n";
        file << "; animationType = forward, lateral, vertical\n";
        file << "; animationType comes from the BSFurnitureMarker bitmask inside the model NIF:\n";
        file << ";   1=kSit (chairs/benches/thrones and every station)  2=kSleep (beds)  4=kLean\n";
        file << "; Why this tier exists: [Defaults] benchType is 0 for both beds and chairs, so they collide;\n";
        file << ";   yet lying and sitting need very different lifts. animationType is the only runtime signal that separates them.\n";
        file << "; Position: after the station defaults, before the benchType==0 seat value.\n";
        for (const auto& [type, off] : g_animOverrides) {
            file << std::format("{}={:.2f},{:.2f},{:.2f}\n", type, off.fwd, off.lat, off.vert);
        }

        file << "\n[Markers]\n";
        file << "; model path@marker flags = forward, lateral, vertical   (most specific: shared per model+marker, recommended)\n";
        file << "; marker flags come from the runtime furnFlags; different markers need separate keys\n";
        for (const auto& [key, off] : g_markerOverrides) {
            file << std::format("{}={:.2f},{:.2f},{:.2f}\n", key, off.fwd, off.lat, off.vert);
        }

        file << "\n[Models]\n";
        file << "; model path = forward, lateral, vertical   (fallback without a marker; lower priority than [Markers])\n";
        for (const auto& [key, off] : g_modelOverrides) {
            file << std::format("{}={:.2f},{:.2f},{:.2f}\n", key, off.fwd, off.lat, off.vert);
        }

        file << "\n[Furniture]\n";
        file << "; furniture base FormID = forward, lateral, vertical   (single reference only, lowest priority)\n";
        for (const auto& [fid, off] : g_overrides) {
            file << std::format("{:08X}={:.2f},{:.2f},{:.2f}\n", fid, off.fwd, off.lat, off.vert);
        }
        logger::info("Config saved: marker {} / model {} / per-reference {} / benchType default {} / animation {}",
            g_markerOverrides.size(), g_modelOverrides.size(), g_overrides.size(),
            g_benchDefaults.size(), g_animOverrides.size());
    }

    void LoadConfig()
    {
        std::ifstream file("Data/SKSE/Plugins/FurnitureUserSizeFix.ini");
        if (!file.is_open()) {
            logger::warn("FurnitureUserSizeFix.ini not found; using defaults (built-in formula disabled)");
            return;
        }
        std::string line;
        std::string section = "General";
        auto trim = [](std::string& s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            const auto e = s.find_last_not_of(" \t\r\n");
            s = (b == std::string::npos) ? std::string{} : s.substr(b, e - b + 1);
        };
        while (std::getline(file, line)) {
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            trim(line);
            // 只把【行首】的 ; 或 # 当注释。
            // 不能在行内截断：模型路径里可能出现 # 或 ;，截了就把 key 弄坏了。
            if (line.empty() || line.front() == ';' || line.front() == '#') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                // [Forbid] 允许"裸行"写法：一行一个 插件名:本地ID / 运行时FormID
                if (section == "Forbid") {
                    g_forbidSpecs.push_back(line);
                }
                continue;
            }
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key);
            trim(val);
            auto parseOffset = [&val](Offset& a_out) {
                float vals[3]{};
                int n = 0;
                std::size_t pos = 0;
                while (n < 3 && pos <= val.size()) {
                    const auto comma = val.find(',', pos);
                    const auto piece = val.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    try {
                        vals[n++] = std::stof(piece);
                    } catch (...) {
                        return false;
                    }
                    if (comma == std::string::npos) {
                        break;
                    }
                    pos = comma + 1;
                }
                if (n != 3) {
                    return false;
                }
                a_out.fwd = vals[0];
                a_out.lat = vals[1];
                a_out.vert = vals[2];
                return true;
            };
            try {
                if (section == "General") {
                    if (key == "fSeatHeight") {
                        g_config.seatHeight = std::stof(val);
                    } else if (key == "fGlobalScale") {
                        g_config.globalScale = std::stof(val);
                    } else if (key == "bScaleCompensate") {
                        g_config.scaleCompensate = (std::stoi(val) != 0);
                    } else if (key == "fCalibrationScale") {
                        g_config.calibrationScale = std::stof(val);
                    } else if (key == "bAllowAboveOne") {
                    g_config.allowAboveOne = (std::stoi(val) != 0);
                } else if (key == "bClampAboveOne") {
                    g_config.clampAboveOne = (std::stoi(val) != 0);
                } else if (key == "bUseArmIK") {
                    g_config.useArmIK = (std::stoi(val) != 0);
                } else if (key == "fMaxArmDeg") {
                    g_config.maxArmDeg = std::clamp(std::stof(val), 0.0f, 90.0f);
                } else if (key == "fDefaultArmDeg") {
                    g_config.defArmDeg = std::clamp(std::stof(val), -90.0f, 90.0f);
                } else if (key == "fDefaultArmLift") {
                    g_config.defArmLift = std::clamp(std::stof(val), -20.0f, 60.0f);
                } else if (key == "bProbeBones") {
                        g_config.probeBones = (std::stoi(val) != 0);
                    }
                } else if (section == "Forbid") {
                    // key=任意值 的写法也接受，取 key 当 spec
                    g_forbidSpecs.push_back(key);
                } else if (section == "Defaults") {
                    // key 是 benchType 的十进制值
                    const auto type = static_cast<std::uint32_t>(std::stoul(key, nullptr, 10));
                    Offset off{};
                    if (parseOffset(off)) {
                        g_benchDefaults[type] = off;
                    } else {
                        logger::warn("Failed to parse benchType default line: {}", key);
                    }
                } else if (section == "AnimType") {
                    // key 是 animationType 的十进制值（位掩码语义：1/2/4）
                    const auto type = static_cast<std::uint32_t>(std::stoul(key, nullptr, 10));
                    if ((type & 1) != 0) {
                        // v0.10.1：kSit 档已废弃 —— 一个共享值会串到铁砧/锻炉/附魔台上
                        logger::warn("[AnimType] {} contains the kSit bit; ignored (kSit furniture is managed by [Defaults] or per-reference values;"
                                     "use \"Save to benchType tier\" or per-reference saving for those)",
                            type);
                        continue;
                    }
                    Offset off{};
                    if (parseOffset(off)) {
                        g_animOverrides[type] = off;
                    } else {
                        logger::warn("Failed to parse animationType default line: {}", key);
                    }
                } else if (section == "Markers") {
                    Offset off{};
                    if (parseOffset(off)) {
                        g_markerOverrides[key] = off;
                    } else {
                        logger::warn("Failed to parse marker line: {}", key);
                    }
                } else if (section == "Models") {
                    std::string mk = key;
                    for (auto& c : mk) {
                        if (c == '/') {
                            c = '\\';
                        } else {
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }
                    }
                    Offset off{};
                    if (parseOffset(off)) {
                        g_modelOverrides[mk] = off;
                    } else {
                        logger::warn("Failed to parse model line: {}", key);
                    }
                } else if (section == "ArmIK") {
                    // 模型 = 手臂角度(度), 残余整体抬升(单位)
                    ArmTune t{};
                    const auto comma = val.find(',');
                    if (comma == std::string::npos) {
                        t.armDeg = std::stof(val);
                    } else {
                        t.armDeg = std::stof(val.substr(0, comma));
                        t.lift = std::stof(val.substr(comma + 1));
                    }
                    t.armDeg = std::clamp(t.armDeg, -90.0f, 90.0f);
                    t.lift = std::clamp(t.lift, -20.0f, 60.0f);
                    g_armTunes[key] = t;
                } else if (section == "Furniture") {
                    const auto fid = static_cast<RE::FormID>(std::stoul(key, nullptr, 16));
                    Offset off{};
                    if (parseOffset(off)) {
                        g_overrides[fid] = off;
                    } else {
                        logger::warn("Failed to parse furniture line: {}", key);
                    }
                }
            } catch (...) {
                logger::warn("Failed to parse config entry: {}={}", key, val);
            }
        }
        logger::info("Config loaded: fSeatHeight={:.2f} fGlobalScale={:.3f} marker={} model={} perRef={} benchType={} anim={} armIK={} armEntries={}",
            g_config.seatHeight, g_config.globalScale,
            g_markerOverrides.size(), g_modelOverrides.size(), g_overrides.size(),
            g_benchDefaults.size(), g_animOverrides.size(),
            g_config.useArmIK, g_armTunes.size());
        LoadLang();   // 菜单文案：语言包 > 主 INI [Menu] > 内置英文
        for (const auto& [type, off] : g_benchDefaults) {
            logger::info("  [Defaults] benchType={} ({}) → F{:.2f} L{:.2f} V{:.2f}",
                type, BenchTypeName(type), off.fwd, off.lat, off.vert);
        }
        for (const auto& [type, off] : g_animOverrides) {
            logger::info("  [AnimType] animationType={} ({}) → F{:.2f} L{:.2f} V{:.2f}",
                type, AnimTypeName(type), off.fwd, off.lat, off.vert);
        }

        // ---- 旧格式 [Markers] 键体检 ----
        // 运行时的 TESFurniture::furnFlags 一定带 kCanSit(1<<30) 或 kCanLean(1<<29)
        // 这类高位（日志实证：椅子 0x40000001、工作台 0x60000001、铁砧 0x68000001）。
        // 所以 flag 部分**不含**这些高位的键，永远不可能与运行时 key 相等 —— 它们是
        // v0.9/v0.10 早期按"离线读 NAM0 当 u32"生成的坏键（NAM0 其实是
        // EntryPointData.disabledPoints，只有低 16 位有效）。
        // 这里只做告警，不擅自改用户数据。
        std::size_t legacyKeys = 0;
        for (const auto& [k, off] : g_markerOverrides) {
            const auto at = k.rfind('@');
            if (at == std::string::npos || at + 1 >= k.size()) {
                continue;
            }
            std::uint32_t flag = 0;
            try {
                flag = static_cast<std::uint32_t>(std::stoul(k.substr(at + 1), nullptr, 16));
            } catch (...) {
                continue;
            }
            if ((flag & 0xE0000000u) == 0) {
                ++legacyKeys;
                logger::warn("  [Markers] legacy-format key (never matches): {}", k);
            }
        }
        if (legacyKeys > 0) {
            logger::warn("*** [Markers] {} keys lack the kCanSit/kCanLean high bits => those {} keys"
                         "will never match. They were generated by an early build that read NAM0 as a u32;"
                         "please re-save them with \"Save to model+marker\" or \"Save to this reference only\". ***",
                legacyKeys, legacyKeys);
        }
    }

    // ---------------- 计算某 actor 坐在某家具上的偏移 ----------------
    // 统一的取值链，菜单与运行时共用，避免两处优先级写岔：
    //   1. [Markers]   模型@坐点            —— 用户对这件家具亲手量过的值
    //   2. [Models]    纯模型               —— 用户对同模型量过、但没区分坐点
    //   3. [Furniture] 单件 FormID          —— 只作用于一件
    //   4. [Defaults]  benchType != 0       —— 工作台专用（磨刀石/护甲台…）
    //   5. [AnimType]  动画档（★ 只对 kSleep/kLean，kSit 一律不走这一档）
    //   6. [Defaults]  benchType == 0       —— 通用坐具兜底（仅 kSit，见下方说明）
    //   7. built-in formula（默认关闭 ⇒ not fixed）
    //
    // 为什么第 4 步要排在 [AnimType] 前面：实测 400 条 FURN 里 animationType 只有
    // kSit(159) 与 kSleep(120) 两种取值，**磨刀石和护甲台也是 kSit**。若让动画档
    // 压过工作台专用值，磨刀石会被套上椅子的抬升量。反过来，第 5 步排在
    // benchType==0 之前也是必须的：床和椅子的 benchType 都是 0，只有动画档能把
    // "躺"和"坐"分开。
    // ★ v0.10.1 追加：动画档**只管 kSleep/kLean**。理由见第 5 步的注释（用户实测
    //   锻炉吃到了椅子值）。kSit 的家具一律由 [Defaults] benchType 与逐件/逐模型值管理。
    // ★ v0.11.2 把 [Furniture] 提到第 1 位：用户反馈"给单件家具保存了特定值，按下保存以后
    //   菜单里也看不见单件值，还是只显示坐点通用值"。根因就是它的优先级原来排在 [Markers]
    //   之后 ⇒ 单件值永远被共享键遮住，保存完 applyToCurrent() 立刻又算回共享值、滑块弹回。
    //   「只保存到这件」的语义本来就是"最具体"，必须赢。
    bool ResolveOverride(RE::TESObjectREFR* a_furn, Offset& a_out, OffsetSource& a_outSrc)
    {
        a_outSrc = OffsetSource::kNone;
        // 家具引用取不到时一律不命中。否则下面 benchType 会读到 0，
        // 误把"普通坐具"的默认值套到一个不明对象上。
        if (!a_furn) {
            return false;
        }
        const auto* base = a_furn->GetBaseObject();
        const auto baseID = base ? base->GetFormID() : 0;
        if (baseID != 0 && g_forbid.count(baseID) != 0) {
            // [Forbid] 名单命中：永远not fixed（SexLab 定位家具等）
            a_outSrc = OffsetSource::kNone;
            return false;
        }
        // 1) 单件 FormID（最具体，压过所有共享键）
        if (baseID != 0) {
            if (const auto it = g_overrides.find(baseID); it != g_overrides.end()) {
                a_out = it->second;
                a_outSrc = OffsetSource::kFormID;
                return true;
            }
        }
        // 2) 模型@坐点（共享）
        const auto markerKey = GetMarkerKey(a_furn);
        if (!markerKey.empty()) {
            if (const auto it = g_markerOverrides.find(markerKey); it != g_markerOverrides.end()) {
                a_out = it->second;
                a_outSrc = OffsetSource::kMarker;
                return true;
            }
        }
        // 3) 纯模型
        const auto modelKey = GetModelKey(a_furn);
        if (!modelKey.empty()) {
            if (const auto it = g_modelOverrides.find(modelKey); it != g_modelOverrides.end()) {
                a_out = it->second;
                a_outSrc = OffsetSource::kModel;
                return true;
            }
        }

        // 4) 工作台专用默认值（benchType != 0，即磨刀石/附魔/炼金/护甲台…）
        const auto benchType = GetBenchType(base);
        const auto itBench = g_benchDefaults.find(benchType);
        const bool hasBenchDefault = (itBench != g_benchDefaults.end());
        if (benchType != 0 && hasBenchDefault) {
            a_out = itBench->second;
            a_outSrc = OffsetSource::kBenchDefault;
            return true;
        }

        // 5) 动画档。**只对不含 kSit 的家具生效**（即床 kSleep / 靠 kLean）。
        //    kSit 覆盖面太广（椅子/长凳/王座 + 铁砧/锻炉/熔炉/制革架/附魔台/炼金台），
        //    一个共享值会串到这些东西上 —— 2026-10-31 用户实测："给椅子存的值，使用锻炉
        //    的时候也被应用了"（日志：anvil benchType=1 来源=[AnimType] 偏移=…6.29）。
        //    kSit 一律交给 [Defaults] benchType 与逐件/逐模型值，动画档不插手。
        const auto anim = GetAnimInfo(a_furn);
        if (anim.valid && (anim.mask & 1) == 0) {
            for (const std::uint32_t bit : { 2u, 4u }) {
                if ((anim.mask & bit) == 0) {
                    continue;
                }
                if (const auto it = g_animOverrides.find(bit); it != g_animOverrides.end()) {
                    a_out = it->second;
                    a_outSrc = OffsetSource::kAnimType;
                    return true;
                }
            }
        }

        // 6) benchType == 0 的通用坐具默认值（最后一档有数可依的兜底）
        if (hasBenchDefault) {
            // ★ 只对"坐"动画放行：这一档的数字是**椅子**量出来的中位数。
            //   床(kSleep) 与靠(kLean) 的家具 benchType 同样是 0，若也吃这份"坐"值，
            //   等于拿坐姿中位数去套躺姿 —— 没有任何依据（v0.9.0 就存在这个隐患，
            //   被怀疑是"床会浮空"的来源）。
            //   动画类型读得到、且不含 kSit 位 ⇒ not fixed，等用户在 [AnimType] 里按档补值。
            //   读不到动画类型时保持旧行为（放行），避免"读不到"变成"连原来有的修复也没了"。
            if (anim.valid && anim.mask != 0 && (anim.mask & 1) == 0) {
                a_outSrc = OffsetSource::kNone;
                return false;
            }
            a_out = itBench->second;
            a_outSrc = OffsetSource::kBenchDefault;
            return true;
        }

        a_outSrc = OffsetSource::kNone;
        return false;
    }

    // ★ 体型补偿系数 k：把配置里的数换算到当前 actor。
    //   两种口径（用 fCalibrationScale 区分，无需额外的开关）：
    //   ① fCalibrationScale = 0      ⇒ **配置里填的数就是 h 本身**（发布用默认值走这条），
    //                                  lift = (1−s) × h
    //   ② fCalibrationScale = 0.65   ⇒ 配置里的数是"在 0.65 体型下标定出来的抬升量"，
    //                                  lift = 值 × (1−s)/(1−0.65)（等价于 h = 值/0.35）
    //   依据：FHSF 的公式骨架就是 (1−s)×Adjust；用户实测磨刀石 10.63 对 FHSF 的 0.35×30=10.50。
    //   ★ 允许负值：s > 1（高大角色）时 (1−s) < 0，会把角色往下压 —— 同样是正确行为，
    //     以前 clamp 到 0 等于"比 1.0 高的人完全not fixed"，对发布版是缺陷。
    // 体型 ≠ 1 才补偿；**体型 ≥ 1 直接不生效**（可配置关闭）。
    // 理由：(0.65→1.00) 这段有锚点（必须落在 0），但 s>1 是纯线性外推、从未实测；
    //  s=1.5 时三轴系数会到 −1.43（进深反向 36 单位），很可能把人推到家具外面去。
    //  用户 2026-11-01 决定：超过 1.0 的角色一律不动。
    bool ScaleAboveOneExempt(float a_scale)
    {
        return g_config.clampAboveOne && !g_config.allowAboveOne && a_scale >= 1.0f;
    }

    float SizeFactor(float a_scale)
    {
        if (!g_config.scaleCompensate) {
            return 1.0f;
        }
        if (a_scale <= 0.05f) {
            return 1.0f;
        }
        if (ScaleAboveOneExempt(a_scale)) {
            return 0.0f;
        }
        const float cs = g_config.calibrationScale;
        if (cs <= 0.005f) {
            return std::clamp(1.0f - a_scale, -2.0f, 4.0f);
        }
        const float denom = 1.0f - cs;
        if (denom < 0.01f) {
            return 1.0f;
        }
        return std::clamp((1.0f - a_scale) / denom, -2.0f, 4.0f);
    }

    // 手臂转角系数：转角 ∝ (1−s)/s（弧长 ∝ (1−s)、臂长 ∝ s）
    float ArmAngleFactor(float a_scale)
    {
        if (!g_config.scaleCompensate || a_scale <= 0.05f) {
            return 1.0f;
        }
        if (ScaleAboveOneExempt(a_scale)) {
            return 0.0f;
        }
        constexpr float kRef = 0.65f;
        return std::clamp(((1.0f - a_scale) / a_scale) / ((1.0f - kRef) / kRef), -2.0f, 4.0f);
    }

    RE::NiPoint3 ComputeOffset(RE::Actor* a_actor, RE::TESObjectREFR* a_furn, float& a_outScale, bool& a_outManual,
        OffsetSource* a_outSrc = nullptr, float* a_outK = nullptr, Offset* a_outLocal = nullptr)
    {
        a_outScale = a_actor ? EffectiveScale(a_actor) : 1.0f;
        a_outManual = false;
        if (a_outSrc) {
            *a_outSrc = OffsetSource::kNone;
        }
        if (a_outLocal) {
            *a_outLocal = Offset{};
        }
        if (a_outK) {
            // 先给"这个体型本该乘多少"，not fixed时也能从日志看出它为什么是 0
            *a_outK = a_actor ? SizeFactor(a_outScale) : 1.0f;
        }
        if (!a_actor) {
            return {};
        }

        Offset off{};
        OffsetSource src = OffsetSource::kNone;
        if (ResolveOverride(a_furn, off, src)) {
            // 只有用户为某件/某型家具显式保存过的才算"手动"；
            // [Defaults] / [AnimType] 这两档是分类默认值，不算手动
            a_outManual = (src == OffsetSource::kMarker || src == OffsetSource::kModel || src == OffsetSource::kFormID);
            if (a_outSrc) {
                *a_outSrc = src;
            }
            const float k = SizeFactor(a_outScale);
            if (a_outK) {
                *a_outK = k;
            }
            off.fwd *= k;
            off.lat *= k;
            off.vert *= k;
            if (a_outLocal) {
                *a_outLocal = off;   // ★ 返回【局部】三轴，世界向量由 hook 每帧按当前朝向算
            }
            return ToWorld(a_actor, off);
        }

        // 没有手动值 → 走默认公式（默认 globalScale=0 时等于not fixed）
        if (a_outScale >= 0.95f && a_outScale <= 1.05f) {
            return {};
        }
        off = {};
        off.vert = (1.0f - a_outScale) * g_config.seatHeight * g_config.globalScale;
        if (a_outSrc) {
            *a_outSrc = OffsetSource::kFormula;
        }
        if (off.vert == 0.0f) {
            if (a_outSrc) {
                *a_outSrc = OffsetSource::kNone;
            }
            return {};
        }
        if (a_outLocal) {
            *a_outLocal = off;
        }
        return ToWorld(a_actor, off);
    }

    // ---------------- 探针：骨架各节点相对根节点的高度（只记录，不改行为） ----------------
    // 关注的是 `节点.world.z − 根.world.z`：我们的抬升是把根的 local 平移一下，
    // 根和所有子节点的 world 会**同步平移同样的量** ⇒ 这个差值对我们的抬升不敏感，
    // 量到的就是"身体某部位在 actor 根节点上方多高"，正是候选的 h。
    struct ProbeHit
    {
        std::string name;
        float       dz;
    };

    // 一堆一眼就知道没用的节点：CME/Weapon 是 RaceMenu 的重复镜像骨、
    // hdt* 是物理代理、Ovl*/PreButtRoot 是 HDT 的碰撞辅助点、Hands [Ovl*] 不是真手骨。
    // v0.11.0 没过滤它们，结果 24 条上限被这些占满，**真正的手骨/脚骨一条都没采到**。
    bool IsProbeJunk(std::string_view a_nm)
    {
        if (a_nm.starts_with("CME ") || a_nm.starts_with("Weapon ") || a_nm.starts_with("hdt")) {
            return true;
        }
        if (a_nm.find("Ovl") != std::string_view::npos ||
            a_nm.find("PreButtRoot") != std::string_view::npos ||
            a_nm.find("AutoRename") != std::string_view::npos) {
            return true;
        }
        return false;
    }

    void ProbeBones(RE::NiAVObject* a_node, int a_depth, float a_rootWorldZ, std::vector<ProbeHit>& a_out)
    {
        // v0.11.1 把深度限在 10 —— 而手骨在骨架里的深度是 11～12
        // （root→NPC→NPC Root→COM→Pelvis→Spn0→Spn1→Spn2→Clavicle→UpperArm→Forearm→Hand），
        // 所以**手骨/脚骨一条都没走不到**。这里放到 18。
        if (!a_node || a_depth > 18 || a_out.size() >= 64) {
            return;
        }
        const char* raw = a_node->name.c_str();
        if (raw && *raw) {
            const std::string_view nm{ raw };
            if (!IsProbeJunk(nm)) {
                // 用带空格的 "Hand [" / "Foot [" 精确避开 "Hands [Ovl0]" 这类物理辅助节点
                static constexpr std::string_view kKeys[] = {
                    "Pelv", "Thigh", "Spine", "Hand [", "Foot [", "Head", "Neck", "Clavicle", "Root ["
                };
                for (const auto k : kKeys) {
                    if (nm.find(k) != std::string_view::npos) {
                        a_out.push_back({ std::string{ nm }, a_node->world.translate.z - a_rootWorldZ });
                        break;
                    }
                }
            }
        }
        if (a_out.size() >= 64) {
            return;
        }
        if (auto* asNode = a_node->AsNode()) {
            for (auto& child : asNode->GetChildren()) {
                ProbeBones(child.get(), a_depth + 1, a_rootWorldZ, a_out);
            }
        }
    }

    // 精确按名字取（不依赖遍历深度）—— 手/脚这种深节点用它兜底
    void ProbeExact(RE::NiAVObject* a_root, std::string_view a_name, float a_rootWorldZ, std::string& a_line)
    {
        if (!a_root) {
            return;
        }
        auto* n = a_root->GetObjectByName(RE::BSFixedString(a_name.data()));
        if (n) {
            a_line += std::format("{}={:.2f} ", a_name, n->world.translate.z - a_rootWorldZ);
        } else {
            a_line += std::format("{}=", a_name);
            a_line += "missing ";
        }
    }

    // 地址是否落在 SkyrimSE.exe 的 .text 段内（只用于日志区分，不再作为安装前提）
    bool IsAddressInTextSegment(const void* a_addr)
    {
        const auto& mod = REL::Module::get();
        const auto seg = mod.segment(REL::Segment::textx);
        const auto segBase = seg.address();
        const auto segSize = static_cast<std::uintptr_t>(seg.size());
        const auto addr = reinterpret_cast<std::uintptr_t>(a_addr);
        return addr >= segBase && addr < (segBase + segSize);
    }

    // 地址是否可执行（**不要求**在 SkyrimSE.exe 的 .text 里）
    //
    // 【为什么放宽】2026-10-31 实测：`FurnitureHandIK` 探针加载时，本插件已经把这个 vtable 条目
    // 换成了自己的 hook，于是探针看到"指针不在 .text"就拒绝安装，一条数据都采不到。
    // 反过来也一样：若探针先加载，本插件会拒绝安装 ⇒ **主功能静默失效**，这是很危险的隐患。
    // ⇒ 判据改为"是不是可执行内存"（VirtualQuery 看 protect 的可执行位），
    //   这样两个插件可以安全地**互相链式 hook**，顺序无关。
    bool IsExecutableAddress(const void* a_addr)
    {
        if (!a_addr) {
            return false;
        }
        // ⚠️ SKSEMenuFramework.h 带进 <windows.h>，它把 MEM_COMMIT / PAGE_EXECUTE* 定义成**宏**，
        //    会把 `REX::W32::PAGE_EXECUTE` 展开成 `REX::W32::0x10` 而报 ":: 右边非法标记"。
        //    所以这里一律用字面量（与下面 VirtualProtect 的 kPageExecuteReadWrite 同一个坑）。
        constexpr std::uint32_t kMemCommit = 0x1000;
        constexpr std::uint32_t kExecMask =
            0x10u /*PAGE_EXECUTE*/ | 0x20u /*PAGE_EXECUTE_READ*/ | 0x40u /*PAGE_EXECUTE_READWRITE*/ | 0x80u /*PAGE_EXECUTE_WRITECOPY*/;
        REX::W32::MEMORY_BASIC_INFORMATION mbi{};
        if (REX::W32::VirtualQuery(a_addr, &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (static_cast<std::uint32_t>(mbi.state) != kMemCommit) {
            return false;
        }
        return (static_cast<std::uint32_t>(mbi.protect) & kExecMask) != 0;
    }

    // 接管某个节点所属**类**的 vtable 条目；可重复调用（同槽位只装一次）
    bool EnsureHooked(RE::NiAVObject* a_node, const char* a_tag)
    {
        if (!a_node) {
            return false;
        }
        auto** vtbl = *reinterpret_cast<void***>(a_node);
        if (!vtbl) {
            return false;
        }
        void** slot = &vtbl[UwdIndex()];
        if (*slot == reinterpret_cast<void*>(&HookedUpdateWorldData)) {
            return true;
        }
        for (std::size_t i = 0; i < g_slotCount; ++i) {
            if (g_slots[i].slot == slot) {
                return true;
            }
        }
        if (g_slotCount >= std::size(g_slots)) {
            logger::error("hook slot table full ({} classes); skipping [{}]", g_slotCount, a_tag);
            return false;
        }
        void* origAddr = *slot;
        if (!IsExecutableAddress(origAddr)) {
            logger::error("[{}] vtable[0x{:X}]=0x{:X} is not an executable address; skipping", a_tag, UwdIndex(),
                reinterpret_cast<std::uintptr_t>(origAddr));
            return false;
        }
        // 注意：SKSEMenuFramework.h 引入 <windows.h>，把 PAGE_EXECUTE_READWRITE 定义成宏 0x40，
        // 写 REX::W32::PAGE_EXECUTE_READWRITE 会展开成 REX::W32::0x40 而报错 ⇒ 用字面量。
        constexpr std::uint32_t kPageExecuteReadWrite = 0x40;
        std::uint32_t old = 0;
        if (!REX::W32::VirtualProtect(slot, sizeof(void*), kPageExecuteReadWrite, &old)) {
            return false;
        }
        g_slots[g_slotCount].slot = slot;
        g_slots[g_slotCount].orig = reinterpret_cast<UpdateWorldData_t>(origAddr);
        ++g_slotCount;
        g_cacheSlot = nullptr;   // 缓存失效
        *slot = reinterpret_cast<void*>(&HookedUpdateWorldData);
        REX::W32::VirtualProtect(slot, sizeof(void*), old, &old);
        if (!g_origUpdateWorldData) {
            g_origUpdateWorldData = reinterpret_cast<UpdateWorldData_t>(origAddr);
        }
        logger::info("*** hook installed [{}] vtable[0x{:X}]=0x{:X}{} total {} classes ***",
            a_tag, UwdIndex(), reinterpret_cast<std::uintptr_t>(origAddr),
            IsAddressInTextSegment(origAddr) ? "(engine original)" : "(chained on top of another plugin hook)", g_slotCount);
        return true;
    }

    // 只保证"玩家 3D 根节点那个类"已接管（开机即可用）
    bool TryInstallHook()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* root = player ? player->Get3D() : nullptr;
        return root ? EnsureHooked(root, "root") : false;
    }

    // ---------------- 菜单 ----------------
    // 上肢 IK 的菜单状态（与三轴偏移那套分开，互不干扰）
    std::atomic<RE::FormID> g_armFurnID{ 0 };
    std::string             g_armModel;
    std::uint32_t           g_armBenchType = 0;
    ArmTune                 g_armEdit{};

#if defined(FUSF_WITH_MENU)

    void __stdcall RenderArmMenu()
    {
        bool en = g_config.useArmIK;
        if (ImGuiMCP::Checkbox(L("menu.armik.enable", "Enable arm IK"), &en)) {
            g_config.useArmIK = en;
            if (!en) {
                g_armEntries.clear();
                g_actorArms.clear();
            } else if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                RegisterArmIK(pl, g_armModel, g_armBenchType);
            }
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.armik.saveToggle", "Save toggle"))) {
            SaveConfig();
        }
        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.armik.help", "Rotates the upper arms to raise the hands; the root stays put, so the feet stay on the ground."));
        ImGuiMCP::Text(L("menu.armik.capHint", "Angle cap %.0f deg (excess converts to a body lift). Negative reverses direction."), g_config.maxArmDeg);
        ImGuiMCP::Separator();

        if (g_armFurnID.load() == 0) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.8f, 0.3f, 1.0f), L("menu.armik.noFurniture", "Not using furniture right now"));
            ImGuiMCP::Text(L("menu.armik.useFirst", "Use a workstation once, then come back here."));
            ImGuiMCP::Separator();
            ImGuiMCP::SliderFloat(L("menu.armik.defAngle", "Default angle (unlisted standing stations)"), &g_config.defArmDeg, -g_config.maxArmDeg, g_config.maxArmDeg, "%.1f");
            ImGuiMCP::SliderFloat(L("menu.armik.defLift", "Default residual lift"), &g_config.defArmLift, -20.0f, 60.0f, "%.2f");
            if (ImGuiMCP::Button(L("menu.armik.saveDefaults", "Save defaults"))) {
                SaveConfig();
            }

            ImGuiMCP::Separator();
            bool allowAbove = g_config.allowAboveOne;
            if (ImGuiMCP::Checkbox(
                    L("menu.armik.allowAboveOne", "Also apply to characters at scale 1.0 or above"), &allowAbove)) {
                g_config.allowAboveOne = allowAbove;
                SaveConfig();
            }
            if (g_config.allowAboveOne) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                    L("menu.armik.allowAboveOneWarn",
                      "UNTESTED: this path has never been verified in game; results for these characters may differ from expectations."));
            } else {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                    L("menu.armik.allowAboveOneOff",
                      "Characters at scale 1.0 or above are currently left untouched."));
            }
            return;
        }

        ImGuiMCP::Text(L("menu.common.model", "Model: %s"), g_armModel.empty() ? L("menu.common.empty", "<none>") : g_armModel.c_str());
        {
            const bool eligible = (g_armBenchType == 1u || g_armBenchType == 3u || g_armBenchType == 5u || g_armBenchType == 7u);
            const bool listed = !g_armModel.empty() && g_armTunes.find(g_armModel) != g_armTunes.end();
            if (!eligible && !listed) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                    L("menu.armik.notEligible",
                      "This furniture does NOT use arm IK by default (seats/beds). Dragging a slider adds it explicitly."));
            } else if (!listed) {
                // ★ 站立式家具但 [ArmIK] 里没有它的条目：滑块显示的是 [General] 默认值，
                //   不是"你保存过的值"。不标出来会让人以为自己的调参被回滚了。
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                    L("menu.armik.usingDefaults",
                      "Showing DEFAULTS from [General] - this model has no saved entry yet. Press Save to keep these values."));
            } else {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    L("menu.armik.savedEntry", "Showing the saved values for this model."));
            }
        }
        ImGuiMCP::Text("FormID: %08X  benchType: %u", g_armFurnID.load(), g_armBenchType);
        ImGuiMCP::Separator();
        bool changed = false;
        changed |= ImGuiMCP::SliderFloat(L("menu.armik.angle", "Arm raise angle (deg)"), &g_armEdit.armDeg, -g_config.maxArmDeg, g_config.maxArmDeg, "%.1f");
        changed |= ImGuiMCP::SliderFloat(L("menu.armik.lift", "Residual body lift (units)"), &g_armEdit.lift, -20.0f, 60.0f, "%.2f");
        if (changed && !g_armModel.empty()) {
            g_armTunes[g_armModel] = g_armEdit;
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                RegisterArmIK(pl, g_armModel, g_armBenchType);
            }
        }
        ImGuiMCP::Separator();
        if (ImGuiMCP::Button(L("menu.armik.saveToModel", "Save to this model"))) {
            if (!g_armModel.empty()) {
                g_armTunes[g_armModel] = g_armEdit;
                SaveConfig();
            }
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.common.zero", "Zero"))) {
            g_armEdit = ArmTune{};
            if (!g_armModel.empty()) {
                g_armTunes[g_armModel] = g_armEdit;
                RegisterArmIK(RE::PlayerCharacter::GetSingleton(), g_armModel, g_armBenchType);
                SaveConfig();
            }
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.common.reloadIni", "Reload INI"))) {
            LoadConfig();
        }
        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.armik.liveApply", "Sliders apply immediately; only Save writes to the INI."));
    }

    void __stdcall RenderMenu()
    {
        ImGuiMCP::Text(L("menu.offset.help1", "Offsets use the character's own facing, not world axes."));
        ImGuiMCP::Text(L("menu.offset.help2", "Applies immediately (on the next use of the same furniture)."));
        ImGuiMCP::Separator();

        const auto fid = g_currentFurnID.load();
        if (fid == 0) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.8f, 0.3f, 1.0f), L("menu.offset.noFurniture", "Not sitting on furniture right now"));
            ImGuiMCP::Text(L("menu.offset.useFirst", "Sit on it first, then come back here."));
            return;
        }

        // 拿到玩家当前坐着的家具引用（取动画类型与"当前生效层级"都要用它）
        RE::TESObjectREFR* playerFurn = nullptr;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            auto furnHandle = player->GetOccupiedFurniture();
            auto furnPtr = furnHandle.get();
            playerFurn = furnPtr ? furnPtr.get() : nullptr;
        }
        auto playerFurnPtr = [&]() -> RE::TESObjectREFR* { return playerFurn; };

        const auto modelKey = GetModelKeyByFormID(fid);
        const auto markerKey = GetMarkerKeyByFormID(fid);
        auto* base = RE::TESForm::LookupByID(fid);
        std::uint32_t flags = 0;
        if (base) {
            flags = GetMarkerFlags(base);
            const char* nm = base->GetName();
            ImGuiMCP::Text(L("menu.offset.furnName", "Furniture: %s  (FormID %08X)"), (nm && *nm) ? nm : L("menu.common.unnamed", "<unnamed>"), fid);
        } else {
            ImGuiMCP::Text(L("menu.offset.furnFid", "Furniture FormID: %08X"), fid);
        }
        const auto benchType = GetBenchType(base);
        if (modelKey.empty()) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.6f, 0.4f, 1.0f), L("menu.offset.modelMissing", "Model: <unavailable, per-reference mode only>"));
        } else {
            ImGuiMCP::Text(L("menu.common.model", "Model: %s"), modelKey.c_str());
        }
        ImGuiMCP::Text(L("menu.offset.flags", "Marker flags: 0x%X"), flags);
        ImGuiMCP::Text(L("menu.offset.benchType", "Bench type: %u (%s)"), benchType, BenchTypeName(benchType));
        if (!markerKey.empty()) {
            ImGuiMCP::Text(L("menu.offset.key", "Override key: %s"), markerKey.c_str());
        }

        ImGuiMCP::Separator();

        // 现状提示：**三层全列出来**，哪一层当前生效就打 ▶
        // （旧版只用 if / else if 显示优先级最高的那一条 ⇒ 存了单件值也被共享键遮住看不见）
        const auto itMarker = markerKey.empty() ? g_markerOverrides.end() : g_markerOverrides.find(markerKey);
        const auto itModel = modelKey.empty() ? g_modelOverrides.end() : g_modelOverrides.find(modelKey);
        const auto itFurn = g_overrides.find(fid);
        const auto nowSrcForTier = [&]() {
            Offset o{};
            OffsetSource s = OffsetSource::kNone;
            ResolveOverride(playerFurnPtr(), o, s);
            return s;
        }();
        const auto green = ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
        const auto amber = ImGuiMCP::ImVec4(1.0f, 0.9f, 0.4f, 1.0f);
        const auto grey = ImGuiMCP::ImVec4(0.6f, 0.6f, 0.6f, 1.0f);

        ImGuiMCP::Text(L("menu.offset.tiers", "-- Stored tiers (▶ = active) --"));
        if (itFurn != g_overrides.end()) {
            ImGuiMCP::TextColored(nowSrcForTier == OffsetSource::kFormID ? green : amber,
                "%s[Furniture] per-ref %08X: F%.2f / L%.2f / V%.2f",
                nowSrcForTier == OffsetSource::kFormID ? "▶ " : "  ", fid,
                itFurn->second.fwd, itFurn->second.lat, itFurn->second.vert);
        } else {
            ImGuiMCP::TextColored(grey, L("menu.offset.tierFurnitureNone", "   [Furniture] per-reference: none"));
        }
        if (itMarker != g_markerOverrides.end()) {
            ImGuiMCP::TextColored(nowSrcForTier == OffsetSource::kMarker ? green : amber,
                "%s[Markers] model@marker: F%.2f / L%.2f / V%.2f",
                nowSrcForTier == OffsetSource::kMarker ? "▶ " : "  ",
                itMarker->second.fwd, itMarker->second.lat, itMarker->second.vert);
        } else {
            ImGuiMCP::TextColored(grey, L("menu.offset.tierMarkersNone", "   [Markers] model@marker: none"));
        }
        if (itModel != g_modelOverrides.end()) {
            ImGuiMCP::TextColored(nowSrcForTier == OffsetSource::kModel ? green : amber,
                "%s[Models] model only: F%.2f / L%.2f / V%.2f",
                nowSrcForTier == OffsetSource::kModel ? "▶ " : "  ",
                itModel->second.fwd, itModel->second.lat, itModel->second.vert);
        } else {
            ImGuiMCP::TextColored(grey, L("menu.offset.tierModelsNone", "   [Models] model only: none"));
        }

        // benchType 默认表这一档单独显示，方便判断"回滚后会落到哪"
        if (const auto itDef = g_benchDefaults.find(benchType); itDef != g_benchDefaults.end()) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                "[Defaults] benchType=%u default: F%.2f / L%.2f / V%.2f",
                benchType, itDef->second.fwd, itDef->second.lat, itDef->second.vert);
        } else {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "[Defaults] benchType=%u has no default (manual only)", benchType);
        }

        // 动画档：从家具模型 NIF 的 BSFurnitureMarker 读，按基对象 FormID 缓存
        // ★ v0.10.1：kSit 不再参与动画档（会串到铁砧/锻炉/附魔台），只有 kSleep/kLean 才有档位。
        auto animInfo = GetAnimInfo(playerFurnPtr());
        const bool animUsesBucket = animInfo.valid && (animInfo.mask & 1) == 0;
        const std::uint32_t hitBit = !animUsesBucket
                                         ? 0u
                                         : ((animInfo.mask & 2) ? 2u : ((animInfo.mask & 4) ? 4u : 0u));
        if (animInfo.valid) {
            ImGuiMCP::Text(L("menu.offset.anim", "Animation: %s (mask 0x%X, %u markers, first entry 0x%X)"),
                AnimMaskName(animInfo.mask).c_str(), animInfo.mask, animInfo.count, animInfo.entryProps);
            if (!animUsesBucket) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "kSit furniture skips the animation tier (managed by [Defaults] benchType or per-model/per-reference values)");
            } else if (const auto itA = g_animOverrides.find(hitBit); itA != g_animOverrides.end()) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                    "[AnimType] %u (%s) default: F%.2f / L%.2f / V%.2f",
                    hitBit, AnimTypeName(hitBit), itA->second.fwd, itA->second.lat, itA->second.vert);
            } else {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                    "[AnimType] %u (%s) has no default", hitBit, AnimTypeName(hitBit));
            }
        } else {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "Animation: <could not read BSFurnitureMarker from the NIF>");
        }
        if (ImGuiMCP::Button(L("menu.offset.reloadAnim", "Re-read animation type"))) {
            animInfo = GetAnimInfo(playerFurnPtr(), true);
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                animInfo.valid ? "Animation type read" : "still unreadable (the furniture 3D may not be loaded)");
        }

        // 当前真正生效的层级 —— 让"为什么抬这么高"一眼可查
        {
            Offset now{};
            OffsetSource nowSrc = OffsetSource::kNone;
            const bool got = ResolveOverride(playerFurnPtr(), now, nowSrc);
            if (got) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    "Active tier: %s  F%.2f / L%.2f / V%.2f",
                    SourceName(nowSrc), now.fwd, now.lat, now.vert);
            } else {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                    "Active tier: none (not fixed)");
            }
        }

        ImGuiMCP::Separator();
        ImGuiMCP::SliderFloat(L("menu.slider.forward", "Forward"), &g_editOffset.fwd, -60.0f, 60.0f, "%.2f");
        ImGuiMCP::SliderFloat(L("menu.slider.lateral", "Lateral"), &g_editOffset.lat, -40.0f, 40.0f, "%.2f");
        ImGuiMCP::SliderFloat(L("menu.slider.vertical", "Vertical"), &g_editOffset.vert, -40.0f, 70.0f, "%.2f");
        ImGuiMCP::Separator();

        // 重算并应用当前坐姿的偏移，同时把滑块同步到"此刻真正生效的值"，
        // 避免界面显示与实际抬升量不一致（回滚 / 归零 / 删覆盖之后尤其明显）。
        auto applyToCurrent = [&]() {
            auto* root = g_currentRoot.load();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!root || !player) {
                return;
            }
            RE::TESObjectREFR* furn = playerFurnPtr();
            float sc = 1.0f;
            bool manual = false;
            OffsetSource src = OffsetSource::kNone;
            Offset local{};
            const auto v = ComputeOffset(player, furn, sc, manual, &src, nullptr, &local);
            // ★ 两套 3D 都登记（与 ENTER 一致）
            const auto roots = CollectActorRoots(player);
            if (local.fwd == 0.0f && local.lat == 0.0f && local.vert == 0.0f) {
                for (auto* r : roots) {
                    g_sittingOffsets.erase(r);
                }
                g_actorRoots.erase(player->GetFormID());
            } else {
                for (auto* r : roots) {
                    SitEntry e{};
                    e.local = local;
                    e.actorID = player->GetFormID();
                    g_sittingOffsets.insert_or_assign(r, e);
                }
                g_actorRoots.insert_or_assign(player->GetFormID(), roots);
            }
            // 滑块跟随真正生效的来源（[Defaults] 命中时显示的就是那张表的值）
            Offset now{};
            OffsetSource nowSrc = OffsetSource::kNone;
            ResolveOverride(furn, now, nowSrc);
            g_editOffset = now;
            logger::info("Menu apply: source={} root={} yaw={:.3f}rad local=({:.2f},{:.2f},{:.2f}) world=({:.2f},{:.2f},{:.2f})",
                SourceName(src), static_cast<const void*>(root), player->GetAngleZ(),
                local.fwd, local.lat, local.vert, v.x, v.y, v.z);
        };

        if (ImGuiMCP::Button(L("menu.btn.saveShared", "Save to model+marker (shared)"))) {
            if (!markerKey.empty()) {
                g_markerOverrides[markerKey] = g_editOffset;
                // 同键的单件覆盖会挡住共享值，顺手清掉，避免"改了却没反应"
                g_overrides.erase(fid);
                SaveConfig();
                applyToCurrent();
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f), L("menu.msg.savedShared", "Saved to model+marker and applied"));
            }
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.saveRef", "Save to this reference only"))) {
            g_overrides[fid] = g_editOffset;
            SaveConfig();
            applyToCurrent();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f), L("menu.msg.savedRef", "Saved to this reference and applied"));
        }

        // 保存到【动画档】：一次覆盖一大类。只有 kSleep/kLean 有档位（kSit 已废弃，见上）。
        {
            const bool canAnim = animUsesBucket && hitBit != 0;
            ImGuiMCP::BeginDisabled(!canAnim);
            auto animLabel = FillBraces(
                L("menu.btn.saveAnim", "Save to animation tier {} (shared by the whole class)"),
                { AnimTypeName(hitBit) });
            if (ImGuiMCP::Button(animLabel.c_str())) {
                if (canAnim) {
                    g_animOverrides[hitBit] = g_editOffset;
                    SaveConfig();
                    applyToCurrent();
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        "Saved to animation tier %u and applied", hitBit);
                }
            }
            if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGuiMCP::SetTooltip(L("menu.tip.saveAnim",
                                       "Writes the current slider values as the default for [AnimType] %u=%s.\n"
                                       "Only beds (kSleep) and leaning (kLean) have a tier; kSit furniture would\n"
                                       "leak into anvils/forges/enchanters, so kSit never uses this tier\n"
                                       "(use the benchType tier below or save per reference)."),
                    hitBit, AnimTypeName(hitBit));
            }
            ImGuiMCP::EndDisabled();
            if (canAnim) {
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button(L("menu.btn.delAnim", "Delete this animation tier"))) {
                    const bool removed = g_animOverrides.erase(hitBit) > 0;
                    SaveConfig();
                    applyToCurrent();
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        removed ? "Deleted this animation tier default" : "this animation tier had no default");
                }
            } else if (!animInfo.valid) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                    "Animation type unavailable; cannot save to an animation tier (use \"Re-read animation type\" first)");
            }
        }

        // 保存到【benchType 档】：铁砧/锻炉/熔炉/制革架 全是 benchType=1，
        // 附魔=3、炼金=5、磨刀石=2、护甲台=7。一次存好，同类全部生效。
        // ★ 只对 kSit（或动画类型未知）放行 —— 床是 kSleep 但 benchType 也是 0，
        //   存进 benchType=0 会把椅子值重新套回床上，正是本轮要消灭的那类错误。
        {
            const bool canBench = !(animInfo.valid && (animInfo.mask & 1) == 0);
            ImGuiMCP::BeginDisabled(!canBench);
            auto benchLabel = FillBraces(
                L("menu.btn.saveBench", "Save to benchType tier ={}({}) (shared by the whole class)"),
                { std::to_string(benchType), BenchTypeName(benchType) });
            if (ImGuiMCP::Button(benchLabel.c_str())) {
                if (canBench) {
                    g_benchDefaults[benchType] = g_editOffset;
                    SaveConfig();
                    applyToCurrent();
                    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        "Saved to [Defaults] benchType=%u and applied", benchType);
                }
            }
            if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGuiMCP::SetTooltip(L("menu.tip.saveBench",
                                       "Writes [Defaults] %u=... so it applies to every benchType=%u piece\n"
                                       "(anvil/forge/smelter/tanning rack are 1; enchanting=3; alchemy=5).\n"
                                       "Disabled for beds (kSleep): their benchType is also 0, so saving here\n"
                                       "would push the sitting value onto beds. Use the animation tier instead."),
                    benchType, benchType);
            }
            ImGuiMCP::EndDisabled();
            if (!canBench) {
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.8f, 1.0f, 1.0f),
                    "Beds/leaning pieces do not use the benchType tier; use the animation tier above");
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(L("menu.btn.delBench", "Delete this benchType tier"))) {
                const bool removed = g_benchDefaults.erase(benchType) > 0;
                SaveConfig();
                applyToCurrent();
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    removed ? "Deleted this benchType tier default" : "this benchType had no default");
            }
        }

        if (ImGuiMCP::Button(L("menu.btn.clear", "Clear"))) {
            g_editOffset = {};
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.delShared", "Delete shared override"))) {
            if (!markerKey.empty()) {
                g_markerOverrides.erase(markerKey);
            }
            if (!modelKey.empty()) {
                g_modelOverrides.erase(modelKey);
            }
            SaveConfig();
            applyToCurrent();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.delRef", "Delete per-reference override"))) {
            g_overrides.erase(fid);
            SaveConfig();
            // 删完要重算：现在可能落到 benchType 默认表，不能只是把目标清掉
            applyToCurrent();
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.offset.revert", "-- Revert / leave unfixed --"));

        // 【回滚到插件默认】：删掉这件家具的全部三级手动覆盖，回落到 [Defaults] 或built-in formula
        const bool canMarker = !markerKey.empty();
        ImGuiMCP::BeginDisabled(!canMarker);
        if (ImGuiMCP::Button(L("menu.btn.revertDefault", "Revert to plugin defaults"))) {
            bool removed = false;
            if (canMarker) {
                removed = g_markerOverrides.erase(markerKey) > 0;
            }
            if (!modelKey.empty()) {
                removed = (g_modelOverrides.erase(modelKey) > 0) || removed;
            }
            removed = (g_overrides.erase(fid) > 0) || removed;
            SaveConfig();
            applyToCurrent();
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                removed ? "Removed all three override tiers; fell back to defaults and applied" : "this piece had no manual override; re-applied using the default");
        }
        if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGuiMCP::SetTooltip(L("menu.tip.revertDefault",
                                   "Removes every override for this furniture in [Markers] / [Models] / [Furniture],\n"
                                   "then recomputes from the [Defaults] benchType value (or the built-in formula)."));
        }
        ImGuiMCP::EndDisabled();

        ImGuiMCP::SameLine();
        ImGuiMCP::BeginDisabled(!canMarker);
        if (ImGuiMCP::Button(L("menu.btn.zeroAll", "Zero everything"))) {
            // 明确"这件not fixed"：写 0,0,0 到共享键，会遮住 [Defaults] 与built-in formula
            if (canMarker) {
                g_markerOverrides[markerKey] = Offset{};
                g_overrides.erase(fid);
                SaveConfig();
                g_editOffset = {};
                applyToCurrent();
                ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    "Wrote 0,0,0 (this piece is not fixed) and applied");
            }
        }
        if (ImGuiMCP::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGuiMCP::SetTooltip(L("menu.tip.zeroShared",
                                   "Writes 0,0,0 to the shared key %s, meaning this furniture is not fixed.\n"
                                   "Note: every other piece sharing the same model and marker is left unfixed too."),
                canMarker ? markerKey.c_str() : L("menu.common.noModelPath", "<model path unavailable>"));
        }
        ImGuiMCP::EndDisabled();
        if (!canMarker) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "Model path unavailable, so the shared key cannot be located; use \"Save to this reference only\" to write 0,0,0");
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.offset.counts", "shared %zu (%zu models) / per-reference %zu / benchType %zu / anim %zu"),
            g_markerOverrides.size(), g_modelOverrides.size(), g_overrides.size(), g_benchDefaults.size(),
            g_animOverrides.size());
    }

    void RegisterMenu()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::warn("SKSEMenuFramework.dll not found; menu not registered (plugin still works)");
            return;
        }
        SKSEMenuFramework::SetSection("Furniture User Size Fix");
        SKSEMenuFramework::AddSectionItem("Furniture Offsets", RenderMenu);
        SKSEMenuFramework::AddSectionItem("Upper-body IK (arm raise)", RenderArmMenu);
        logger::info("Menu registered: Furniture User Size Fix / Furniture Offsets + Upper-body IK");
        logger::info("Menu registered: Furniture User Size Fix / Furniture Offsets");
    }

#else   // ---- 没有 SKSE Menu Framework 头：菜单不编译，其余功能照常 ----
    void RegisterMenu()
    {
        logger::info("Built without SKSE Menu Framework support; "
                     "the in-game menu is unavailable (configure via the INI).");
    }

#endif

    // ---------------- 事件 ----------------
    class FurnitureSink final : public RE::BSTEventSink<RE::TESFurnitureEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESFurnitureEvent* a_event,
            RE::BSTEventSource<RE::TESFurnitureEvent>*) override
        {
            try {
                if (!a_event || !a_event->actor) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* actor = a_event->actor->As<RE::Actor>();
                if (!actor) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool enter = a_event->type.get() == RE::TESFurnitureEvent::FurnitureEventType::kEnter;
                const bool isPlayer = (actor == RE::PlayerCharacter::GetSingleton());
                RE::NiPointer<RE::Actor> keep(actor);
                // 事件自己带着家具引用，比事后再去 GetOccupiedFurniture() 稳（EXIT 时后者已空）
                RE::NiPointer<RE::TESObjectREFR> furnKeep(a_event->targetFurniture.get());

                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([keep, furnKeep, enter, isPlayer]() noexcept {
                        try {
                            auto* a = keep.get();
                            if (!a) {
                                return;
                            }
                            const auto actorID = a->GetFormID();
                            auto* root = a->Get3D();
                            if (enter) {
                                if (!TryInstallHook()) {
                                    return;
                                }
                                // ENTER 必须有 3D 才有意义
                                if (!root) {
                                    return;
                                }
                                if (g_sittingOffsets.size() > 64) {
                                    g_sittingOffsets.clear();
                                    logger::warn("Target set grew abnormally large; cleared");
                                }
                                RE::TESObjectREFR* furn = furnKeep.get();
                                if (!furn) {
                                    auto furnHandle = a->GetOccupiedFurniture();
                                    auto furnPtr = furnHandle.get();
                                    furn = furnPtr ? furnPtr.get() : nullptr;
                                }
                                float scale = 1.0f;
                                bool manual = false;
                                OffsetSource src = OffsetSource::kNone;
                                float kFactor = 1.0f;
                                Offset localOff{};
                                const auto v = ComputeOffset(a, furn, scale, manual, &src, &kFactor, &localOff);
                                const auto baseID = furn && furn->GetBaseObject()
                                                        ? furn->GetBaseObject()->GetFormID()
                                                        : 0;
                                const auto mkKey = GetMarkerKey(furn);
                                const auto benchType = GetBenchType(furn ? furn->GetBaseObject() : nullptr);
                                const auto anim = GetAnimInfo(furn);
                                const auto pureModel = GetModelKey(furn);   // 手臂 IK 与菜单都用它
                                if (isPlayer) {
                                    g_currentFurnID.store(baseID);
                                    g_currentRoot.store(root);
                                    // 菜单初值走与运行时完全相同的取值链
                                    //   @坐点 > 纯模型 > 单件 > benchType专用 > 动画档 > benchType通用
                                    Offset init{};
                                    OffsetSource initSrc = OffsetSource::kNone;
                                    ResolveOverride(furn, init, initSrc);
                                    g_editOffset = init;
                                    // 手臂 IK 菜单状态：**显示实际生效值**
                                    // （旧写法在拿不到显式条目时无条件填默认 45/5.3，
                                    //   导致躺在床上一看菜单就以为床被套了 IK —— 其实根本没生效）
                                    g_armFurnID.store(baseID);
                                    g_armModel = pureModel;
                                    g_armBenchType = benchType;
                                    g_armEdit = EffectiveArmTune(pureModel, benchType);
                                    // ---- 探针：把各候选高度和"配置里当前相当于多少 h"打一行 ----
                                    if (g_config.probeBones) {
                                        const float denom = 1.0f - g_config.calibrationScale;
                                        const float hCfg = (denom > 0.01f) ? init.vert / denom : 0.0f;
                                        std::vector<ProbeHit> hits;
                                        ProbeBones(root, 0, root->world.translate.z, hits);
                                        std::string line;
                                        for (const auto& h : hits) {
                                            line += std::format("{}={:.2f} ", h.name, h.dz);
                                        }
                                        // 手/脚/头这种深节点再用精确名字兜一次
                                        std::string exact;
                                        const float rz = root->world.translate.z;
                                        for (const auto n : { "NPC L Hand [LHnd]", "NPC R Hand [RHnd]",
                                                 "NPC L Foot [LFt ]", "NPC R Foot [RFt ]",
                                                 "NPC Head [Head]", "NPC Neck [Neck]" }) {
                                            ProbeExact(root, n, rz, exact);
                                        }
                                        logger::info("[PROBE] furn=0x{:08X} model={} scale={:.3f} cfgH={:.2f} source={} | exact: {}| walk: {}",
                                            baseID, mkKey.empty() ? "<none>" : mkKey.c_str(), scale, hCfg,
                                            SourceName(initSrc), exact, line.empty() ? "<no matching nodes>" : line);
                                    }
                                }
                                // ★ 上肢 IK（v0.13.0 合并）：残余抬升并入 localOff.vert，
                                //   手臂旋转单独登记到 g_armEntries。
                                const float armLift = ArmLiftFor(pureModel, benchType, scale);
                                if (armLift != 0.0f) {
                                    localOff.vert += armLift;
                                }
                                const auto armCount = RegisterArmIK(a, pureModel, benchType);

                                const bool zero = (localOff.fwd == 0.0f && localOff.lat == 0.0f && localOff.vert == 0.0f);
                                // 同一 actor 上一次的记录先清掉（防止漏 EXIT 后累积/串到新节点）
                                if (const auto prev = g_actorRoots.find(actorID); prev != g_actorRoots.end()) {
                                    for (auto* r : prev->second) {
                                        g_sittingOffsets.erase(r);
                                    }
                                    g_actorRoots.erase(prev);
                                }
                                // ★ 两套 3D（第三人称体 / 第一人称体）都要登记，否则看不见的那套会穿模
                                const auto roots = CollectActorRoots(a);
                                if (!zero) {
                                    for (auto* r : roots) {
                                        SitEntry e{};
                                        e.local = localOff;
                                        e.actorID = actorID;
                                        g_sittingOffsets.insert_or_assign(r, e);
                                    }
                                    g_actorRoots.insert_or_assign(actorID, roots);
                                } else {
                                    g_sittingOffsets.erase(root);
                                }
                                {
                                    std::string rootList;
                                    for (auto* r : roots) {
                                        rootList += std::format("{} ", static_cast<const void*>(r));
                                    }
                                    logger::info(">>> ENTER [{}] furn=0x{:08X} key={} benchType={} anim={} markers={} roots=[{}] yaw={:.3f}rad engineScale={:.3f} effScale={:.3f} k={:.3f} local=({:.2f},{:.2f},{:.2f} ) source={} world=({:.2f},{:.2f},{:.2f}) armBones={} targets={}",
                                        isPlayer ? "PLAYER" : "NPC", baseID,
                                        mkKey.empty() ? "<none>" : mkKey.c_str(), benchType,
                                        anim.valid ? AnimMaskName(anim.mask).c_str() : "<unread>",
                                        anim.valid ? anim.count : 0u,
                                        rootList.empty() ? "none" : rootList.c_str(), a->GetAngleZ(),
                                        a->GetScale(), scale, kFactor,
                                        localOff.fwd, localOff.lat, localOff.vert,
                                        zero ? "not fixed" : SourceName(src),
                                        v.x, v.y, v.z, armCount, g_sittingOffsets.size());
                                }
                            } else {
                                // ★ v0.10.1：EXIT **完全不依赖** Get3D() 是否可用。
                                //   旧版第一步就 `if (!root) return;`，而第一人称退出家具的瞬间
                                //   Get3D() 常常为 null（1st/3rd 两套 3D 正在切换）⇒ 直接返回、
                                //   偏移永不清 ⇒ 角色浮空，要再坐一次再退出才归零（用户实测）。
                                //   现在按 ENTER 时登记的 root 删，root 为 null 也照样能清。
                                std::size_t removed = 0;
                                if (const auto it = g_actorRoots.find(actorID); it != g_actorRoots.end()) {
                                    for (auto* r : it->second) {
                                        removed += g_sittingOffsets.erase(r);
                                    }
                                    g_actorRoots.erase(it);
                                }
                                // 兜底：当前 3D（两套都试）若还挂着也一起删
                                for (auto* r : CollectActorRoots(a)) {
                                    removed += g_sittingOffsets.erase(r);
                                }
                                ClearArms(actorID);   // ★ 手臂 IK 同步清掉
                                if (isPlayer) {
                                    g_currentFurnID.store(0);
                                    g_currentRoot.store(nullptr);
                                    g_armFurnID.store(0);
                                    g_armModel.clear();
                                }
                                logger::info("<<< EXIT [{}] 3D={} cleared={} targets={}",
                                    isPlayer ? "PLAYER" : "NPC", root ? "valid" : "null",
                                    removed, g_sittingOffsets.size());
                            }
                        } catch (const std::exception& e) {
                            logger::error("Furniture handler error: {}", e.what());
                        } catch (...) {
                            logger::error("Furniture handler: unknown error");
                        }
                    });
                }
            } catch (...) {
                logger::error("FurnitureSink::ProcessEvent threw (swallowed)");
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    RE::BSTEventSink<RE::TESFurnitureEvent>* g_sink = new FurnitureSink();

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }
        if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame) {
            g_sittingOffsets.clear();
            // 读档后 3D 会重建，marker 缓存一并作废（下次坐下时重读）
            g_animCache.clear();
            g_animFailCount.clear();
            g_actorRoots.clear();
            g_armEntries.clear();
            g_actorArms.clear();
            g_currentRoot.store(nullptr);
            g_currentFurnID.store(0);
            logger::info("kPostLoadGame: runtime state cleared (including animation cache)");
            return;
        }
        if (a_msg->type != SKSE::MessagingInterface::kDataLoaded) {
            return;
        }
        logger::info("kDataLoaded");

        ResolveForbid();

        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink(g_sink);
            logger::info("TESFurnitureEvent sink registered");
        }
        TryInstallHook();
        RegisterMenu();
    }
}

SKSEPluginInfo(
    .Version = REL::Version{ 1, 2, 0, 0 },
    .Name = "FurnitureUserSizeFix"sv,
    .Author = "RandyLua"sv,
    .SupportEmail = ""sv,
    // ★ 多运行时（SE 1.5.97 + AE 1.6.629+）：
    //   Independent = 不依赖某个运行时独有的结构布局；
    //   AddressLibrary = 两个运行时都靠 Address Library 解析 REL::ID 偏移。
    //   代价：AE 侧运行时必须有对应的 version-1-6-xxx-0.bin。
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = REL::Version{ 0, 2, 0, 20 })

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    auto path = logger::log_directory();
    if (!path) {
        return false;
    }
    *path /= "FurnitureUserSizeFix.log"sv;

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");

    logger::info("FurnitureUserSizeFix v1.2.0 loading...");
    logger::info("Offsets are shared per model@marker: tune the same model+marker once");
    logger::info("Lookup order: [Markers] model@marker > [Models] model > [Furniture] per-reference > [Defaults] benchType > [AnimType] (kSleep/kLean only) > built-in formula");

    LoadConfig();

    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }
    return true;
}


# AniDesk-Qt 外部 AI 服务部署指南

把"拖图变身"从**降级效果**（静态原图）升级为**完整效果**（透明角色 + AI 起名 + 专属动图），
需要部署以下 1~3 个服务。按投入从低到高排序，装多少生效多少。

---

## ① 视觉模型（AI 起名 / 人设提取）—— 一条命令

管线第二步的 `PersonaExtractor` 通过 Ollama 调用多模态模型"看图识角色"。

```bash
# 显存较小（≤6GB，如 RTX 3050 4GB）选 3b：
ollama pull qwen2.5vl:3b
# 显存充裕（≥8GB）可选 7b，效果更好：
ollama pull qwen2.5vl:7b
```

> 显存提示：桌宠对话模型（qwen3.5:0.8b，约 1.1GB）常驻显存；4GB 卡上再加载 3b 视觉模型
> 会部分卸载到 CPU，人设提取单次约 1~3 分钟（拖图后看气泡进度即可），对话不受影响。

然后在 `src/PetWidget.cpp` 的 `setupAiServices()` 中把模型名改成与你拉取的一致：

```cpp
m_pipeline->setVisionApi("http://localhost:11434/v1", "", "qwen2.5vl:3b");
```

重新编译后，拖图时气泡会显示"人设提取完成：xxx"，角色获得 AI 起的名字、性格与打招呼台词。

## ② 本地抠图服务（透明角色）—— 一个 Python 脚本

管线第一步的 `BackgroundRemover` 把图片背景去掉，让角色以透明 PNG 浮在桌面上
（而不是一张矩形照片）。

```bash
pip install fastapi uvicorn rembg onnxruntime
python services/rmbg_server.py      # 监听 127.0.0.1:8765
```

- 首次抠图会自动下载 U2Net 模型（约 170MB）到用户目录。
- 接口契约：`POST http://127.0.0.1:8765/removebg`，请求体为图片字节，响应为透明 PNG 字节
  （与 `src/BackgroundRemover.h` 中 `Mode::LocalHttp` 的约定一致）。
- 也可改用 [remove.bg](https://www.remove.bg/) 云端：`setBackgroundRemovalApi("https://api.remove.bg/v1.0/remove-bg", "你的Key")`。

## ③ 动图驱动服务（角色有动作）—— ComfyUI 节点方案（已选定）

管线第二步的 `LivePortraitClient` 对透明形象生成三段动图（眨眼/说话/微笑）。
已选定 **ComfyUI + AdvancedLivePortrait 节点**方案，本仓库提供完整对接件：

```
services/
├── comfyui_animate_server.py                            # 包装服务：/animate 契约 → ComfyUI 队列
├── comfyui_workflows/advanced_liveportrait_animate.json # 工作流模板（占位符可替换）
├── driving/                                             # 驱动视频：blink/speak/smile.mp4
└── install_comfyui.bat                                  # ComfyUI 便携版自动化安装
```

**部署步骤：**

1. **安装 ComfyUI**：双击 `install_comfyui.bat`（下载约 2.5GB Windows 便携版 +
   克隆 [ComfyUI-AdvancedLivePortrait](https://github.com/dfl/ComfyUI-AdvancedLivePortrait)
   节点；也可手动装，装好后 ComfyUI 需可访问 `http://127.0.0.1:8188`）。
2. **准备驱动视频**：三段人脸驱动视频按 `blink.mp4 / speak.mp4 / smile.mp4`
   命名放入 `services/driving/`（可从
   [LivePortrait 仓库示例](https://github.com/KwaiVGI/LivePortrait/tree/main/assets/examples/driving)
   下载，或自拍）。
3. **启动**：先启 ComfyUI（`run_nvidia_gpu.bat`），再安装包装服务依赖
   `pip install fastapi uvicorn httpx`（建议也放进 venv）并运行
   `python services/comfyui_animate_server.py`——监听 `127.0.0.1:7890`，
   正是桌宠默认的动图服务地址，无需改任何 C++ 代码。
4. **工作流校准**：若你的节点版本与模板不一致，在 ComfyUI 网页里摆好工作流后用
   **Save (API Format)** 导出替换
   `comfyui_workflows/advanced_liveportrait_animate.json`，并把图片输入写成
   `{{IMAGE}}`、驱动视频输入写成 `{{DRIVING_VIDEO}}`、帧率写成 `{{FPS}}`。

**本机注意事项（RTX 3050 Laptop 4GB）：**

- 动图推理前先腾显存：让 Ollama 卸载模型（`ollama stop qwen3.5:0.8b` 等），
  否则 4GB 装不下两套模型。
- LivePortrait 是**人脸驱动**模型，对虹猫这类卡通角色效果需实测；检测不佳时
  调整 AdvancedLivePortrait 的裁剪参数或更换驱动视频。
- 生成一段动图约 30~120 秒，完成后桌宠自动换上动图形象。
- 说明：LivePortrait 驱动的是**面部表情**；"拔剑"这类肢体动作需要图像到视频
  生成模型（Kling/即梦等云端 API），契约不变、换包装服务即可，作为后续扩展。

---

## 部署完成后的效果对照

| 部署情况 | 拖图后的效果 |
|---|---|
| 什么都没部署 | 矩形原图当宠物（自带呼吸浮动微动作） |
| + ① 视觉模型 | AI 起名："变身完成！我是「虹猫」～" + 专属性格与台词 |
| + ② 抠图服务 | 背景消失，角色透明浮在桌面上 |
| + ③ 动图服务 | 角色会眨眼、说话张嘴、微笑——真正的"活"宠物 |

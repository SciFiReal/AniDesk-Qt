#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
comfyui_animate_server.py —— AniDesk-Qt 动图服务（ComfyUI LivePortrait 包装层）

与 LivePortraitClient 的契约：
    POST http://127.0.0.1:7890/animate
    { "image": "<透明PNG base64>", "template": "blink|speak|smile" }
    响应：{ "gif_b64": "<GIF base64>" }

依赖 ComfyUI（默认 http://127.0.0.1:8188）已安装：
    1. ComfyUI（推荐 Windows 便携版）
    2. 自定义节点：ComfyUI-AdvancedLivePortrait（dfl）
       git clone 到 ComfyUI/custom_nodes/ 后按其 README 安装模型
    3. 驱动视频：services/driving/{blink,speak,smile}.mp4
       （可用 LivePortrait 仓库自带的示例驱动视频，或自拍人脸视频）
    4. 工作流模板：services/comfyui_workflows/advanced_liveportrait_animate.json
       （若节点版本不同，请在 ComfyUI 里摆好工作流后用 Save (API Format) 导出，
         并把图片输入替换为 {{IMAGE}}、驱动视频输入替换为 {{DRIVING_VIDEO}}）

安装依赖：
    pip install fastapi uvicorn httpx

运行：
    python services/comfyui_animate_server.py     # 监听 127.0.0.1:7890
"""
import base64
import json
import time
from pathlib import Path

import httpx
from fastapi import FastAPI
from fastapi.responses import JSONResponse

COMFY_BASE = "http://127.0.0.1:8188"
WORKFLOW_PATH = Path(__file__).parent / "comfyui_workflows" / "advanced_liveportrait_animate.json"
DRIVING_DIR = Path(__file__).parent / "driving"
POLL_INTERVAL = 1.0
QUEUE_TIMEOUT = 900        # ComfyUI 排队+推理总超时（秒）

TEMPLATE_VIDEO = {
    "idle": "blink.mp4",     # 眨眼 → Idle.gif
    "talking": "speak.mp4",  # 说话 → Talking.gif
    "happy": "smile.mp4",    # 微笑 → Happy.gif
}

app = FastAPI(title="AniDesk LivePortrait Service (ComfyUI)")


# ---------------------------------------------------------------- ComfyUI API
def upload_to_comfy(client: httpx.Client, data: bytes, filename: str, mime: str) -> str:
    """上传素材到 ComfyUI，返回服务器端文件名（input 目录引用名）"""
    files = {"image": (filename, data, mime)}
    r = client.post(f"{COMFY_BASE}/upload/image", files=files,
                    data={"overwrite": "true"}, timeout=120)
    r.raise_for_status()
    info = r.json()
    name = info["name"]
    if info.get("subfolder"):
        name = f"{info['subfolder']}/{name}"
    return name


def queue_workflow(client: httpx.Client, workflow: dict) -> str:
    """提交工作流到 ComfyUI 队列，返回 prompt_id"""
    r = client.post(f"{COMFY_BASE}/prompt", json={"prompt": workflow}, timeout=60)
    if r.status_code != 200:
        # ComfyUI 会在响应里指出哪个节点/参数不合法，原样透出方便排查
        raise RuntimeError(f"ComfyUI 拒绝工作流: {r.text[:500]}")
    return r.json()["prompt_id"]


def wait_for_output(client: httpx.Client, prompt_id: str) -> dict:
    """轮询 /history 直到该任务完成，返回包含输出的那条 history 记录"""
    deadline = time.time() + QUEUE_TIMEOUT
    while time.time() < deadline:
        r = client.get(f"{COMFY_BASE}/history/{prompt_id}", timeout=30)
        history = r.json().get(prompt_id)
        if history:                        # completed（含出错完成）
            status = history.get("status", {})
            if not status.get("completed", True):
                raise RuntimeError(f"ComfyUI 工作流执行失败: {str(status)[:500]}")
            return history
        time.sleep(POLL_INTERVAL)
    raise RuntimeError("等待 ComfyUI 输出超时")


def fetch_output_gif(client: httpx.Client, history: dict) -> bytes:
    """从 history 的 outputs 里找第一个 GIF 文件并通过 /view 下载"""
    outputs = history.get("outputs", {})
    for node_out in outputs.values():
        for key in ("gifs", "images", "videos"):
            for item in node_out.get(key, []):
                if not item.get("filename", "").lower().endswith(".gif"):
                    continue
                r = client.get(f"{COMFY_BASE}/view", params={
                    "filename": item["filename"],
                    "subfolder": item.get("subfolder", ""),
                    "type": item.get("type", "output"),
                }, timeout=300)
                r.raise_for_status()
                return r.content
    raise RuntimeError(f"输出中未找到 GIF: {json.dumps(outputs)[:500]}")


def build_workflow(image_ref: str, driving_ref: str, fps: int) -> dict:
    """读取工作流模板并替换占位符（{{IMAGE}}/{{DRIVING_VIDEO}}/{{FPS}}）"""
    text = WORKFLOW_PATH.read_text(encoding="utf-8")
    text = (text.replace("{{IMAGE}}", image_ref)
                .replace("{{DRIVING_VIDEO}}", driving_ref)
                .replace("{{FPS}}", str(fps)))
    return json.loads(text)


# ---------------------------------------------------------------- API
@app.get("/health")
def health():
    try:
        r = httpx.get(f"{COMFY_BASE}/system_stats", timeout=5)
        return {"status": "ok", "comfyui": r.status_code == 200}
    except Exception:
        return {"status": "ok", "comfyui": False,
                "hint": f"ComfyUI 未响应，请确认已启动: {COMFY_BASE}"}


@app.post("/animate")
async def animate(request: Request):
    body = await request.json()
    image_b64 = body.get("image", "")
    template = body.get("template", "idle")
    fps = int(body.get("fps", 25))

    driving_name = TEMPLATE_VIDEO.get(template)
    if not driving_name:
        return JSONResponse(status_code=400,
                            content={"error": f"未知模板: {template}"})
    driving_path = DRIVING_DIR / driving_name
    if not driving_path.exists():
        return JSONResponse(status_code=500, content={
            "error": f"缺少驱动视频 {driving_path}（放入 LivePortrait 示例或自拍视频）"})
    if not WORKFLOW_PATH.exists():
        return JSONResponse(status_code=500, content={
            "error": f"缺少工作流模板 {WORKFLOW_PATH}"})

    try:
        with httpx.Client() as client:
            # 1) 上传透明形象与驱动视频
            image_ref = upload_to_comfy(client, base64.b64decode(image_b64),
                                        f"anidesk_src_{int(time.time())}.png",
                                        "image/png")
            driving_ref = upload_to_comfy(client, driving_path.read_bytes(),
                                          driving_name, "video/mp4")
            # 2) 提交工作流并等待
            workflow = build_workflow(image_ref, driving_ref, fps)
            prompt_id = queue_workflow(client, workflow)
            history = wait_for_output(client, prompt_id)
            # 3) 取回 GIF
            gif_bytes = fetch_output_gif(client, history)
    except httpx.HTTPError as e:
        return JSONResponse(status_code=502,
                            content={"error": f"ComfyUI 通信失败: {e}"})
    except RuntimeError as e:
        return JSONResponse(status_code=500, content={"error": str(e)})

    return {"gif_b64": base64.b64encode(gif_bytes).decode()}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=7890)

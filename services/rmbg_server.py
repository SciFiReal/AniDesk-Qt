#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rmbg_server.py —— AniDesk-Qt 本地抠图服务（与 BackgroundRemover 的 LocalHttp 契约匹配）

接口契约：
    POST http://127.0.0.1:8765/removebg
    请求体：原始图像字节（image/png 或 image/jpeg）
    响应：  带透明通道的 PNG 字节（image/png）

安装（一次性）：
    pip install fastapi uvicorn rembg onnxruntime

运行：
    python services/rmbg_server.py
    （首次抠图时会自动下载 U2Net 模型约 170MB 到用户目录）

启动后在 AniDesk-Qt 里拖入任意静态图片，管线第一步就会真正抠出
透明角色（而不是走"使用原图"的降级路径）。
"""
from io import BytesIO

from fastapi import FastAPI, Request
from fastapi.responses import Response

app = FastAPI(title="AniDesk RMBG Service")

# rembg 会话：默认 u2net 通用模型（首次调用自动下载模型文件）
_session = None


def get_session():
    global _session
    if _session is None:
        from rembg import new_session
        _session = new_session("u2net")
    return _session


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/removebg")
async def removebg(request: Request):
    data = await request.body()
    if not data:
        return Response(content='{"error": "empty body"}',
                        status_code=400, media_type="application/json")

    # 懒加载：首次请求才 import/下载模型，避免启动卡顿
    from rembg import remove
    result = remove(data, session=get_session())

    # 统一重编码为 PNG（rembg 返回的本身就是 PNG 字节，这里过一道保险）
    from PIL import Image
    image = Image.open(BytesIO(result)).convert("RGBA")
    out = BytesIO()
    image.save(out, format="PNG")
    return Response(content=out.getvalue(), media_type="image/png")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8765)

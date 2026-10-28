import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path

import uvicorn
from fastapi import FastAPI, HTTPException, WebSocket
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from eye import EyeController
from pipeline import Pipeline
from settings import settings

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")

eye = EyeController(settings.eye_url, settings.calibration_path, settings.eye_send_hz)
pipeline = Pipeline(settings, on_people=eye.on_people)


@asynccontextmanager
async def lifespan(app: FastAPI):
    await asyncio.to_thread(pipeline.load_model)
    pipeline.start()
    eye_task = asyncio.create_task(eye.run())
    yield
    eye_task.cancel()
    pipeline.shutdown()


app = FastAPI(lifespan=lifespan)


class TeachMode(BaseModel):
    active: bool


class Aim(BaseModel):
    yaw: float
    pitch: float


@app.get("/api/eye")
def eye_state():
    return eye.state()


@app.post("/api/teach")
def teach(body: TeachMode):
    eye.set_teach(body.active)
    return eye.state()


@app.post("/api/teach/aim")
def teach_aim(body: Aim):
    eye.teach_aim(body.yaw, body.pitch)
    return {"ok": True}


@app.post("/api/calibration/points")
def add_point():
    if error := eye.save_teach_point():
        raise HTTPException(409, error)
    return eye.state()


@app.delete("/api/calibration/points/{index}")
def delete_point(index: int):
    if not eye.remove_point(index):
        raise HTTPException(404, "No such point")
    return eye.state()


@app.delete("/api/calibration/points")
def clear_points():
    eye.clear_points()
    return eye.state()


@app.websocket("/ws")
async def stream(websocket: WebSocket):
    await websocket.accept()
    queue = pipeline.subscribe()

    async def send():
        while True:
            message = await queue.get()
            if isinstance(message, str):
                await websocket.send_text(message)
            else:
                await websocket.send_bytes(message)

    async def receive():
        # We don't expect client messages; this just notices disconnects promptly.
        while True:
            await websocket.receive_text()

    tasks = [asyncio.create_task(send()), asyncio.create_task(receive())]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for task in tasks:
            task.cancel()
        # Either task ending (disconnect, send failure) ends the session; swallow its error.
        await asyncio.gather(*tasks, return_exceptions=True)
        pipeline.unsubscribe(queue)


app.mount("/", StaticFiles(directory=Path(__file__).parent / "static", html=True), name="static")


if __name__ == "__main__":
    uvicorn.run(app, host=settings.host, port=settings.port)

import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path

import uvicorn
from fastapi import FastAPI, WebSocket
from fastapi.staticfiles import StaticFiles

from pipeline import Pipeline
from settings import settings

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")

pipeline = Pipeline(settings)


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Load the model up front so the first viewer doesn't wait for it.
    await asyncio.to_thread(pipeline.load_model)
    yield
    pipeline.shutdown()


app = FastAPI(lifespan=lifespan)


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

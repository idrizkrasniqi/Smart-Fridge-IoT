import cv2
import threading
import asyncio
import json
from aiortc import RTCPeerConnection, RTCSessionDescription, VideoStreamTrack, RTCIceCandidate
from av import VideoFrame
import websockets
from pyngrok import ngrok, conf
from urllib.parse import urlparse
from flask import Flask
import time
import requests
import base64
import signal
import sys

# --- Shared Threaded Video Capture ---
class VideoCaptureThreaded:
    def __init__(self, src=0):
        self.cap = None
        for index in range(3):  # Try indices 0, 1, 2
            self.cap = cv2.VideoCapture(index, cv2.CAP_DSHOW)
            if self.cap.isOpened():
                print(f"Camera opened successfully with index {index}")
                break
        if not self.cap or not self.cap.isOpened():
            print("Error: Could not open any camera. Check if camera is connected and not in use.")
            self.running = False
            return
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 2)
        self.frame = None
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self._update, args=())
        self.thread.daemon = True
        self.thread.start()

    def _update(self):
        while self.running:
            ret, frame = self.cap.read()
            if ret:
                with self.lock:
                    self.frame = frame
            else:
                print("Warning: Failed to read frame.")
            time.sleep(0.01)

    def read(self, mirror=False):
        with self.lock:
            frame = self.frame.copy() if self.frame is not None else None
            if frame is not None and mirror:
                frame = cv2.flip(frame, 1)  # Horizontal flip for mirror effect
            return frame

    def stop(self):
        self.running = False
        if self.thread.is_alive():
            self.thread.join()
        if self.cap:
            self.cap.release()

# Instantiate shared video capture
video = VideoCaptureThreaded()

# --- Modified CameraStreamTrack to use shared video ---
class CameraStreamTrack(VideoStreamTrack):
    def __init__(self):
        super().__init__()

    async def recv(self):
        frame = video.read(mirror=True)  # Enable mirror for live stream
        if frame is None:
            print("No frame available, sleeping...")
            await asyncio.sleep(0.02)
            return await self.recv()
        frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        video_frame = VideoFrame.from_ndarray(frame_rgb, format="rgb24")
        pts, time_base = await self.next_timestamp()
        video_frame.pts = pts
        video_frame.time_base = time_base
        return video_frame

# --- WebRTC signaling handler ---
async def signaling_handler(websocket):
    print("Incoming signaling connection")
    pc = RTCPeerConnection()
    pc.addTrack(CameraStreamTrack())

    @pc.on("icecandidate")
    async def on_ice(candidate):
        if candidate:
            try:
                await websocket.send(json.dumps({
                    "type": "candidate",
                    "candidate": {
                        "candidate": candidate.candidate,
                        "sdpMid": candidate.sdpMid,
                        "sdpMLineIndex": candidate.sdpMLineIndex
                    }
                }))
            except Exception:
                pass

    try:
        async for message in websocket:
            try:
                data = json.loads(message)
            except Exception:
                continue

            mtype = data.get("type")
            if mtype == "offer" and "sdp" in data:
                print("Received offer — creating answer")
                offer = RTCSessionDescription(sdp=data["sdp"], type="offer")
                await pc.setRemoteDescription(offer)
                answer = await pc.createAnswer()
                await pc.setLocalDescription(answer)
                await websocket.send(json.dumps({
                    "type": "answer",
                    "sdp": pc.localDescription.sdp
                }))
                print("Sent answer back to client.")
            elif mtype == "candidate" and "candidate" in data:
                cand = data["candidate"]
                try:
                    ice = RTCIceCandidate(
                        sdpMid=cand.get("sdpMid"),
                        sdpMLineIndex=cand.get("sdpMLineIndex"),
                        candidate=cand.get("candidate")
                    )
                    await pc.addIceCandidate(ice)
                except Exception:
                    pass
    except websockets.exceptions.ConnectionClosed:
        print("Signaling connection closed")
    except Exception as e:
        print("signaling handler error:", e)
    finally:
        try:
            await pc.close()
        except Exception:
            pass
        print("Peer connection closed")

# --- Start WebSocket server ---
async def ws_main(host="0.0.0.0", port=8765):
    server = await websockets.serve(signaling_handler, host, port)
    print(f"WebSocket signaling server running on ws://{host}:{port}")
    await server.wait_closed()

def start_ws_server_in_thread(loop):
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(ws_main("0.0.0.0", 8765))
    except Exception as e:
        print("WS server stopped:", e)
    finally:
        try:
            loop.run_until_complete(loop.shutdown_asyncgens())
        except Exception:
            pass
        loop.close()

# --- Flask App ---
app = Flask(__name__)

# URL e Google Apps Script Web App
GOOGLE_APPS_SCRIPT_URL = "https://script.google.com/macros/s/AKfycby4Zw-kOY4Tlq6aURPAnvAuirfob2C4Spo6MImLdj82JhLsvDy8wyQcS47vO7cWEXqYUw/exec"  # Zëvendëso me URL-në tënde

# Route për të kapur foto dhe dërguar te Google Apps Script
@app.route('/trigger_photo', methods=['GET'])
def trigger_photo():
    global video
    frame = video.read(mirror=True)  # Enable mirror for photo
    if frame is not None:
        frame = cv2.resize(frame, (640, 480))
        ret, buffer = cv2.imencode('.jpg', frame)
        if ret:
            base64_image = base64.b64encode(buffer).decode('utf-8')
            threading.Thread(target=send_to_google_drive, args=(base64_image,)).start()
            return "Foto u kap dhe po dërgohet në Google Drive!", 200
    return "Gabim gjatë kapjes së fotos.", 500

# Funksioni për të dërguar foto te Google Apps Script
def send_to_google_drive(base64_image):
    try:
        headers = {'Content-Type': 'application/json'}
        data = {'image': base64_image}
        response = requests.post(GOOGLE_APPS_SCRIPT_URL, json=data, headers=headers)
        if response.status_code == 200:
            print(f"Foto u ruajt në Google Drive: {response.json()}")
        else:
            print(f"Gabim gjatë dërgimit në Google Drive: {response.text}")
    except Exception as e:
        print(f"Gabim gjatë dërgimit në Google Drive: {str(e)}")

def run_flask():
    app.run(host='0.0.0.0', port=5000, debug=True, use_reloader=False, threaded=True)

# Shutdown handler
def shutdown():
    print("Shutting down...")
    video.stop()
    ngrok.kill()  # Disconnect ngrok tunnels
    sys.exit(0)

if __name__ == '__main__':
    # Konfiguro ngrok
    conf.get_default().ngrok_path = r'C:\Users\idriz\Downloads\ngrok-v3-stable-windows-amd64\ngrok.exe'
    conf.get_default().auth_token = "31UwiwAtZw7zyMkgRGUhFPigms4_7xf14KBQ3meDSSmVLUzF4"  # Token nga 3.py për Flask static domain

    # Create asyncio loop for WS
    ws_loop = asyncio.new_event_loop()

    # Start WebSocket server në thread
    ws_thread = threading.Thread(target=start_ws_server_in_thread, args=(ws_loop,), daemon=True)
    ws_thread.start()

    # Nis Flask në një thread të veçantë
    flask_thread = threading.Thread(target=run_flask, daemon=True)
    flask_thread.start()

    # Konfiguro ngrok tunnels: static për WS, dynamic për Flask (trigger_photo)
    try:
        ws_public_url = ngrok.connect(8765, hostname="settling-lobster-distinctly.ngrok-free.app")
        flask_public_url = ngrok.connect(5000, proto="http")
    except Exception as e:
        print(f"Ngrok error: {e}")
        # Fallback në lokale ose dynamic për të dyja
        ws_public_url = ngrok.connect(8765, proto="http")
        flask_public_url = ngrok.connect(5000, proto="http")

    if ws_public_url:
        ws_host = urlparse(ws_public_url.public_url).hostname
        print(f"WebRTC signaling është në wss://{ws_host} (remote/HTTPS/Netlify)")
    print(f"Ose lokalisht në ws://127.0.0.1:8765")
    print(f"ESP32 mund të thërret: {flask_public_url}/trigger_photo për të shkaktuar ruajtjen e fotos.")

    # Handle Ctrl+C
    signal.signal(signal.SIGINT, lambda sig, frame: shutdown())
    signal.signal(signal.SIGTERM, lambda sig, frame: shutdown())

    # Mbaj programin duke ekzekutuar (wait for threads)
    try:
        flask_thread.join()
        ws_thread.join()
    except KeyboardInterrupt:
        shutdown()

    # Ndalo video capture kur mbaron
    video.stop()
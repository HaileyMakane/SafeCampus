import threading
import os
import numpy as np
import datetime
import time
from tensorflow.keras.models import load_model
from tensorflow.keras.preprocessing import image
import firebase_admin
from firebase_admin import credentials, db
from ultralytics import YOLO

# ---------- CONFIG ----------
MODEL_PATH = "contextual_model.keras"
IMG_SIZE = (224, 224)

FIREBASE_KEY = "sos-bracelet-e9b9b-firebase-adminsdk-fbsvc-6444639360.json"
DATABASE_URL = "https://sos-bracelet-e9b9b-default-rtdb.europe-west1.firebasedatabase.app"

CLASS_NAMES = ['crowd', 'dark', 'isolated', 'normal', 'running']

DEVICE_ID = "ESP32_CAM_01"
FRAME_SOURCE = "frame1"   # simulate ESP32 stream
ALERT_WINDOW_SECONDS = 5
ALERT_EXPIRY_SECONDS = 60  # auto-resolve if not manually acknowledged

# ---------- LOAD MODEL ----------
model = load_model(MODEL_PATH)

YOLO_MODEL_PATH = "yolo_runs/weapon_detector/weights/yolov8n.pt"
YOLO_CONF_THRESHOLD = 0.45   # minimum confidence to trust a detection
YOLO_OVERRIDE_THRESHOLD = 0.70  # above this → treat as strong weapon signal

if not os.path.exists(YOLO_MODEL_PATH):
    raise FileNotFoundError(f"YOLO weights not found at: {YOLO_MODEL_PATH}")

yolo_model = YOLO(YOLO_MODEL_PATH)
print("YOLO weapon model loaded ✅")

# ---------- FIREBASE ----------
try:
    cred = credentials.Certificate(FIREBASE_KEY)
    firebase_admin.initialize_app(cred, {
        'databaseURL': DATABASE_URL
    })
    ref = db.reference("alerts")
    print("Firebase initialized ✅")
except Exception as e:
    print("Firebase init failed ❌:", e)
    ref = None

# ---------- PRIORITY ----------
def assign_priority(level):
    if level == "HIGH":
        return 1
    elif level == "MEDIUM":
        return 2
    else:
        return 3

active_alerts = {}

# ---------- ALERT CREATION ----------
def create_alert():
    alert_data = {
        "device_id": DEVICE_ID,
        "timestamp": datetime.datetime.now().isoformat(),
        "status": "ACTIVE",
        "acknowledged": False,
        "level": "PENDING"
    }

    if ref:
        alert_ref = ref.push(alert_data)
        return alert_ref.key
    else:
        print("[WARN] Firebase not initialized. Generating local alert ID.")
        return f"LOCAL_ALERT_{int(time.time())}"

# ---------- FRAME COLLECTION ----------
def collect_frames(folder, duration=5):
    frames = []
    seen_timestamps = set()
    start_time = time.time()

    for filename in sorted(os.listdir(folder)):
        if not filename.lower().endswith((".jpg", ".png", ".jpeg")):
            continue

        # Extract timestamp from filename (assumes frame_123456.jpg)
        ts = filename.split("_")[-1].split(".")[0]
        if ts in seen_timestamps:
            continue
        seen_timestamps.add(ts)

        frames.append(os.path.join(folder, filename))

        if time.time() - start_time >= duration:
            break

    return frames

# ---------- IMAGE PROCESSING WITH RETRY ----------
def process_image_with_retry(img_path, retries=3):
    for attempt in range(retries):
        try:
            img = image.load_img(img_path, target_size=IMG_SIZE)
            img_array = image.img_to_array(img) / 255.0
            img_array = np.expand_dims(img_array, axis=0)

            preds = model.predict(img_array, verbose=0)[0]
            results = dict(zip(CLASS_NAMES, preds))
            print(f"\nRAW PREDICTIONS → {results}")
            return results

        except Exception as e:
            print(f"[Retry {attempt+1}] failed for {img_path}")

    print(f"[SKIP] Corrupted image: {img_path}")
    return None

# ------------- WEAPON DETECT WITH YOLO -------------
def detect_weapon_yolo(img_path):
    """
    Returns a float 0-1 representing weapon confidence from YOLO.
    Returns None if YOLO can't process the frame.
    Uses the highest-confidence weapon detection in the frame.
    """
    try:
        results = yolo_model(img_path, verbose=False, conf=YOLO_CONF_THRESHOLD)
        detections = results[0].boxes

        if detections is None or len(detections) == 0:
            return 0.0

        confs = detections.conf.cpu().numpy()
        return float(max(confs))   # highest confidence detection

    except Exception as e:
        print(f"[YOLO ERROR] {img_path}: {e}")
        return None

# ---------- SCORING ----------
def compute_score(results, yolo_weapon_conf=None):
    weapon_score = 0.0

    if yolo_weapon_conf is not None:
        if yolo_weapon_conf >= YOLO_OVERRIDE_THRESHOLD:
            weapon_score = min(yolo_weapon_conf * 1.1, 1.0)
        elif yolo_weapon_conf > 0:
            weapon_score = yolo_weapon_conf

    if weapon_score > 0.7:
        if results.get("crowd", 0) > 0.4:
            weapon_score = min(weapon_score * 1.2, 1.0)
        if results.get("isolated", 0) > 0.5:
            weapon_score = min(weapon_score * 1.1, 1.0)

    weights = {
        "weapon": 1.0,
        "running": 0.4,
        "isolated": 0.3,
        "crowd": 0.2,
        "dark": 0.2,
        "normal": -0.15
    }

    score = (
        weapon_score * weights["weapon"] +
        results.get("running", 0) * weights["running"] +
        results.get("isolated", 0) * weights["isolated"] +
        results.get("crowd", 0) * weights["crowd"] +
        results.get("dark", 0) * weights["dark"] +
        results.get("normal", 0) * weights["normal"]
    )

    return max(0, min(float(score), 1))

# ---------- ALERT PROCESSING ----------
def process_alert(frames):
    scores = []
    all_results = []
    yolo_weapon_detections = []

    for frame in frames:
        res = process_image_with_retry(frame)
        if res is None:
            continue

        # Run YOLO weapon check in parallel
        yolo_conf = detect_weapon_yolo(frame)
        yolo_weapon_detections.append(yolo_conf if yolo_conf is not None else 0.0)
        
        if yolo_conf is not None and yolo_conf > YOLO_CONF_THRESHOLD:
            print(f"[YOLO] Weapon detected in {frame} — conf: {yolo_conf:.2f}")

        score = compute_score(res, yolo_weapon_conf=yolo_conf)
        scores.append(score)
        all_results.append(res)

    if not scores:
        return None

    # SMOOTHING (unchanged)
    smoothed = []
    alpha = 0.6
    for i, s in enumerate(scores):
        smoothed.append(s if i == 0 else alpha * s + (1 - alpha) * smoothed[i-1])

    max_score = max(max(scores), max(smoothed))
    avg_score = np.mean(smoothed)

    # Count frames where YOLO confidently detected a weapon
    yolo_weapon_frames = [c for c in yolo_weapon_detections if c >= YOLO_OVERRIDE_THRESHOLD]

    if len(yolo_weapon_frames) >= 2:
        # Multiple strong YOLO detections → definitive
        final_score = max(scores)
    elif max_score > 0.7:
        final_score = 0.6
    else:
        final_score = avg_score

    final_score = max(0, min(final_score, 1))

    if final_score < 0.2:
        level = "LOW"
    elif final_score < 0.7:
        level = "MEDIUM"
    else:
        level = "HIGH"

    return {
        "scores": scores,
        "smoothed": smoothed,
        "max_score": max_score,
        "final_score": final_score,
        "level": level,
        "yolo_weapon_detections": yolo_weapon_detections   # store for Firebase logging
    }

# ---------- UPDATE ALERT ----------
def update_alert(alert_id, result):
    if ref:
        alert_ref = db.reference(f"alerts/{alert_id}")
        update_data = {
            "frame_scores": result["scores"],
            "smoothed_scores": result["smoothed"],
            "max_score": result["max_score"],
            "final_score": result["final_score"],
            "level": result["level"],
            "priority_rank": assign_priority(result["level"]),
            "last_updated": datetime.datetime.now().isoformat(),
            "yolo_weapon_detections": result.get("yolo_weapon_detections", [])
        }
        alert_ref.update(update_data)

# ---------- ALERT WORKER (THREAD) ----------
def alert_worker(alert_id, device_folder):
    print(f"[THREAD STARTED] Alert {alert_id}")
    start_time = time.time()

    while True:
        # Check expiry
        if time.time() - start_time >= ALERT_EXPIRY_SECONDS:
            print(f"[AUTO-RESOLVE] Alert {alert_id} expired")
            if ref:
                db.reference(f"alerts/{alert_id}").update({"status": "RESOLVED"})
            break

        # Fetch alert status
        alert_data = db.reference(f"alerts/{alert_id}").get() if ref else {"status": "ACTIVE"}
        if alert_data.get("status") == "RESOLVED":
            print(f"[THREAD END] Alert {alert_id} resolved")
            break

        frames = collect_frames(device_folder, ALERT_WINDOW_SECONDS)
        if not frames:
            time.sleep(2)
            continue

        result = process_alert(frames)
        if result:
            # Drop low alerts
            update_alert(alert_id, result)  # ✅ ALWAYS update first

            if result["level"] == "LOW":
                print(f"[DROP] Low alert {alert_id}")
                if ref:
                    db.reference(f"alerts/{alert_id}").update({"status": "RESOLVED"})
                break

            if result["level"] == "HIGH":
                print(f"⚠️ HIGH ALERT: {alert_id}")

        time.sleep(2)

# ---------- ALERT TRIGGER ----------
def trigger_alert(device_id, device_folder):
    alert_id = create_alert()
    print(f"[NEW ALERT] {alert_id}")

    thread = threading.Thread(
        target=alert_worker,
        args=(alert_id, device_folder),
        daemon=True
    )
    thread.start()
    active_alerts[alert_id] = thread

# ---------- REMINDER SYSTEM ----------
def check_unacknowledged():
    if not ref:
        return
    alerts = ref.get()
    if not alerts:
        return
    for key, alert in alerts.items():
        if not alert.get("acknowledged", False) and alert.get("status") == "ACTIVE":
            print(f"[REMINDER] Alert {key} not acknowledged")

# ---------- MAIN ----------
print("System running... Waiting for triggers")
DEVICE_FOLDER = FRAME_SOURCE

# Simulate multiple triggers
trigger_alert(DEVICE_ID, DEVICE_FOLDER)
time.sleep(3)
trigger_alert(DEVICE_ID, DEVICE_FOLDER)

# Keep system alive
while True:
    check_unacknowledged()

    # Clean up finished threads
    for alert_id, thread in list(active_alerts.items()):
        if not thread.is_alive():
            del active_alerts[alert_id]

    time.sleep(5)
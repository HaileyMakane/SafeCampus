"""
SOS BRACELET — AI SECURITY DASHBOARD
─────────────────────────────────────────────────────────────
Fixes applied
─────────────────────────────────────────────────────────────
1.  AI OWNS PRIORITY NODE  — update_alert() writes the full
    priority object + root-level ai_status / priority_level /
    priority_rank fields so the dashboard can query easily.
    DevKit no longer sends a priority field; it sends
    ai_status="PENDING" and the AI sets it to "PROCESSED".

2.  MPU TRIGGER OVERRIDE — if trigger == "MPU" (physical
    assault detected by accelerometer), final level is forced
    to HIGH with a minimum score of 0.80, per architecture.

3.  THREAD-SAFE seen_alerts — replaced plain set with a
    set + threading.Lock to prevent race conditions between
    daemon worker threads.

4.  TEMP FILE CLEANUP — every frame downloaded to disk is
    deleted in a finally block after the worker finishes.

5.  CAMERA-OFFLINE FALLBACK — if frames never arrive after
    FRAME_WAIT_TIMEOUT_S, the alert is processed with 0 frames
    and priority is assigned from trigger type alone, so alerts
    are never left stuck as ACTIVE forever.

6.  YOLO FINAL SCORE FIX — when 2+ strong YOLO detections are
    present, final_score now uses the mean YOLO confidence
    (not max CNN composite score which was unrelated).

7.  update_alert() writes to both priority sub-node AND root
    fields for easy Firebase querying.
─────────────────────────────────────────────────────────────
"""

import threading
import os
import shutil
import numpy as np
import datetime
import time
import requests
from urllib.parse import quote
from tensorflow.keras.models import load_model
from tensorflow.keras.preprocessing import image
import firebase_admin
from firebase_admin import credentials, db
from ultralytics import YOLO

# =========================================================
# CONFIG
# =========================================================
MODEL_PATH      = "contextual_model.keras"
YOLO_MODEL_PATH = "yolo_runs/weapon_detector/weights/yolov8n.pt"

IMG_SIZE = (224, 224)

FIREBASE_KEY = "safecampus-4c8a2-firebase-adminsdk-fbsvc-953a25efd6.json"
DATABASE_URL = "https://safecampus-4c8a2-default-rtdb.europe-west1.firebasedatabase.app"
BUCKET_NAME  = "safecampus-4c8a2.firebasestorage.app"

CLASS_NAMES = ["crowd", "dark", "isolated", "normal", "running"]

YOLO_CONF_THRESHOLD     = 0.45
YOLO_OVERRIDE_THRESHOLD = 0.70

ALERT_EXPIRY_SECONDS  = 60
FRAME_WAIT_TIMEOUT_S  = 30   # FIX: camera-offline fallback — give up waiting after this
POLL_INTERVAL         = 2

TEMP_DOWNLOAD_DIR = "temp_cloud_frames"
os.makedirs(TEMP_DOWNLOAD_DIR, exist_ok=True)

# =========================================================
# LOAD MODELS
# =========================================================
cnn_model = load_model(MODEL_PATH)

if not os.path.exists(YOLO_MODEL_PATH):
    raise FileNotFoundError(f"YOLO weights not found: {YOLO_MODEL_PATH}")

yolo_model = YOLO(YOLO_MODEL_PATH)

print("✅ CNN + YOLO models loaded")

# =========================================================
# FIREBASE RTDB
# =========================================================
cred = credentials.Certificate(FIREBASE_KEY)
firebase_admin.initialize_app(cred, {"databaseURL": DATABASE_URL})
alerts_ref = db.reference("active_alerts")

print("✅ Firebase RTDB initialized")

# =========================================================
# THREAD-SAFE SEEN ALERTS
# FIX: plain set replaced with set + Lock to prevent race
#      conditions when multiple worker threads finish and
#      try to add to seen_alerts concurrently.
# =========================================================
_seen_lock  = threading.Lock()
seen_alerts = set()

def mark_seen(alert_id: str) -> None:
    with _seen_lock:
        seen_alerts.add(alert_id)

def is_seen(alert_id: str) -> bool:
    with _seen_lock:
        return alert_id in seen_alerts

# =========================================================
# PRIORITY RANK
# =========================================================
def assign_priority_rank(level: str) -> int:
    return {"HIGH": 1, "MEDIUM": 2, "LOW": 3}.get(level, 3)

# =========================================================
# CLOUD STORAGE — LIST FRAMES
# =========================================================
def list_latest_frames(alert_id: str, max_frames: int = 3) -> list:
    url = (
        f"https://firebasestorage.googleapis.com/v0/b/"
        f"{BUCKET_NAME}/o?prefix={quote(f'frames/{alert_id}/')}"
    )
    try:
        r = requests.get(url, timeout=10)
        if r.status_code != 200:
            print(f"❌ Failed listing frames ({r.status_code}): {r.text[:200]}")
            return []
        items = r.json().get("items", [])
        items.sort(
            key=lambda x: x.get("updated", x.get("timeCreated", "")),
            reverse=True
        )
        return items[:max_frames]
    except Exception as e:
        print(f"❌ Frame listing error: {e}")
        return []

# =========================================================
# CLOUD STORAGE — DOWNLOAD FRAME
# =========================================================
def download_frame(blob_name: str, alert_id: str) -> str | None:
    encoded  = quote(blob_name, safe="")
    url      = (
        f"https://storage.googleapis.com/storage/v1/b/"
        f"{BUCKET_NAME}/o/{encoded}?alt=media"
    )
    # FIX: include alert_id in subdir so cleanup is per-alert
    local_dir = os.path.join(TEMP_DOWNLOAD_DIR, alert_id)
    os.makedirs(local_dir, exist_ok=True)

    local_path = os.path.join(local_dir, os.path.basename(blob_name))

    try:
        r = requests.get(url, timeout=15)
        if r.status_code != 200:
            print(f"❌ Download failed ({r.status_code}): {r.text[:200]}")
            return None
        with open(local_path, "wb") as f:
            f.write(r.content)
        return local_path
    except Exception as e:
        print(f"❌ Download exception: {e}")
        return None

def fetch_latest_cloud_frames(alert_id: str) -> list:
    cloud_items  = list_latest_frames(alert_id)
    local_frames = []
    for item in cloud_items:
        path = download_frame(item["name"], alert_id)
        if path:
            local_frames.append(path)
    return local_frames

# FIX: clean up all temp files for an alert after processing
def cleanup_temp_frames(alert_id: str) -> None:
    local_dir = os.path.join(TEMP_DOWNLOAD_DIR, alert_id)
    if os.path.isdir(local_dir):
        shutil.rmtree(local_dir, ignore_errors=True)
        print(f"🗑️  Cleaned temp frames for {alert_id}")

# =========================================================
# IMAGE PROCESSING — CNN
# =========================================================
def process_image_with_retry(img_path: str, retries: int = 3) -> dict | None:
    for attempt in range(retries):
        try:
            img       = image.load_img(img_path, target_size=IMG_SIZE)
            img_array = image.img_to_array(img) / 255.0
            img_array = np.expand_dims(img_array, axis=0)
            preds     = cnn_model.predict(img_array, verbose=0)[0]
            return dict(zip(CLASS_NAMES, preds))
        except Exception as e:
            print(f"[CNN retry {attempt + 1}] {img_path}: {e}")
    return None

# =========================================================
# WEAPON DETECTION — YOLO
# =========================================================
def detect_weapon_yolo(img_path: str) -> float:
    try:
        results    = yolo_model(img_path, verbose=False, conf=YOLO_CONF_THRESHOLD)
        detections = results[0].boxes
        if detections is None or len(detections) == 0:
            return 0.0
        confs = detections.conf.cpu().numpy()
        return float(max(confs))
    except Exception as e:
        print(f"[YOLO ERROR] {img_path}: {e}")
        return 0.0

# =========================================================
# SCORING — per frame
# =========================================================
def compute_frame_score(cnn_results: dict, yolo_weapon_conf: float = 0.0) -> float:
    if yolo_weapon_conf >= YOLO_OVERRIDE_THRESHOLD:
        weapon_score = min(yolo_weapon_conf * 1.1, 1.0)
    else:
        weapon_score = yolo_weapon_conf

    weights = {
        "weapon":   1.0,
        "running":  0.4,
        "isolated": 0.3,
        "crowd":    0.2,
        "dark":     0.2,
        "normal":  -0.15,
    }

    score = (
        weapon_score                         * weights["weapon"]   +
        cnn_results.get("running",   0)      * weights["running"]  +
        cnn_results.get("isolated",  0)      * weights["isolated"] +
        cnn_results.get("crowd",     0)      * weights["crowd"]    +
        cnn_results.get("dark",      0)      * weights["dark"]     +
        cnn_results.get("normal",    0)      * weights["normal"]
    )
    return max(0.0, min(float(score), 1.0))

# =========================================================
# PROCESS ALERT — run CNN + YOLO across all frames
# =========================================================
def process_alert(frames: list, trigger_type: str = "BUTTON") -> dict | None:
    scores               = []
    smoothed             = []
    yolo_weapon_detections = []
    alpha                = 0.6

    for i, frame in enumerate(frames):
        cnn_result = process_image_with_retry(frame)
        if cnn_result is None:
            continue

        yolo_conf = detect_weapon_yolo(frame)
        yolo_weapon_detections.append(yolo_conf)

        score = compute_frame_score(cnn_result, yolo_conf)
        scores.append(score)

        # Exponential moving average smoothing
        if i == 0:
            smoothed.append(score)
        else:
            smoothed.append(alpha * score + (1 - alpha) * smoothed[-1])

    # ── Handle camera-offline case (no frames processed) ──
    if not scores:
        # No visual data — fall back to trigger type
        # MPU = physical assault → HIGH; BUTTON = unknown → MEDIUM
        fallback_level = "HIGH"   if trigger_type == "MPU" else "MEDIUM"
        fallback_score = 0.85     if trigger_type == "MPU" else 0.50
        return {
            "scores":               [],
            "smoothed":             [],
            "max_score":            fallback_score,
            "final_score":          fallback_score,
            "level":                fallback_level,
            "yolo_weapon_detections": [],
            "no_frames":            True,
        }

    max_score = max(scores)
    avg_score = float(np.mean(smoothed))

    strong_yolo = [c for c in yolo_weapon_detections
                   if c >= YOLO_OVERRIDE_THRESHOLD]

    # ── Final score decision ──────────────────────────────
    if len(strong_yolo) >= 2:
        # FIX: use mean of YOLO confidences, not CNN composite (max_score)
        final_score = float(np.mean(strong_yolo))
    elif max_score > 0.7:
        final_score = max(max_score, avg_score)
    else:
        final_score = avg_score

    final_score = max(0.0, min(final_score, 1.0))

    # ── Level assignment ──────────────────────────────────
    if final_score < 0.2:
        level = "LOW"
    elif final_score < 0.7:
        level = "MEDIUM"
    else:
        level = "HIGH"

    # FIX: MPU (accelerometer = physical assault) → force HIGH
    if trigger_type == "MPU":
        level       = "HIGH"
        final_score = max(final_score, 0.80)

    return {
        "scores":               scores,
        "smoothed":             smoothed,
        "max_score":            max_score,
        "final_score":          final_score,
        "level":                level,
        "yolo_weapon_detections": yolo_weapon_detections,
        "no_frames":            False,
    }

# =========================================================
# UPDATE ALERT IN FIREBASE
# ─────────────────────────────────────────────────────────
# FIX: AI now CREATES the priority node (was only a sub-write).
#      Also writes root-level ai_status / priority_level /
#      priority_rank so the dashboard can query without
#      traversing the nested priority object.
# =========================================================
def update_alert(alert_id: str, result: dict) -> None:
    rank = assign_priority_rank(result["level"])
    now  = datetime.datetime.now().isoformat()

    # ── Create / replace the full priority node ───────────
    db.reference(f"active_alerts/{alert_id}/priority").set({
        "level":                result["level"],
        "score":                round(result["final_score"], 4),
        "rank":                 rank,
        "frame_scores":         [round(s, 4) for s in result["scores"]],
        "smoothed_scores":      [round(s, 4) for s in result["smoothed"]],
        "yolo_detections":      [round(c, 4) for c in result["yolo_weapon_detections"]],
        "no_frames":            result.get("no_frames", False),
        "updated_at":           now,
        "analysis_source":      "CLOUD_STORAGE_AI",
    })

    # ── Update root-level shortcut fields ─────────────────
    db.reference(f"active_alerts/{alert_id}").update({
        "ai_status":      "PROCESSED",     # DevKit sends "PENDING"; AI sets "PROCESSED"
        "priority_level": result["level"],
        "priority_rank":  rank,
    })

# =========================================================
# ALERT WORKER — runs in a daemon thread per alert
# =========================================================
def alert_worker(alert_id: str, trigger_type: str) -> None:
    print(f"🚨 Processing alert {alert_id} | trigger={trigger_type}")
    start = time.time()

    try:
        while True:
            # ── Expiry ────────────────────────────────────
            if time.time() - start > ALERT_EXPIRY_SECONDS:
                print(f"⌛ Alert expired: {alert_id}")
                db.reference(f"active_alerts/{alert_id}").update({
                    "status":    "EXPIRED",
                    "ai_status": "EXPIRED",
                })
                break

            # ── Fetch frames ──────────────────────────────
            frames = fetch_latest_cloud_frames(alert_id)

            if len(frames) < 1:
                # FIX: if camera has been offline for FRAME_WAIT_TIMEOUT_S,
                #      process with 0 frames rather than waiting indefinitely
                elapsed = time.time() - start
                if elapsed > FRAME_WAIT_TIMEOUT_S:
                    print(f"⚠️  No frames after {FRAME_WAIT_TIMEOUT_S}s — processing without camera data")
                else:
                    time.sleep(POLL_INTERVAL)
                    continue

            print(f"📷 {len(frames)} frame(s) available for {alert_id}")

            # ── Run AI ────────────────────────────────────
            result = process_alert(frames, trigger_type=trigger_type)

            if result is None:
                print("⚠️  process_alert returned None — unexpected")
                time.sleep(POLL_INTERVAL)
                continue

            # ── Write priority node (AI creates it here) ──
            update_alert(alert_id, result)

            level = result["level"]
            score = result["final_score"]

            print(f"✅ {alert_id} → {level} (score={score:.2f})")

            # ── Update status and exit ────────────────────
            final_status = "RESOLVED" if level == "LOW" else "PROCESSED"

            db.reference(f"active_alerts/{alert_id}").update({
                "status": final_status,
            })

            if level == "HIGH":
                print(f"🔴 HIGH RISK CONFIRMED — {alert_id}")
            elif level == "MEDIUM":
                print(f"🟡 MEDIUM RISK — {alert_id}")
            else:
                print(f"🟢 LOW RISK — {alert_id}")

            break

    except Exception as e:
        print(f"❌ Worker crashed for {alert_id}: {e}")
        db.reference(f"active_alerts/{alert_id}").update({
            "status":    "AI_ERROR",
            "ai_status": "AI_ERROR",
            "ai_error":  str(e),
        })

    finally:
        # FIX: always clean up temp frames regardless of outcome
        cleanup_temp_frames(alert_id)
        # FIX: thread-safe mark as seen
        mark_seen(alert_id)
        print(f"✅ Worker finished: {alert_id}")

# =========================================================
# MONITOR — polls Firebase for new ACTIVE alerts
# =========================================================
def monitor_new_alerts() -> None:
    print("👀 Monitoring Firebase for new alerts...")
    print(f"🚀 Triggering AI for {'alert_id'}"
          f" ('frames=3/3'),"
          f" trigger={'sos_button'},"
                          f" timeout={'no'})")
    

    # Track alerts currently being handled to avoid double-spawning
    in_progress: set = set()

    while True:
        try:
            alerts = alerts_ref.get() or {}

            for alert_id, alert in alerts.items():
                # Skip already processed or in-progress alerts
                if is_seen(alert_id) or alert_id in in_progress:
                    continue

                status           = alert.get("status", "")
                uploaded         = alert.get("frames_uploaded", 0)
                expected         = alert.get("frames_expected", 0)
                trigger_type     = alert.get("trigger", "BUTTON")
                timestamp        = alert.get("timestamp")

                if status != "ACTIVE":
                    continue

                # ── Condition 1: all frames have arrived ──
                frames_ready = (expected > 0 and uploaded >= expected)

                # ── Condition 2: camera offline / button-only trigger ──
                # If the alert has been ACTIVE for > FRAME_WAIT_TIMEOUT_S
                # and no frames are expected yet, start the worker anyway.
                alert_age = 0
                if timestamp:
                    try:
                        alert_age = time.time() - float(timestamp)
                    except (TypeError, ValueError):
                        pass

                camera_timeout = (
                    not frames_ready and
                    alert_age > FRAME_WAIT_TIMEOUT_S
                )

                if frames_ready or camera_timeout:
                    print(f"🚀 Triggering AI for {alert_id}"
                          f" (frames={uploaded}/{expected},"
                          f" trigger={trigger_type},"
                          f" timeout={'yes' if camera_timeout else 'no'})")

                    in_progress.add(alert_id)

                    t = threading.Thread(
                        target=alert_worker,
                        args=(alert_id, trigger_type),
                        daemon=True,
                    )
                    t.start()

            time.sleep(POLL_INTERVAL)

        except Exception as e:
            print(f"❌ Monitor loop error: {e}")
            time.sleep(5)

# =========================================================
# MAIN
# =========================================================
if __name__ == "__main__":
    monitor_new_alerts()

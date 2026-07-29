# train_yolo.py
from ultralytics import YOLO

model = YOLO("yolov8n.pt")  # nano is fastest for edge deployment

model.train(
    data="data.yaml",       # your dataset config
    epochs=50,
    imgsz=640,
    batch=16,
    name="weapon_detector",
    project="yolo_runs",
    patience=10,            # early stopping
    augment=True
)

# Load best weights explicitly, then export
best_model = YOLO("yolo_runs/weapon_detector/weights/best.pt")
best_model.export(format="pt")
print("Best weights saved to yolo_runs/weapon_detector/weights/best.pt")
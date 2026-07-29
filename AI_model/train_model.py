import tensorflow as tf
from tensorflow.keras import layers, models
from tensorflow.keras.applications import MobileNetV2
from tensorflow.keras.preprocessing.image import ImageDataGenerator

# ---------- CONFIG ----------
DATASET_PATH = "dataset" 
IMG_SIZE = (224, 224)
BATCH_SIZE = 16
EPOCHS = 10

CONTEXTUAL_CLASS_NAMES = ['crowd', 'dark', 'isolated', 'normal', 'running']
NUM_CONTEXTUAL_CLASSES = len(CONTEXTUAL_CLASS_NAMES)

# ---------- DATA AUGMENTATION ----------
train_datagen = ImageDataGenerator(
    rescale=1./255,
    validation_split=0.2,
    rotation_range=20,
    zoom_range=0.2,
    horizontal_flip=True,
    brightness_range=[0.8, 1.2]
)

train_data = train_datagen.flow_from_directory(
    DATASET_PATH,
    target_size=IMG_SIZE,
    batch_size=BATCH_SIZE,
    class_mode='categorical',
    subset='training',
    classes=CONTEXTUAL_CLASS_NAMES
)

val_data = train_datagen.flow_from_directory(
    DATASET_PATH,
    target_size=IMG_SIZE,
    batch_size=BATCH_SIZE,
    class_mode='categorical',
    subset='validation',
    classes=CONTEXTUAL_CLASS_NAMES
)

print("Contextual Classes:", train_data.class_indices)

# ---------- BASE MODEL ----------
base_model = MobileNetV2(
    input_shape=(224, 224, 3),
    include_top=False,
    weights='imagenet'
)

# Freeze most layers
for layer in base_model.layers:
    layer.trainable = False

# ---------- CUSTOM HEAD ----------
x = base_model.output
x = layers.GlobalAveragePooling2D()(x)

# Dropout to reduce overfitting
x = layers.Dropout(0.5)(x)

# Output layer for contextual classification
predictions = layers.Dense(NUM_CONTEXTUAL_CLASSES, activation='softmax')(x)

model = models.Model(inputs=base_model.input, outputs=predictions)

# ---------- COMPILE ----------
model.compile(
    optimizer='adam',
    loss='categorical_crossentropy',
    metrics=['accuracy']
)

# ---------- TRAIN ----------
history = model.fit(
    train_data,
    validation_data=val_data,
    epochs=EPOCHS
)

# ---------- SAVE MODEL ----------
model.save("contextual_model.keras")

print("Contextual model training complete.")

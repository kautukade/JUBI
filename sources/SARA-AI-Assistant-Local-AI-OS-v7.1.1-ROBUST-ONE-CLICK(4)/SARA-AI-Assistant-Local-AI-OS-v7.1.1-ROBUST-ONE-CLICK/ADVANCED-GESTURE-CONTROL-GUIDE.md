# SARA v7 Advanced Gesture Control

## Two separate modes

- **ORB HAND:** webcam gestures control only the dashboard holographic orb.
- **REAL CURSOR:** webcam landmarks are sent to the authenticated local Agent and control the Windows pointer through PyAutoGUI.

## Gesture map

| Gesture | Action |
|---|---|
| Index finger | Move Windows cursor |
| Thumb + index quick pinch | Left click |
| Two quick index pinches | Double click |
| Thumb + index hold/move | Drag and drop |
| Index + middle fingers | Scroll |
| Thumb + middle pinch | Right click |
| Thumb + ring pinch | Screenshot |
| Thumb + pinky pinch | Show Desktop |
| Index + pinky | Alt+Tab |
| Thumb up/down | Volume up/down |
| Three fingers | Media play/pause |
| Open palm | Neutral; no pause or stop |
| Closed fist | Neutral; no emergency action |

## Accidental-action prevention

- confidence threshold
- smoothing
- cooldown
- drag hold threshold
- screen-edge margin
- dominant-hand/mirror calibration
- authenticated expiring gesture session

## Safety controls outside gestures

Gesture emergency detection is intentionally removed. Manual controls remain:

- `Ctrl + Alt + Shift + S`
- Dashboard Mission Control `STOP ALL`
- System tray `Emergency Stop`
- Voice command `SARA pause control`

## Requirements

- Camera permission
- Local Agent online
- `public\mediapipe\hand_landmarker.task`
- local MediaPipe WASM
- Windows session unlocked
- Real Cursor session explicitly enabled

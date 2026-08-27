#!/usr/bin/env python3
"""Patch external/SDL for Android right/middle mouse button support (idempotent)."""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SDL = REPO / "external" / "SDL"
MOUSE_C = SDL / "src" / "video" / "android" / "SDL_androidmouse.c"
SURFACE_JAVA = SDL / "android-project" / "app" / "src" / "main" / "java" / "org" / "libsdl" / "app" / "SDLSurface.java"
CONTROLLER_JAVA = SDL / "android-project" / "app" / "src" / "main" / "java" / "org" / "libsdl" / "app" / "SDLControllerManager.java"


def patch_text(path, old, new, label):
    text = path.read_text(encoding="utf-8")
    if new in text:
        print(label + ": already patched; skipping.")
        return False
    if old not in text:
        print(label + ": ANCHOR NOT FOUND, aborting!", file=sys.stderr)
        sys.exit(1)
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(label + ": applied.")
    return True


def main():
    c_text = MOUSE_C.read_text(encoding="utf-8")
    if "#define ACTION_BUTTON_PRESS   11" not in c_text:
        old_defs = "#define ACTION_SCROLL     8"
        new_defs = old_defs + "\n#define ACTION_BUTTON_PRESS   11\n#define ACTION_BUTTON_RELEASE 12"
        if old_defs not in c_text:
            print("CRITICAL: ACTION_SCROLL define anchor missing", file=sys.stderr)
            return 1
        c_text = c_text.replace(old_defs, new_defs)
        print("SDL_androidmouse.c: defines applied.")

    old_sw = "    case ACTION_MOVE:\n    case ACTION_HOVER_MOVE:\n        SDL_SendMouseMotion(window, 0, relative, (int)x, (int)y);\n        break;\n"
    new_sw = old_sw + ("    case ACTION_BUTTON_PRESS:\n        changes = state & ~last_state;\n"
                       "        button = TranslateButton(changes);\n        last_state = state;\n"
                       "        SDL_SendMouseMotion(window, 0, relative, (int)x, (int)y);\n"
                       "        SDL_SendMouseButton(window, 0, SDL_PRESSED, button);\n        break;\n\n"
                       "    case ACTION_BUTTON_RELEASE:\n        changes = last_state & ~state;\n"
                       "        button = TranslateButton(changes);\n        last_state = state;\n"
                       "        SDL_SendMouseMotion(window, 0, relative, (int)x, (int)y);\n"
                       "        SDL_SendMouseButton(window, 0, SDL_RELEASED, button);\n        break;\n")
    if "case ACTION_BUTTON_PRESS:" not in c_text:
        if old_sw not in c_text:
            print("CRITICAL: ACTION_MOVE switch anchor missing", file=sys.stderr)
            return 1
        c_text = c_text.replace(old_sw, new_sw)
        print("SDL_androidmouse.c: switch branches applied.")
    MOUSE_C.write_text(c_text, encoding="utf-8")

    s_old = "            SDLActivity.onNativeMouse(mouseButton, action, x, y, motionListener.inRelativeMode());\n        } else {"
    s_mid = ("            // Secondary (right) and tertiary (middle) clicks arrive as\n"
             "            // ACTION_BUTTON_PRESS / ACTION_BUTTON_RELEASE, which the native\n"
             "            // Android_OnMouse switch does not handle. Normalise to the\n"
             "            // ACTION_DOWN / ACTION_UP values the C side expects.\n"
             "            if (action == MotionEvent.ACTION_BUTTON_PRESS || action == MotionEvent.ACTION_BUTTON_RELEASE) {\n"
             "                action = (action == MotionEvent.ACTION_BUTTON_PRESS) ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;\n"
             "            }\n\n"
             "            SDLActivity.onNativeMouse(mouseButton, action, x, y, motionListener.inRelativeMode());\n        } else {")
    patch_text(SURFACE_JAVA, s_old, s_mid, "SDLSurface.java")

    c_old = "                        SDLActivity.onNativeMouse(0, action, x, y, false);\n                        return true;\n\n                    default:"
    c_new = ("                        SDLActivity.onNativeMouse(0, action, x, y, false);\n                        return true;\n\n"
             "                    case MotionEvent.ACTION_BUTTON_PRESS:\n                    case MotionEvent.ACTION_BUTTON_RELEASE:\n"
             "                        x = event.getX(0);\n                        y = event.getY(0);\n"
             "                        int nAct = (action == MotionEvent.ACTION_BUTTON_PRESS) ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;\n"
             "                        SDLActivity.onNativeMouse(event.getButtonState(), nAct, x, y, false);\n                        return true;\n\n"
             "                    default:")
    patch_text(CONTROLLER_JAVA, c_old, c_new, "SDLControllerManager.java")
    return 0


if __name__ == "__main__":
    sys.exit(main())

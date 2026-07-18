# Save / Load HD UI

This directory contains the production assets for `LoadSaveWindowHD.tjs`.
The original `frame/FRM_03xx` resources and `SystemWindow.tjs` remain intact.

## Source mapping

| Production name | Reference source | Purpose |
| --- | --- | --- |
| `background_save.png` | `CDDQ-23.png` | 1920×1080 save background |
| `background_load.png` | `CDDQ-24.png` | 1920×1080 load background |
| `button_return_game_*.png` | `CDDQ-02/09.png` | Return to game |
| `button_return_title_*.png` | `CDDQ-03/08.png` | Return to title |
| `button_move_*.png` | `CDDQ-04/10.png` | Move mode |
| `button_settings_*.png` | `CDDQ-05/11.png` | Settings |
| `button_delete_*.png` | `CDDQ-06/12.png` | Delete mode |
| `button_copy_*.png` | `CDDQ-07/13.png` | Copy mode |
| `page_*.png` | `CDDQ-14..19.png` | Page navigation |
| `scroll_thumb_*.png` | `CDDQ-20/21.png` | Scroll position indicator |
| `slot_hover.png` | `CDDQ-22.png` | Slot hover / selection overlay |

The two backgrounds were center-cropped from 1922×1082 to the project's
1920×1080 UI coordinate space. Small two-state buttons were normalized to an
even 300×64 sprite sheet where their source width was uneven.

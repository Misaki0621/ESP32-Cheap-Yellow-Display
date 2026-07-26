# LVGL Migration Issues & Fixes

STOCK_MONI project migrated from hand-rendered UI (LCD + GUI + XPT2046 components) to LVGL v9.5.0.
This document summarizes all issues encountered and their solutions.

---

## 0. Architecture Change

**Before:** Custom pixel-level rendering via `LCD_DrawFillRectangle()`, `LCD_ShowString()`, bit-banged touch polling.

**After:** LVGL widgets (`lv_button`, `lv_keyboard`, `lv_textarea`, `lv_label`) with `esp_lvgl_port` integration.

| Component | Old | New |
|---|---|---|
| Display init | `components/LCD/lcd.c` (custom ILI9341 driver) | `main/lcd.c` (ESP-IDF `esp_lcd_ili9341` + LVGL port) |
| Touch | `components/XPT2046/xpt2046.c` (bit-banged) | `main/touch.c` (hardware SPI via `esp_lcd_touch_xpt2046`) |
| Pin config | Hardcoded in component headers | `main/hardware.h` (single source) |
| Dependencies | Manual component dirs | `main/idf_component.yml` (auto-fetched: lvgl, esp_lvgl_port, ili9341, xpt2046) |

---

## 1. Landscape → Portrait Orientation

**Symptom:** UI layout designed for 320px width overflowed when switched to portrait (240px wide).

**Fix:**
- Changed `LV_DISPLAY_ROTATION_90` → `LV_DISPLAY_ROTATION_0` in `app_main.c`
- Reduced all widget widths from ~280-300px to ~220-230px
- `hardware.h` already had `LCD_H_RES=240, LCD_V_RES=320` (native portrait)

**Files:**
- `main/hardware.h` — unchanged
- `main/app_main.c` — 8 width values adjusted

---

## 2. Task Watchdog Timeout (IDLE0 not fed)

**Symptom:**
```
E (5993) task_wdt: Task watchdog got triggered - IDLE0 (CPU 0)
E (5993) task_wdt: CPU 0: main
```

**Root cause:** `wifi_scan_get_results()` called `xEventGroupWaitBits(..., 10000ms)` while holding `lvgl_port_lock`, blocking the entire main loop from yielding to IDLE.

**Fix:**
- Removed blocking `xEventGroupWaitBits` from `wifi_scan_get_results()` — main loop already confirms scan completion upstream
- Removed blocking `xEventGroupWaitBits` from connect result checks
- Disabled Task Watchdog entirely: `CONFIG_ESP_TASK_WDT=n` in `sdkconfig.defaults`

**Files:**
- `main/app_main.c` — `wifi_scan_get_results()` de-blocked
- `sdkconfig.defaults` — watchdog disabled

---

## 3. Main Loop Restructuring (3-Phase Pattern)

**Symptom:** WiFi event checks were inside `if (lvgl_port_lock(0))` — when LVGL render task held the lock, all WiFi logic was skipped.

**Fix:** Split main loop into 3 phases:

```
while (1) {
    vTaskDelay(30ms);

    // Phase 1 (no lock): Check WiFi event bits, set boolean flags
    // Phase 2 (with lock, 5000ms timeout): Apply flagged UI changes
    // Phase 3 (with lock, 0ms timeout): Detect page switches
}
```

**Key insight:** EventGroup operations don't need the LVGL lock. Only LVGL API calls do.

**File:** `main/app_main.c`

---

## 4. `lvgl_port_unlock()` + `vTaskDelay()` + `lvgl_port_lock()` → Crash

**Symptom:**
```
Guru Meditation Error: Core 0 panic'ed (StoreProhibited)
PC: vPortYieldFromInt
EXCVADDR: 0xffffffa0
```

**Root cause:** In `populate_wifi_list()`, releasing the lock mid-loop allowed LVGL render task to operate on a half-built object tree:

```c
// BROKEN pattern:
lvgl_port_unlock();          // Render task grabs lock
vTaskDelay(5);                // Render task accesses half-deleted objects
lvgl_port_lock(-1);           // Main task re-acquires lock → corrupted state
```

**Fix:** Never release the LVGL lock during object tree mutations. Use a single atomic pass.

```c
// CORRECT: all mutations in one locked pass
lvgl_port_lock(-1);
lv_obj_clean(screen);
// ... create all objects ...
lvgl_port_unlock();
```

---

## 5. `vTaskDelay(1)` While Holding Lock → `InstrFetchProhibited`

**Symptom:**
```
Guru Meditation Error: Core 0 panic'ed (InstrFetchProhibited)
PC: draw_letter_cb (font rendering)
EXCVADDR: 0x8010aaf4
```

**Root cause:** `vTaskDelay(1)` inside the lock yielded CPU without releasing the lock. LVGL internal timers/events queued during the yield were then processed in an inconsistent state.

**Fix:** Remove ALL yield/delay calls from LVGL-locked code paths. Object mutations must be lock-atomic.

---

## 6. Dynamic Object Creation → Render Race

**Symptom:** Same `InstrFetchProhibited` crash in `draw_letter_cb` even after removing yield calls.

**Root cause:** `populate_wifi_list()` called `lv_obj_clean()` to destroy all children, then re-created 20+ objects. The flex layout engine computed positions during the render cycle, but object tree state was inconsistent with layout cache.

**Fix:** Pre-create all UI objects at initialization, toggle visibility only at runtime:

```c
// create_wifi_list_screen(): pre-create title, hint, fail label, container, 10 buttons + labels
// populate_wifi_list():   only update lv_label_set_text() + lv_obj_clear/add_flag(HIDDEN)
// show_scan_fail():       only toggle visibility of hint/fail/container
```

**No `lv_obj_clean()`, `lv_obj_delete()`, or `lv_obj_create()` at runtime.**

**File:** `main/app_main.c`

---

## 7. LVGL Stack Overflow

**Symptom:**
```
***ERROR*** A stack overflow in task has been detected
vApplicationStackOverflowHook → vTaskSwitchContext
```

**Root cause:** LVGL render task stack was 4096 bytes — too small for flex layout computing 34+ nested objects.

**Fix:** Increased LVGL task stack from 4096 → **8192** bytes.

**File:** `main/lcd.c` line 143

---

## 8. LVGL SysMon / PerfMon Lock Contention

**Symptom:** Screen stuck on "Scanning WiFi..." — `lvgl_port_lock(200ms)` timed out hundreds of times because LVGL render task (prio=4) continuously held the lock rendering performance monitors.

**Root cause:** `CONFIG_LV_USE_SYSMON=y` and `CONFIG_LV_USE_PERF_MONITOR=y` caused the render task to refresh every frame, starving the main task (prio=1) from acquiring the lock.

**Fix:** Disabled both in `sdkconfig.defaults`:
```diff
- CONFIG_LV_USE_SYSMON=y
- CONFIG_LV_USE_PERF_MONITOR=y
```

**File:** `sdkconfig.defaults`

---

## 9. English UI & Log Messages

**Change:** All 8 screen-visible strings and 20+ ESP_LOGI messages translated from Chinese to English.

**File:** `main/app_main.c`

---

## 10. ESP-IDF v6.0 API Incompatibilities

During migration from the LVGL9 example (written for newer ESP-IDF):

| Issue | Fix |
|---|---|
| `esp_event_handler_register` expects 4 args, not 5 | Removed `instance` parameter |
| `driver/ledc.h` not found | Added `esp_driver_ledc` to REQUIRES |
| `esp_lcd_panel_dev_config_t` has no `color_space` | Removed `.color_space` field |
| Binary size exceeds 1MB partition | Switched to `partitions_singleapp_large.csv` (1500K) |

---

## Key Takeaways

1. **LVGL mutations must be lock-atomic.** Never yield, delay, or release the lock mid-mutation.
2. **Pre-allocate widgets.** Create all objects at startup, update text/visibility at runtime.
3. **No blocking inside `lvgl_port_lock`.** EventGroup checks, network calls, etc. belong in Phase 1 (no lock).
4. **Stack size matters.** LVGL + flex layout + 30+ objects needs at least 8KB render task stack.
5. **Performance monitors are expensive.** `LV_USE_SYSMON` and `LV_USE_PERF_MONITOR` can starve user tasks.
6. **ESP-IDF v6.0 has API differences** from the versions used in the upstream LVGL9 example.

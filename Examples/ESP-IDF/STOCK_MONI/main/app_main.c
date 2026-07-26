/* ========================================================================
 *  STOCK_MONI — ESP32 CYD WiFi 股票监视器 (LVGL版)
 *  使用 LVGL 原生控件(按钮/键盘/文本域)替代手绘渲染
 * ======================================================================== */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "lcd.h"
#include "touch.h"

static const char *TAG = "stock_moni";

/* ============================ LED GPIO ================================== */
#define LED_RED_GPIO    4
#define LED_GREEN_GPIO  16
#define LED_BLUE_GPIO   17

/* ============================ WiFi 事件位 =============================== */
#define WIFI_SCAN_DONE_BIT   BIT0
#define WIFI_CONNECTED_BIT   BIT1
#define WIFI_AUTH_FAIL_BIT   BIT2

/* ============================ 全局状态 ================================== */
static EventGroupHandle_t g_wifi_events = NULL;
static wifi_ap_record_t   g_ap_list[10];
static uint16_t           g_ap_count = 0;
static uint32_t           g_connect_tick = 0;
static bool               g_scan_failed = false;
static uint32_t           g_scan_start_tick = 0;
static int                g_selected_ap = -1;
static char               g_password[64] = "";

/* ============================ LVGL 屏幕 ================================== */
static lv_obj_t *scr_wifi_list = NULL;
static lv_obj_t *scr_password  = NULL;
static lv_obj_t *scr_connecting = NULL;
static lv_obj_t *scr_success   = NULL;
static lv_obj_t *scr_failure   = NULL;

/* 密码页控件(需要在回调中引用) */
static lv_obj_t *ta_pwd = NULL;
static lv_obj_t *kb_pwd = NULL;
static lv_obj_t *label_ssid = NULL;

/* 失败页控件 */
static lv_obj_t *ta_retry = NULL;
static lv_obj_t *kb_retry = NULL;

/* WiFi 列表页控件(预创建) */
static lv_obj_t *wifi_btns[10];
static lv_obj_t *wifi_btn_labels[10];
static lv_obj_t *wifi_container = NULL;
static lv_obj_t *wifi_hint = NULL;
static lv_obj_t *wifi_fail = NULL;
static lv_obj_t *wifi_title = NULL;

/* ============================ LED ======================================== */
static void led_init(void)
{
	gpio_set_direction(LED_RED_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(LED_GREEN_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(LED_BLUE_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(LED_RED_GPIO, 1);
	gpio_set_level(LED_GREEN_GPIO, 1);
	gpio_set_level(LED_BLUE_GPIO, 1);
}

static void led_set_green(void) {
	gpio_set_level(LED_RED_GPIO, 1); gpio_set_level(LED_GREEN_GPIO, 0); gpio_set_level(LED_BLUE_GPIO, 1);
}
static void led_set_red(void) {
	gpio_set_level(LED_RED_GPIO, 0); gpio_set_level(LED_GREEN_GPIO, 1); gpio_set_level(LED_BLUE_GPIO, 1);
}

/* ============================ WiFi 事件处理 ============================== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
			       int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
		ESP_LOGI(TAG, "[WiFi] scan done");
		xEventGroupSetBits(g_wifi_events, WIFI_SCAN_DONE_BIT);
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *d = event_data;
		ESP_LOGI(TAG, "[WiFi] disconnected, reason=%d", d->reason);
		if (d->reason == WIFI_REASON_AUTH_FAIL ||
		    d->reason == WIFI_REASON_NO_AP_FOUND ||
		    d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
			xEventGroupSetBits(g_wifi_events, WIFI_AUTH_FAIL_BIT);
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *ip = event_data;
		ESP_LOGI(TAG, "[WiFi] IP: " IPSTR, IP2STR(&ip->ip_info.ip));
		xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
	}
}

static void wifi_init(void)
{
	g_wifi_events = xEventGroupCreate();
	esp_netif_init();
	esp_event_loop_create_default();
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_wifi_init(&cfg);
	esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_start();
	ESP_LOGI(TAG, "WiFi initialized");
}

static void wifi_scan_start(void)
{
	ESP_LOGI(TAG, "Starting WiFi scan...");
	xEventGroupClearBits(g_wifi_events, WIFI_SCAN_DONE_BIT);
	wifi_scan_config_t scan_cfg = { .scan_type = WIFI_SCAN_TYPE_ACTIVE };
	esp_wifi_scan_start(&scan_cfg, false);
}

static uint16_t wifi_scan_get_results(void)
{

	uint16_t num = 0;
	esp_wifi_scan_get_ap_num(&num);
	if (num == 0) return 0;

	wifi_ap_record_t *all = calloc(num, sizeof(wifi_ap_record_t));
	if (!all) return 0;
	esp_wifi_scan_get_ap_records(&num, all);

	/* 按 RSSI 降序冒泡排序 */
	for (uint16_t i = 0; i < num; i++)
		for (uint16_t j = i + 1; j < num; j++)
			if (all[i].rssi < all[j].rssi) {
				wifi_ap_record_t t = all[i]; all[i] = all[j]; all[j] = t;
			}
	uint16_t take = num > 10 ? 10 : num;
	memcpy(g_ap_list, all, take * sizeof(wifi_ap_record_t));
	g_ap_count = take;
	free(all);

	for (uint16_t i = 0; i < take; i++)
		ESP_LOGI(TAG, "  [%d] SSID=%-24s RSSI=%d",
			 i, (char *)g_ap_list[i].ssid, g_ap_list[i].rssi);
	return take;
}

static void wifi_try_connect(const char *ssid, const char *password)
{
	ESP_LOGI(TAG, "Connecting SSID=%s PWD=%s", ssid, password);
	xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT | WIFI_AUTH_FAIL_BIT);
	esp_wifi_disconnect();
	vTaskDelay(pdMS_TO_TICKS(500));

	wifi_config_t cfg = {0};
	strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
	strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
	cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	esp_wifi_set_config(WIFI_IF_STA, &cfg);
	esp_wifi_connect();
	g_connect_tick = xTaskGetTickCount();
}

/* ============================ LVGL 回调 ================================== */

/* WiFi 列表页: 点击某个 AP 按钮 */
static void on_wifi_btn_click(lv_event_t *e)
{
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	ESP_LOGI(TAG, "WiFi button[%d] clicked: %s", idx, (char *)g_ap_list[idx].ssid);
	g_selected_ap = idx;
	g_password[0] = '\0';

	/* 更新密码页的 SSID 副标题 */
	lv_label_set_text_fmt(label_ssid, "Connect to: %s", (char *)g_ap_list[idx].ssid);
	lv_textarea_set_text(ta_pwd, "");

	lv_scr_load(scr_password);
	ESP_LOGI(TAG, "Switching to password page");
}

/* 密码页 / 失败页: Textarea 获得焦点时弹出键盘 */
static void on_ta_focused(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target(e);
	lv_obj_t *kb = lv_event_get_user_data(e);

	/* 键盘内容超出屏幕底部时, 将 textarea 往上移 */
	uint16_t h = lv_display_get_vertical_resolution(lv_obj_get_display(ta));
	if (lv_obj_get_y(ta) > h / 2) {
		lv_obj_set_y(ta, 4);
	}
	lv_keyboard_set_textarea(kb, ta);
	lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
	ESP_LOGI(TAG, "Keyboard shown");
}

/* 密码/重试 textarea: 键盘按下 OK */
static void on_ta_ready(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_target(e);
	const char *pwd = lv_textarea_get_text(ta);
	ESP_LOGI(TAG, "Keyboard OK: password=%s", pwd);
	strncpy(g_password, pwd, sizeof(g_password) - 1);

	if (g_selected_ap >= 0 && g_selected_ap < g_ap_count) {
		wifi_try_connect((char *)g_ap_list[g_selected_ap].ssid, g_password);
	} else {
		ESP_LOGW(TAG, "No AP selected");
	}
	lv_scr_load(scr_connecting);
	ESP_LOGI(TAG, "Switching to connecting page");
}

/* 密码/重试 textarea: 键盘按下取消 */
static void on_ta_cancel(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_user_data(e);
	lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
	ESP_LOGI(TAG, "Keyboard hidden");
}

/* ============================ 页面创建函数 ============================== */

/*
 * 创建 WiFi 列表页 — 预创建所有对象, 运行时只改文字/可见性
 */
static void create_wifi_list_screen(void)
{
	scr_wifi_list = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_wifi_list, lv_color_black(), LV_STATE_DEFAULT);

	wifi_title = lv_label_create(scr_wifi_list);
	lv_label_set_text(wifi_title, "WiFi Networks");
	lv_obj_set_style_text_color(wifi_title, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_align(wifi_title, LV_ALIGN_TOP_MID, 0, 4);

	wifi_hint = lv_label_create(scr_wifi_list);
	lv_label_set_text(wifi_hint, "Scanning WiFi...");
	lv_obj_set_style_text_color(wifi_hint, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_align(wifi_hint, LV_ALIGN_CENTER, 0, 0);

	wifi_fail = lv_label_create(scr_wifi_list);
	lv_label_set_text(wifi_fail, "WIFI SCAN FAIL");
	lv_obj_set_style_text_color(wifi_fail, lv_color_hex(0xFF0000), LV_STATE_DEFAULT);
	lv_obj_align(wifi_fail, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_flag(wifi_fail, LV_OBJ_FLAG_HIDDEN);  /* 初始隐藏 */

	/* 容器(初始隐藏) */
	wifi_container = lv_obj_create(scr_wifi_list);
	lv_obj_set_size(wifi_container, 230, 270);
	lv_obj_align(wifi_container, LV_ALIGN_TOP_MID, 0, 24);
	lv_obj_set_style_bg_color(wifi_container, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(wifi_container, 0, LV_STATE_DEFAULT);
	lv_obj_set_flex_flow(wifi_container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(wifi_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);

	/* 预创建 10 个按钮(初始隐藏) */
	for (int i = 0; i < 10; i++) {
		lv_obj_t *btn = lv_button_create(wifi_container);
		lv_obj_set_size(btn, 220, 28);
		lv_obj_set_style_bg_color(btn, lv_color_white(), LV_STATE_DEFAULT);
		lv_obj_set_style_shadow_width(btn, 4, LV_STATE_DEFAULT);
		lv_obj_set_style_shadow_ofs_x(btn, 2, LV_STATE_DEFAULT);
		lv_obj_set_style_shadow_ofs_y(btn, 2, LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(btn, lv_color_hex(0xCCCCCC), LV_STATE_PRESSED);
		lv_obj_set_style_shadow_width(btn, 0, LV_STATE_PRESSED);
		lv_obj_add_event_cb(btn, on_wifi_btn_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);

		lv_obj_t *label = lv_label_create(btn);
		lv_obj_set_style_text_color(label, lv_color_black(), LV_STATE_DEFAULT);
		lv_obj_center(label);

		wifi_btns[i] = btn;
		wifi_btn_labels[i] = label;
	}
}

/*
 * 用扫描结果填充 WiFi 列表 — 只更新文字和可见性, 不增删对象
 */
static void populate_wifi_list(void)
{
	lv_obj_add_flag(wifi_hint, LV_OBJ_FLAG_HIDDEN);      /* 隐藏提示 */
	lv_obj_add_flag(wifi_fail, LV_OBJ_FLAG_HIDDEN);       /* 隐藏失败 */
	lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_HIDDEN); /* 显示容器 */

	for (int i = 0; i < 10; i++) {
		if (i < g_ap_count) {
			char buf[48];
			snprintf(buf, sizeof(buf), "%-24s  %d dBm",
				 (char *)g_ap_list[i].ssid, g_ap_list[i].rssi);
			lv_label_set_text(wifi_btn_labels[i], buf);
			lv_obj_clear_flag(wifi_btns[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(wifi_btns[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
	ESP_LOGI(TAG, "WiFi list populated: %d APs", g_ap_count);
}

/* 显示扫描失败 — 只切换可见性 */
static void show_scan_fail(void)
{
	lv_obj_add_flag(wifi_hint, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clear_flag(wifi_fail, LV_OBJ_FLAG_HIDDEN);
	ESP_LOGE(TAG, "WiFi scan failed!");
}

/*
 * 创建密码输入页
 *   scr_password: 标题 + SSID副标题 + 文本域 + 键盘(初始隐藏)
 */
static void create_password_screen(void)
{
	scr_password = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_password, lv_color_black(), LV_STATE_DEFAULT);

	lv_obj_t *title = lv_label_create(scr_password);
	lv_label_set_text(title, "PASSWORD");
	lv_obj_set_style_text_color(title, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

	label_ssid = lv_label_create(scr_password);
	lv_label_set_text(label_ssid, "Connect to: --");
	lv_obj_set_style_text_color(label_ssid, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_align(label_ssid, LV_ALIGN_TOP_MID, 0, 30);

	/* 文本域(明文) */
	ta_pwd = lv_textarea_create(scr_password);
	lv_textarea_set_one_line(ta_pwd, true);
	lv_textarea_set_max_length(ta_pwd, 63);
	lv_obj_set_size(ta_pwd, 220, 36);
	lv_obj_align(ta_pwd, LV_ALIGN_TOP_MID, 0, 56);
	lv_obj_set_style_bg_color(ta_pwd, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(ta_pwd, lv_color_black(), LV_STATE_DEFAULT);

	/* 键盘(初始隐藏) */
	kb_pwd = lv_keyboard_create(scr_password);
	lv_obj_set_size(kb_pwd, 230, 150);
	lv_obj_align(kb_pwd, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_add_flag(kb_pwd, LV_OBJ_FLAG_HIDDEN);

	/* textarea 获取焦点时弹出键盘 */
	lv_obj_add_event_cb(ta_pwd, on_ta_focused, LV_EVENT_FOCUSED, kb_pwd);
	lv_obj_add_event_cb(ta_pwd, on_ta_ready, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(ta_pwd, on_ta_cancel, LV_EVENT_CANCEL, kb_pwd);

	ESP_LOGI(TAG, "Password page created");
}

/*
 * 创建连接中页面
 */
static void create_connecting_screen(void)
{
	scr_connecting = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_connecting, lv_color_black(), LV_STATE_DEFAULT);

	lv_obj_t *label = lv_label_create(scr_connecting);
	lv_label_set_text(label, "Connecting...");
	lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

/*
 * 创建成功页面
 */
static void create_success_screen(void)
{
	scr_success = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_success, lv_color_black(), LV_STATE_DEFAULT);

	lv_obj_t *check = lv_label_create(scr_success);
	lv_label_set_text(check, LV_SYMBOL_OK);
	lv_obj_set_style_text_color(check, lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
	lv_obj_align(check, LV_ALIGN_CENTER, 0, -20);

	lv_obj_t *text = lv_label_create(scr_success);
	lv_label_set_text(text, "Connected!");
	lv_obj_set_style_text_color(text, lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
	lv_obj_align(text, LV_ALIGN_CENTER, 0, 40);
}

/*
 * 创建失败+重试页面
 */
static void create_failure_screen(void)
{
	scr_failure = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_failure, lv_color_black(), LV_STATE_DEFAULT);

	lv_obj_t *cross = lv_label_create(scr_failure);
	lv_label_set_text(cross, LV_SYMBOL_CLOSE);
	lv_obj_set_style_text_color(cross, lv_color_hex(0xFF0000), LV_STATE_DEFAULT);
	lv_obj_align(cross, LV_ALIGN_CENTER, 0, -40);

	lv_obj_t *text = lv_label_create(scr_failure);
	lv_label_set_text(text, "Wrong Password");
	lv_obj_set_style_text_color(text, lv_color_hex(0xFF0000), LV_STATE_DEFAULT);
	lv_obj_align(text, LV_ALIGN_CENTER, 0, 10);

	/* 重试文本域 */
	ta_retry = lv_textarea_create(scr_failure);
	lv_textarea_set_one_line(ta_retry, true);
	lv_textarea_set_max_length(ta_retry, 63);
	lv_obj_set_size(ta_retry, 220, 36);
	lv_obj_align(ta_retry, LV_ALIGN_BOTTOM_MID, 0, -155);
	lv_obj_set_style_bg_color(ta_retry, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(ta_retry, lv_color_black(), LV_STATE_DEFAULT);

	/* 重试键盘 */
	kb_retry = lv_keyboard_create(scr_failure);
	lv_obj_set_size(kb_retry, 230, 150);
	lv_obj_align(kb_retry, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_add_flag(kb_retry, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(ta_retry, on_ta_focused, LV_EVENT_FOCUSED, kb_retry);
	lv_obj_add_event_cb(ta_retry, on_ta_ready, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(ta_retry, on_ta_cancel, LV_EVENT_CANCEL, kb_retry);

	ESP_LOGI(TAG, "Failure/retry page created");
}

/* ============================ 主函数 ==================================== */

void app_main(void)
{
	/* ---- NVS ---- */
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	/* ---- LED ---- */
	led_init();

	/* ---- LCD + LVGL ---- */
	esp_lcd_panel_io_handle_t lcd_io;
	esp_lcd_panel_handle_t lcd_panel;
	esp_lcd_touch_handle_t tp;

	ESP_ERROR_CHECK(lcd_display_brightness_init());
	ESP_ERROR_CHECK(app_lcd_init(&lcd_io, &lcd_panel));
	lv_display_t *lvgl_disp = app_lvgl_init(lcd_io, lcd_panel);
	if (!lvgl_disp) { ESP_LOGE(TAG, "LVGL init failed!"); esp_restart(); }

	ESP_ERROR_CHECK(touch_init(&tp));
	lvgl_port_touch_cfg_t touch_cfg = { .disp = lvgl_disp, .handle = tp };
	lvgl_port_add_touch(&touch_cfg);
	ESP_ERROR_CHECK(lcd_display_brightness_set(75));
	ESP_ERROR_CHECK(lcd_display_rotate(lvgl_disp, LV_DISPLAY_ROTATION_0));

	/* ---- 创建所有页面 ---- */
	create_wifi_list_screen();
	create_password_screen();
	create_connecting_screen();
	create_success_screen();
	create_failure_screen();

	/* ---- WiFi 初始化 + 首次扫描 ---- */
	wifi_init();
	wifi_scan_start();
	g_scan_start_tick = xTaskGetTickCount();

	/* ---- 加载 WiFi 列表页 ---- */
	lv_scr_load(scr_wifi_list);
	ESP_LOGI(TAG, "Entering main loop");

	/* ---- 页面状态机 ---- */
	enum { STATE_LIST, STATE_PWD, STATE_CONNECTING, STATE_SUCCESS, STATE_FAILURE } state = STATE_LIST;
	bool scan_populated = false;

	while (1) {
		vTaskDelay(pdMS_TO_TICKS(30));

		/* === Phase 1: 检查 WiFi 事件 (无需 LVGL 锁) === */
		bool need_populate = false;
		bool need_show_fail = false;
		bool need_connect_success = false;
		bool need_connect_fail = false;
		bool need_connect_timeout = false;

		/* WiFi 扫描检查 */
		if (state == STATE_LIST && !scan_populated && !g_scan_failed) {
			EventBits_t bits = xEventGroupWaitBits(
				g_wifi_events, WIFI_SCAN_DONE_BIT,
				pdFALSE, pdFALSE, 0);
			if (bits & WIFI_SCAN_DONE_BIT) {
				uint16_t count = wifi_scan_get_results();
				if (count > 0) {
					need_populate = true;
				} else {
					g_scan_failed = true;
					need_show_fail = true;
				}
			} else if ((xTaskGetTickCount() - g_scan_start_tick) > pdMS_TO_TICKS(15000)) {
				g_scan_failed = true;
				need_show_fail = true;
			}
		}

		/* 连接结果检查 */
		if (state == STATE_CONNECTING) {
			EventBits_t bits = xEventGroupWaitBits(
				g_wifi_events,
				WIFI_CONNECTED_BIT | WIFI_AUTH_FAIL_BIT,
				pdFALSE, pdFALSE, 0);
			if (bits & WIFI_CONNECTED_BIT) {
				need_connect_success = true;
			} else if (bits & WIFI_AUTH_FAIL_BIT) {
				need_connect_fail = true;
			} else if ((xTaskGetTickCount() - g_connect_tick) > pdMS_TO_TICKS(15000)) {
				need_connect_timeout = true;
			}
		}

		/* === Phase 2: 应用 UI 变更 (需要 LVGL 锁) === */
		bool has_ui_work = need_populate || need_show_fail ||
				   need_connect_success || need_connect_fail ||
				   need_connect_timeout;

		if (has_ui_work && lvgl_port_lock(pdMS_TO_TICKS(5000))) {
			if (need_populate) {
				populate_wifi_list();
				scan_populated = true;
				xEventGroupClearBits(g_wifi_events, WIFI_SCAN_DONE_BIT);
			}
			if (need_show_fail) {
				show_scan_fail();
				xEventGroupClearBits(g_wifi_events, WIFI_SCAN_DONE_BIT);
			}
			if (need_connect_success) {
				xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
				led_set_green();
				lv_scr_load(scr_success);
				state = STATE_SUCCESS;
				ESP_LOGI(TAG, "===== Connected =====");
			}
			if (need_connect_fail) {
				xEventGroupClearBits(g_wifi_events, WIFI_AUTH_FAIL_BIT);
				led_set_red();
				lv_textarea_set_text(ta_retry, "");
				lv_obj_add_flag(kb_retry, LV_OBJ_FLAG_HIDDEN);
				lv_scr_load(scr_failure);
				state = STATE_FAILURE;
				ESP_LOGI(TAG, "===== Wrong password =====");
			}
			if (need_connect_timeout) {
				led_set_red();
				lv_scr_load(scr_failure);
				state = STATE_FAILURE;
				ESP_LOGW(TAG, "Connection timeout");
			}
			lvgl_port_unlock();
		}

		/* === Phase 3: 检测页面切换 + 触摸 (需要 LVGL 锁) === */
		if (lvgl_port_lock(0)) {
			lv_obj_t *active = lv_scr_act();
			if (active == scr_password && state != STATE_PWD) {
				state = STATE_PWD;
				ESP_LOGI(TAG, "--> Password page");
			} else if (active == scr_connecting && state != STATE_CONNECTING) {
				state = STATE_CONNECTING;
				ESP_LOGI(TAG, "--> Connecting");
			} else if (active == scr_failure && state != STATE_FAILURE) {
				state = STATE_FAILURE;
				ESP_LOGI(TAG, "--> Failure/retry");
			} else if (active == scr_wifi_list && state != STATE_LIST) {
				state = STATE_LIST;
			}
			lvgl_port_unlock();
		}
	}
}

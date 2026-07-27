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
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
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
#define PING_OK_BIT          BIT3
#define PING_FAIL_BIT        BIT4

/* ============================ 全局状态 ================================== */
static EventGroupHandle_t g_wifi_events = NULL;
static EventGroupHandle_t g_ping_events = NULL;
static wifi_ap_record_t   g_ap_list[10];
static uint16_t           g_ap_count = 0;
static uint32_t           g_connect_tick = 0;
static bool               g_scan_failed = false;
static uint32_t           g_scan_start_tick = 0;
static int                g_selected_ap = -1;
static char               g_password[64] = "";
static int                g_state = 0;            /* 0=LIST 1=PWD 2=CONNECTING 3=SUCCESS 4=FAILURE */
static bool               g_scan_populated = false;
static bool               g_ping_pending = false; /* main loop 中执行 ping */

/* ============================ 股票数据 =================================== */
typedef struct { const char *code; const char *name; const char *sina; } stock_info_t;
static stock_info_t g_stocks[] = {
	{"sh688008", "688008", NULL},     /* 澜起科技 A股 */
	{"sh600036", "600036", NULL},     /* 招商银行 A股 */
	{"usNDX",    "NDX",    ".ndx"},   /* 纳斯达克100 美股 */
	{"us.INX",   "SPX",    ".inx"},   /* 标普500 美股 */
};
static int      g_stock_idx = 0;           /* 当前股票索引 */
#define KLINE_MAX  180

typedef struct { float o, c, h, l; } kline_t;
static kline_t g_kline[KLINE_MAX];
static int     g_kline_count = 0;
static float   g_price, g_change_pct, g_open, g_prev_close;
static bool    g_market_open = false;
static int     g_stock_view = 7;               /* 7/30/180 */
static int     g_stock_refresh_s = 5;          /* 倒计时秒 */
static uint32_t g_stock_tick = 0;
static bool    g_stock_kline_running = false;
static bool    g_stock_snap_running = false;
static char    g_stock_buf[32768];              /* HTTP 响应缓冲区 */
static int     g_stock_buf_len = 0;
static bool    g_stock_need_ui = false;

/* ============================ LVGL 屏幕 ================================== */
static lv_obj_t *scr_wifi_list = NULL;
static lv_obj_t *scr_password  = NULL;
static lv_obj_t *scr_connecting = NULL;
static lv_obj_t *scr_success   = NULL;
static lv_obj_t *scr_failure   = NULL;
static lv_obj_t *scr_ping_ok   = NULL;
static lv_obj_t *scr_ping_fail = NULL;
static lv_obj_t *scr_stock     = NULL;

/* 股票页控件 */
static lv_obj_t     *stock_chart = NULL;
static lv_chart_series_t *stock_series = NULL;
static lv_obj_t     *stock_header = NULL;
static lv_obj_t     *stock_footer = NULL;
static lv_obj_t     *stock_btn7 = NULL, *stock_btn30 = NULL, *stock_btn180 = NULL;
static lv_obj_t     *stock_ylabels[4];  /* Y 轴 4 个标签 */
static lv_obj_t     *stock_xlabels[10]; /* X 轴最多 10 个标签 */
static lv_display_t *g_lvgl_disp = NULL;

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
enum { LED_OFF, LED_BLINK_BLUE, LED_BLINK_RED_3, LED_SOLID_RED, LED_BLINK_GREEN_3, LED_SOLID_GREEN };
static int      g_led_state = LED_OFF;
static uint32_t g_led_last_toggle = 0;
static int      g_led_blink_cnt = 0;
static bool     g_led_on = false;

static void led_init(void)
{
	gpio_set_direction(LED_RED_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(LED_GREEN_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(LED_BLUE_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(LED_RED_GPIO, 1);
	gpio_set_level(LED_GREEN_GPIO, 1);
	gpio_set_level(LED_BLUE_GPIO, 1);
}

static void led_all_off(void) {
	gpio_set_level(LED_RED_GPIO, 1); gpio_set_level(LED_GREEN_GPIO, 1); gpio_set_level(LED_BLUE_GPIO, 1);
}

/* 切换指定 LED: on=true→点亮, on=false→熄灭 */
static void led_set(uint8_t gpio, bool on) {
	gpio_set_level(gpio, on ? 0 : 1);
}

/* 非阻塞 LED 状态机, 在 main loop 中每帧调用 */
static void led_update(void)
{
	uint32_t now = xTaskGetTickCount();
	uint32_t iv;

	switch (g_led_state) {
	case LED_BLINK_BLUE:
		iv = pdMS_TO_TICKS(500);
		if (now - g_led_last_toggle >= iv) {
			g_led_on = !g_led_on;
			led_set(LED_BLUE_GPIO, g_led_on);
			g_led_last_toggle = now;
		}
		break;

	case LED_BLINK_RED_3:
		iv = pdMS_TO_TICKS(300);
		if (now - g_led_last_toggle >= iv) {
			g_led_on = !g_led_on;
			led_set(LED_RED_GPIO, g_led_on);
			g_led_blink_cnt++;
			if (g_led_blink_cnt >= 6) {
				led_set(LED_RED_GPIO, true);
				g_led_state = LED_SOLID_RED;
			}
			g_led_last_toggle = now;
		}
		break;

	case LED_BLINK_GREEN_3:
		iv = pdMS_TO_TICKS(300);
		if (now - g_led_last_toggle >= iv) {
			g_led_on = !g_led_on;
			led_set(LED_GREEN_GPIO, g_led_on);
			g_led_blink_cnt++;
			if (g_led_blink_cnt >= 6) {
				led_set(LED_GREEN_GPIO, true);
				g_led_state = LED_SOLID_GREEN;
			}
			g_led_last_toggle = now;
		}
		break;

	default:
		break;
	}
}

/* 触发 LED 状态切换 */
static void led_start_blink(int state) {
	g_led_state = state;
	g_led_blink_cnt = 0;
	g_led_last_toggle = xTaskGetTickCount();
	g_led_on = false;
	led_all_off();
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
		lv_scr_load(scr_connecting);
		ESP_LOGI(TAG, "Switching to connecting page");
	} else {
		ESP_LOGW(TAG, "No AP selected");
	}
}

/* 密码/重试 textarea: 键盘按下取消 */
static void on_ta_cancel(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_user_data(e);
	lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
	ESP_LOGI(TAG, "Keyboard hidden");
}

/* ============================ Ping 功能 ================================== */

static esp_err_t ping_http_event_handler(esp_http_client_event_t *evt)
{
	if (evt->event_id == HTTP_EVENT_ON_DATA || evt->event_id == HTTP_EVENT_ON_FINISH) {
		xEventGroupSetBits(g_ping_events, PING_OK_BIT);
	}
	return ESP_OK;
}

static void do_ping(void)
{
	ESP_LOGI(TAG, "Pinging qt.gtimg.cn...");
	xEventGroupClearBits(g_ping_events, PING_OK_BIT | PING_FAIL_BIT);

	esp_http_client_config_t cfg = {
		.url = "http://qt.gtimg.cn/",
		.event_handler = ping_http_event_handler,
		.timeout_ms = 5000,
	};
	esp_http_client_handle_t client = esp_http_client_init(&cfg);
	esp_err_t err = esp_http_client_perform(client);

	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Ping failed: %s", esp_err_to_name(err));
		xEventGroupSetBits(g_ping_events, PING_FAIL_BIT);
	}
	esp_http_client_cleanup(client);
}

/* ============================ 股票数据获取 =============================== */

static esp_err_t stock_http_handler(esp_http_client_event_t *evt)
{
	if (evt->event_id == HTTP_EVENT_ON_DATA) {
		int copy = evt->data_len;
		/* ring buffer: 满时丢弃前半旧数据 */
		if (g_stock_buf_len + copy > (int)sizeof(g_stock_buf) - 1) {
			int keep = g_stock_buf_len * 3 / 4;
			memmove(g_stock_buf, g_stock_buf + g_stock_buf_len - keep, keep);
			g_stock_buf_len = keep;
			g_stock_buf[g_stock_buf_len] = '\0';
		}
		int remain = sizeof(g_stock_buf) - g_stock_buf_len - 1;
		if (copy > remain) copy = remain;
		memcpy(g_stock_buf + g_stock_buf_len, evt->data, copy);
		g_stock_buf_len += copy;
		g_stock_buf[g_stock_buf_len] = '\0';
	}
	return ESP_OK;
}

/* 解析实时行情(pipe分隔) */
static void parse_snapshot(const char *body)
{
	const char *p = strstr(body, "\"");
	if (!p) return;
	p++;
	char tmp[256]; int idx = 0, fi = 0;
	while (*p && *p != '"') {
		if (*p == '~') { tmp[idx] = '\0'; fi++; idx = 0;
			if (fi == 4) g_price = atof(tmp);         /* field 4 = latest price */
			if (fi == 5) g_prev_close = atof(tmp);     /* field 5 = prev close */
			if (fi == 6) g_open = atof(tmp);           /* field 6 = open */
			if (fi == 33) g_change_pct = atof(tmp);    /* field 33 = change % */
			if (fi == 41 && tmp[0]) g_market_open = strstr(tmp, "Open") || strstr(tmp, "open");
			if (fi == 57 && tmp[0]) g_market_open = (strcmp(tmp, "ZS") == 0);  /* 美股 ZS=开市 */
		} else if (idx < 255) tmp[idx++] = *p;
		p++;
	}
	ESP_LOGI(TAG, "Snapshot: price=%.2f open=%.2f prev=%.2f chg=%.2f%%", g_price, g_open, g_prev_close, g_change_pct);
}

static void fetch_snapshot(void)
{
	g_stock_buf_len = 0; g_stock_buf[0] = '\0';
	char url[128];
	snprintf(url, sizeof(url), "http://qt.gtimg.cn/q=%s", g_stocks[g_stock_idx].code);
	esp_http_client_config_t cfg = {
		.url = url,
		.event_handler = stock_http_handler,
		.timeout_ms = 5000,
		.buffer_size = 2048,
	};
	esp_http_client_handle_t cli = esp_http_client_init(&cfg);
	esp_err_t err = esp_http_client_perform(cli);
	if (err == ESP_OK) parse_snapshot(g_stock_buf);
	esp_http_client_cleanup(cli);
}

/* 从 JSON 对象起始处提取指定字段的浮点值 */
static float sina_extract(const char *p, const char *key)
{
	char buf[8]; snprintf(buf, sizeof(buf), "\"%s\":\"", key);
	const char *v = strstr(p, buf);
	return v ? (float)atof(v + strlen(buf)) : 0.0f;
}

/* 解析新浪美股 K 线 — 两遍扫描, 只取最后 KLINE_MAX 条 */
static int parse_kline_us(void)
{
	int total = 0;
	char *p = g_stock_buf;
	while ((p = strstr(p, "\"d\":\"")) != NULL) { total++; p++; }

	int skip = total - KLINE_MAX;
	if (skip < 0) skip = 0;
	p = g_stock_buf;
	for (int i = 0; i < skip; i++) { p = strstr(p, "\"d\":\""); if (!p) break; p++; }

	int cnt = 0;
	while (cnt < KLINE_MAX && (p = strstr(p, "\"d\":\"")) != NULL) {
		g_kline[cnt].c = sina_extract(p, "c");
		g_kline[cnt].o = sina_extract(p, "o");
		g_kline[cnt].h = sina_extract(p, "h");
		g_kline[cnt].l = sina_extract(p, "l");
		cnt++; p++;
	}
	return cnt;
}

/* 解析 K 线 — 手动提取, 不依赖 cJSON */
static int parse_kline(void)
{
	char *p = strstr(g_stock_buf, "qfqday\":[");
	int off = 9;
	if (!p) { p = strstr(g_stock_buf, "\"day\":["); off = 7; }
	if (!p) return 0;
	p += off;

	int cnt = 0;
	while (cnt < KLINE_MAX && *p == '[') {
		float o = 0, c = 0, h = 0, l = 0;
		/* 复制一行到临时缓冲区, 替换引号/逗号为空格便于 sscanf */
		char line[128]; int li = 0;
		while (*p && *p != ']' && li < 127) line[li++] = *p++;
		line[li] = '\0';
		for (int i = 0; i < li; i++) if (line[i] == '"' || line[i] == ',') line[i] = ' ';
		sscanf(line, "[ %*s %f %f %f %f", &o, &c, &h, &l);
		//
		ESP_LOGI(TAG, "  parsed[%d] o=%.2f c=%.2f h=%.2f l=%.2f", cnt, o, c, h, l);
		g_kline[cnt].o = o; g_kline[cnt].c = c;
		g_kline[cnt].h = h; g_kline[cnt].l = l;
		cnt++;
		p = strchr(p + 1, '[');
		if (!p) break;
	}
	//
	ESP_LOGI(TAG, "parse_kline: %d records, first close=%.2f last close=%.2f",
		 cnt, cnt > 0 ? g_kline[0].c : 0.0f, cnt > 0 ? g_kline[cnt-1].c : 0.0f);
	return cnt;
}

static void fetch_kline(int days)
{
	g_stock_buf_len = 0; g_stock_buf[0] = '\0';
	char url[256];
	if (g_stocks[g_stock_idx].sina) {
		/* 美股用新浪 API */
		snprintf(url, sizeof(url), "https://stock.finance.sina.com.cn/usstock/api/json_v2.php/US_MinKService.getDailyK?symbol=%s&type=daily&num=%d",
			 g_stocks[g_stock_idx].sina, days > KLINE_MAX ? KLINE_MAX : days);
	} else {
		/* A 股用腾讯财经 fqkline API */
		snprintf(url, sizeof(url), "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=%s,day,,,%d,qfq",
			 g_stocks[g_stock_idx].code, days > KLINE_MAX ? KLINE_MAX : days);
	}
	esp_http_client_config_t cfg = {
		.url = url,
		.event_handler = stock_http_handler,
		.timeout_ms = 8000,
		.buffer_size = 4096,
		.crt_bundle_attach = esp_crt_bundle_attach,
	};
	esp_http_client_handle_t cli = esp_http_client_init(&cfg);
	esp_http_client_set_header(cli, "User-Agent", "Mozilla/5.0");
	esp_err_t err = esp_http_client_perform(cli);
	int status = esp_http_client_get_status_code(cli);
	//
	ESP_LOGI(TAG, "K-line HTTP status=%d, buf len=%d, snippet: %.80s", status, g_stock_buf_len, g_stock_buf);
	if (err == ESP_OK) {
		g_kline_count = g_stocks[g_stock_idx].sina ? parse_kline_us() : parse_kline();
	}
	esp_http_client_cleanup(cli);
	//
	ESP_LOGI(TAG, "K-line: %d days fetched", g_kline_count);
}

/* ============================ 股票页创建 ================================= */

static void stock_update_chart(void)
{
	int view = g_stock_view;
	int start = g_kline_count - view;
	if (start < 0) start = 0;
	int n = g_kline_count - start;
	float ymin = 0, ymax = 100;

	if (g_kline_count > 0 && stock_series && n > 0) {
		int total = n + 1;
		float extra_close = g_price > 0 ? g_price : g_kline[g_kline_count-1].c;
		float extra_high  = g_price > g_kline[g_kline_count-1].h ? g_price : g_kline[g_kline_count-1].h;
		float extra_low   = g_price < g_kline[g_kline_count-1].l ? g_price : g_kline[g_kline_count-1].l;

		ymin = 999999; ymax = 0;
		for (int i = start; i < g_kline_count; i++) {
			if (g_kline[i].l < ymin) ymin = g_kline[i].l;
			if (g_kline[i].h > ymax) ymax = g_kline[i].h;
		}
		if (extra_low < ymin) ymin = extra_low;
		if (extra_high > ymax) ymax = extra_high;

		float pad = (ymax - ymin) * 0.05f;
		lv_chart_set_point_count(stock_chart, total);
		lv_chart_set_range(stock_chart, LV_CHART_AXIS_PRIMARY_Y, (int)(ymin - pad), (int)(ymax + pad));
		lv_chart_set_div_line_count(stock_chart, 3, (total <= 8) ? (total - 1) : 9);

		for (int i = 0; i < n; i++)
			lv_chart_set_next_value(stock_chart, stock_series, (int)g_kline[start + i].c);
		lv_chart_set_next_value(stock_chart, stock_series, (int)extra_close);
		lv_chart_refresh(stock_chart);
	}

	/* 更新 Y 轴标签 */
	char buf[16];
	float yrange = (ymax - ymin) * 1.05f + 1;
	for (int i = 0; i < 4; i++) {
		if (g_kline_count > 0)
			snprintf(buf, sizeof(buf), "%.0f", ymin + yrange * (3 - i) / 3);
		else
			snprintf(buf, sizeof(buf), "--");
		lv_label_set_text(stock_ylabels[i], buf);
		lv_obj_align_to(stock_ylabels[i], stock_chart, LV_ALIGN_OUT_LEFT_MID, 10, -80 + i * 53);
	}
	/* 隐藏所有 X 轴标签 */
	for (int i = 0; i < 10; i++)
		lv_obj_add_flag(stock_xlabels[i], LV_OBJ_FLAG_HIDDEN);
}

static void stock_update_header(void)
{
	if (!stock_header) return;
	bool up = g_change_pct >= 0;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s  %.2f  %s%.2f%%  %s",
		g_stocks[g_stock_idx].code, g_price,
		up ? "^" : "v", g_change_pct,
		g_market_open ? "Open" : "Closed");
	lv_label_set_text(stock_header, buf);
	lv_obj_set_style_text_color(stock_header,
		up ? lv_color_hex(0xFF0000) : lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
}

static void stock_update_footer(void)
{
	if (!stock_footer) return;
	char buf[64];
	snprintf(buf, sizeof(buf), "O:%.2f  C:%.2f  [%2ds]", g_open, g_price, g_stock_refresh_s);
	lv_label_set_text(stock_footer, buf);
}

static void on_stock_btn7(lv_event_t *e)  { g_stock_view = 7;  g_stock_need_ui = true; }
static void on_stock_btn30(lv_event_t *e) { g_stock_view = 30; g_stock_need_ui = true; }
static void on_stock_btn180(lv_event_t *e){ g_stock_view = 180; g_stock_need_ui = true; }

static void on_stock_next(lv_event_t *e)
{
	g_stock_idx = (g_stock_idx + 1) % (sizeof(g_stocks)/sizeof(g_stocks[0]));
	ESP_LOGI(TAG, "Switch stock: idx=%d code=%s", g_stock_idx, g_stocks[g_stock_idx].code);
	g_kline_count = 0;
	g_stock_view = 7;
	g_stock_refresh_s = 5;
	g_stock_tick = xTaskGetTickCount();
	lv_chart_set_point_count(stock_chart, 0);  /* 清除旧折线 */
	lv_chart_refresh(stock_chart);
	g_stock_kline_running = true;
	g_stock_snap_running = true;
	g_stock_need_ui = true;
}

static void create_stock_screen(void)
{
	scr_stock = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_stock, lv_color_black(), LV_STATE_DEFAULT);

	/* 左侧按钮 */
	stock_btn7 = lv_button_create(scr_stock);
	lv_obj_set_size(stock_btn7, 48, 48);
	lv_obj_align(stock_btn7, LV_ALIGN_LEFT_MID, 4, -55);
	lv_obj_add_event_cb(stock_btn7, on_stock_btn7, LV_EVENT_CLICKED, NULL);
	lv_obj_t *l7 = lv_label_create(stock_btn7); lv_label_set_text(l7, "7"); lv_obj_center(l7);

	stock_btn30 = lv_button_create(scr_stock);
	lv_obj_set_size(stock_btn30, 48, 48);
	lv_obj_align(stock_btn30, LV_ALIGN_LEFT_MID, 4, -5);
	lv_obj_add_event_cb(stock_btn30, on_stock_btn30, LV_EVENT_CLICKED, NULL);
	lv_obj_t *l30 = lv_label_create(stock_btn30); lv_label_set_text(l30, "30"); lv_obj_center(l30);

	stock_btn180 = lv_button_create(scr_stock);
	lv_obj_set_size(stock_btn180, 48, 48);
	lv_obj_align(stock_btn180, LV_ALIGN_LEFT_MID, 4, 45);
	lv_obj_add_event_cb(stock_btn180, on_stock_btn180, LV_EVENT_CLICKED, NULL);
	lv_obj_t *l180 = lv_label_create(stock_btn180); lv_label_set_text(l180, "180"); lv_obj_center(l180);

	lv_obj_t *btn_next = lv_button_create(scr_stock);
	lv_obj_set_size(btn_next, 48, 48);
	lv_obj_align(btn_next, LV_ALIGN_LEFT_MID, 4, 95);
	lv_obj_add_event_cb(btn_next, on_stock_next, LV_EVENT_CLICKED, NULL);
	lv_obj_t *ln = lv_label_create(btn_next); lv_label_set_text(ln, ">>"); lv_obj_center(ln);

	/* 上部信息栏 */
	stock_header = lv_label_create(scr_stock);
	lv_obj_set_style_text_color(stock_header, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_align(stock_header, LV_ALIGN_TOP_RIGHT, 0, 4);

	/* 折线图 */
	stock_chart = lv_chart_create(scr_stock);
	lv_obj_set_size(stock_chart, 250, 180);
	lv_obj_align(stock_chart, LV_ALIGN_RIGHT_MID, 0, 12);
	lv_chart_set_type(stock_chart, LV_CHART_TYPE_LINE);
	lv_obj_set_style_bg_color(stock_chart, lv_color_black(), LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(stock_chart, 0, LV_STATE_DEFAULT);
	lv_chart_set_range(stock_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
	lv_obj_set_style_line_color(stock_chart, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
	lv_obj_set_style_size(stock_chart, 0, 0, LV_PART_INDICATOR);
	stock_series = lv_chart_add_series(stock_chart, lv_color_hex(0xFFFFFF), LV_CHART_AXIS_PRIMARY_Y);

	/* Y 轴标签(左侧) */
	for (int i = 0; i < 4; i++) {
		stock_ylabels[i] = lv_label_create(scr_stock);
		lv_obj_set_style_text_color(stock_ylabels[i], lv_color_white(), LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(stock_ylabels[i], &lv_font_montserrat_14, LV_STATE_DEFAULT);
		lv_obj_align(stock_ylabels[i], LV_ALIGN_LEFT_MID, 56, -70 + i * 45);
	}
	/* X 轴标签(底部) */
	for (int i = 0; i < 10; i++) {
		stock_xlabels[i] = lv_label_create(scr_stock);
		lv_obj_set_style_text_color(stock_xlabels[i], lv_color_white(), LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(stock_xlabels[i], &lv_font_montserrat_14, LV_STATE_DEFAULT);
		lv_obj_add_flag(stock_xlabels[i], LV_OBJ_FLAG_HIDDEN);
	}

	/* 底部信息栏 */
	stock_footer = lv_label_create(scr_stock);
	lv_obj_set_style_text_color(stock_footer, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_align(stock_footer, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
}

static void on_continue_click(lv_event_t *e)
{
	if (g_state == 3) { /* Connected -> ping */
		ESP_LOGI(TAG, "Continue: starting ping...");
		g_state = 5;
		g_ping_pending = true;
	} else if (g_state == 6) { /* Ping OK -> stock */
		ESP_LOGI(TAG, "Continue: entering stock page...");
		g_state = 8;
		lcd_display_rotate(g_lvgl_disp, LV_DISPLAY_ROTATION_90);
		lv_scr_load(scr_stock);
		g_stock_kline_running = true;
		g_stock_snap_running = true;
		g_stock_refresh_s = 5;
		g_stock_tick = xTaskGetTickCount();
	}
}

static void on_reping_click(lv_event_t *e)
{
	ESP_LOGI(TAG, "Re-ping clicked");
	g_ping_pending = true;
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
static void on_rescan_click(lv_event_t *e)
{
	ESP_LOGI(TAG, "Rescan WiFi clicked");
	if (g_state >= 8) lcd_display_rotate(g_lvgl_disp, LV_DISPLAY_ROTATION_0);
	g_ap_count = 0;
	g_scan_failed = false;
	g_scan_populated = false;
	g_selected_ap = -1;
	g_state = 0;
	lv_obj_clear_flag(wifi_hint, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(wifi_fail, LV_OBJ_FLAG_HIDDEN);
	wifi_scan_start();
	g_scan_start_tick = xTaskGetTickCount();
	led_start_blink(LED_BLINK_BLUE);
	lv_scr_load(scr_wifi_list);
}

static void create_success_screen(void)
{
	scr_success = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_success, lv_color_black(), LV_STATE_DEFAULT);

	lv_obj_t *check = lv_label_create(scr_success);
	lv_label_set_text(check, LV_SYMBOL_OK);
	lv_obj_set_style_text_color(check, lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
	lv_obj_align(check, LV_ALIGN_CENTER, 0, -40);

	lv_obj_t *text = lv_label_create(scr_success);
	lv_label_set_text(text, "Connected!");
	lv_obj_set_style_text_color(text, lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
	lv_obj_align(text, LV_ALIGN_CENTER, 0, 10);

	lv_obj_t *btn_rescan = lv_button_create(scr_success);
	lv_obj_set_size(btn_rescan, 200, 36);
	lv_obj_align(btn_rescan, LV_ALIGN_CENTER, 0, 60);
	lv_obj_add_event_cb(btn_rescan, on_rescan_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl_rescan = lv_label_create(btn_rescan);
	lv_label_set_text(lbl_rescan, "Rescan WiFi");
	lv_obj_center(lbl_rescan);

	lv_obj_t *btn_continue = lv_button_create(scr_success);
	lv_obj_set_size(btn_continue, 200, 36);
	lv_obj_align(btn_continue, LV_ALIGN_CENTER, 0, 110);
	lv_obj_add_event_cb(btn_continue, on_continue_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl_continue = lv_label_create(btn_continue);
	lv_label_set_text(lbl_continue, "Continue");
	lv_obj_center(lbl_continue);
}

static void create_ping_ok_screen(void)
{
	scr_ping_ok = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_ping_ok, lv_color_black(), LV_STATE_DEFAULT);

	lv_obj_t *check = lv_label_create(scr_ping_ok);
	lv_label_set_text(check, LV_SYMBOL_OK);
	lv_obj_set_style_text_color(check, lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
	lv_obj_align(check, LV_ALIGN_CENTER, 0, -40);

	lv_obj_t *text = lv_label_create(scr_ping_ok);
	lv_label_set_text(text, "Ping OK");
	lv_obj_set_style_text_color(text, lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
	lv_obj_align(text, LV_ALIGN_CENTER, 0, 10);

	lv_obj_t *btn_rescan = lv_button_create(scr_ping_ok);
	lv_obj_set_size(btn_rescan, 200, 36);
	lv_obj_align(btn_rescan, LV_ALIGN_CENTER, 0, 60);
	lv_obj_add_event_cb(btn_rescan, on_rescan_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl_rescan = lv_label_create(btn_rescan);
	lv_label_set_text(lbl_rescan, "Rescan WiFi");
	lv_obj_center(lbl_rescan);

	lv_obj_t *btn_cont = lv_button_create(scr_ping_ok);
	lv_obj_set_size(btn_cont, 200, 36);
	lv_obj_align(btn_cont, LV_ALIGN_CENTER, 0, 110);
	lv_obj_add_event_cb(btn_cont, on_continue_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl_cont = lv_label_create(btn_cont);
	lv_label_set_text(lbl_cont, "Continue");
	lv_obj_center(lbl_cont);
}

static void create_ping_fail_screen(void)
{
	scr_ping_fail = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_ping_fail, lv_color_black(), LV_STATE_DEFAULT);

	lv_obj_t *cross = lv_label_create(scr_ping_fail);
	lv_label_set_text(cross, LV_SYMBOL_CLOSE);
	lv_obj_set_style_text_color(cross, lv_color_hex(0xFF0000), LV_STATE_DEFAULT);
	lv_obj_align(cross, LV_ALIGN_CENTER, 0, -40);

	lv_obj_t *text = lv_label_create(scr_ping_fail);
	lv_label_set_text(text, "Ping Failed");
	lv_obj_set_style_text_color(text, lv_color_hex(0xFF0000), LV_STATE_DEFAULT);
	lv_obj_align(text, LV_ALIGN_CENTER, 0, 10);

	lv_obj_t *btn_rescan = lv_button_create(scr_ping_fail);
	lv_obj_set_size(btn_rescan, 200, 36);
	lv_obj_align(btn_rescan, LV_ALIGN_CENTER, 0, 60);
	lv_obj_add_event_cb(btn_rescan, on_rescan_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl_rescan = lv_label_create(btn_rescan);
	lv_label_set_text(lbl_rescan, "Rescan WiFi");
	lv_obj_center(lbl_rescan);

	lv_obj_t *btn_reping = lv_button_create(scr_ping_fail);
	lv_obj_set_size(btn_reping, 200, 36);
	lv_obj_align(btn_reping, LV_ALIGN_CENTER, 0, 110);
	lv_obj_add_event_cb(btn_reping, on_reping_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *lbl_reping = lv_label_create(btn_reping);
	lv_label_set_text(lbl_reping, "Re-ping");
	lv_obj_center(lbl_reping);
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
	lv_obj_align(cross, LV_ALIGN_CENTER, 0, -70);

	lv_obj_t *text = lv_label_create(scr_failure);
	lv_label_set_text(text, "Wrong Password");
	lv_obj_set_style_text_color(text, lv_color_hex(0xFF0000), LV_STATE_DEFAULT);
	lv_obj_align(text, LV_ALIGN_CENTER, 0, -45);

	/* 重试文本域 */
	ta_retry = lv_textarea_create(scr_failure);
	lv_textarea_set_one_line(ta_retry, true);
	lv_textarea_set_max_length(ta_retry, 63);
	lv_obj_set_size(ta_retry, 220, 36);
	lv_obj_align(ta_retry, LV_ALIGN_CENTER, 0, -16);
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
	g_lvgl_disp = lvgl_disp;
	if (!lvgl_disp) { ESP_LOGE(TAG, "LVGL init failed!"); esp_restart(); }

	ESP_ERROR_CHECK(touch_init(&tp));
	lvgl_port_touch_cfg_t touch_cfg = { .disp = lvgl_disp, .handle = tp };
	lvgl_port_add_touch(&touch_cfg);
	ESP_ERROR_CHECK(lcd_display_brightness_set(75));
	ESP_ERROR_CHECK(lcd_display_rotate(lvgl_disp, LV_DISPLAY_ROTATION_0));
	g_ping_events = xEventGroupCreate();

	/* ---- 创建所有页面(需 LVGL 锁) ---- */
	lvgl_port_lock(-1);
	create_wifi_list_screen();
	create_password_screen();
	create_connecting_screen();
	create_success_screen();
	create_failure_screen();
	create_ping_ok_screen();
	create_ping_fail_screen();
	create_stock_screen();
	lvgl_port_unlock();

	/* ---- WiFi 初始化 + 首次扫描 ---- */
	wifi_init();
	wifi_scan_start();
	g_scan_start_tick = xTaskGetTickCount();
	led_start_blink(LED_BLINK_BLUE);

	/* ---- 加载 WiFi 列表页 ---- */
	lv_scr_load(scr_wifi_list);
	ESP_LOGI(TAG, "Entering main loop");

	/* ---- 页面状态机 ---- */
	g_state = 0;  /* STATE_LIST */
	g_scan_populated = false;

	while (1) {
		vTaskDelay(pdMS_TO_TICKS(30));

		/* === Phase 1: 检查 WiFi 事件 (无需 LVGL 锁) === */
		led_update();

		if (g_ping_pending) {
			g_ping_pending = false;
			do_ping();
		}

		/* 股票数据获取(无锁) */
		if (g_stock_kline_running) {
			g_stock_kline_running = false;
			fetch_kline(KLINE_MAX);
			g_stock_need_ui = true;
		}
		if (g_stock_snap_running) {
			g_stock_snap_running = false;
			fetch_snapshot();
			g_stock_need_ui = true;
		}

		/* 股票刷新倒计时(仅stock页) */
		if (g_state == 8 && (xTaskGetTickCount() - g_stock_tick) >= pdMS_TO_TICKS(1000)) {
			g_stock_tick = xTaskGetTickCount();
			g_stock_refresh_s--;
			g_stock_need_ui = true;
			if (g_stock_refresh_s <= 0) {
				g_stock_refresh_s = 5;
				g_stock_snap_running = true;
			}
		}

		bool need_populate = false;
		bool need_show_fail = false;
		bool need_connect_success = false;
		bool need_connect_fail = false;

		/* WiFi 扫描检查 */
		if (g_state == 0 && !g_scan_populated && !g_scan_failed) {
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
		if (g_state == 2) {
			EventBits_t bits = xEventGroupWaitBits(
				g_wifi_events,
				WIFI_CONNECTED_BIT | WIFI_AUTH_FAIL_BIT,
				pdFALSE, pdFALSE, 0);
			if (bits & WIFI_CONNECTED_BIT) {
				need_connect_success = true;
			} else if (bits & WIFI_AUTH_FAIL_BIT) {
				need_connect_fail = true;
			}
		}

		/* Ping 结果检查 */
		bool need_ping_ok = false;
		bool need_ping_fail = false;
		if (g_state == 5) {
			EventBits_t bits = xEventGroupWaitBits(
				g_ping_events, PING_OK_BIT | PING_FAIL_BIT,
				pdFALSE, pdFALSE, 0);
			if (bits & PING_OK_BIT) {
				need_ping_ok = true;
			} else if (bits & PING_FAIL_BIT) {
				need_ping_fail = true;
			}
		}

		/* === Phase 2: 应用 UI 变更 (需要 LVGL 锁) === */
		bool has_ui_work = need_populate || need_show_fail ||
				   need_connect_success || need_connect_fail ||
				   need_ping_ok || need_ping_fail ||
				   g_stock_need_ui;

		if (has_ui_work && lvgl_port_lock(pdMS_TO_TICKS(5000))) {
			if (need_populate) {
				populate_wifi_list();
				g_scan_populated = true;
				xEventGroupClearBits(g_wifi_events, WIFI_SCAN_DONE_BIT);
			}
			if (need_show_fail) {
				show_scan_fail();
				xEventGroupClearBits(g_wifi_events, WIFI_SCAN_DONE_BIT);
			}
			if (need_connect_success) {
				xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
				led_start_blink(LED_BLINK_GREEN_3);
				lv_scr_load(scr_success);
				g_state = 3;
				ESP_LOGI(TAG, "===== Connected =====");
			}
			if (need_connect_fail) {
				xEventGroupClearBits(g_wifi_events, WIFI_AUTH_FAIL_BIT);
				led_start_blink(LED_BLINK_RED_3);
				lv_textarea_set_text(ta_retry, "");
				lv_obj_add_flag(kb_retry, LV_OBJ_FLAG_HIDDEN);
				lv_scr_load(scr_failure);
				g_state = 4;
				ESP_LOGI(TAG, "===== Wrong password =====");
			}
			if (need_ping_ok) {
				xEventGroupClearBits(g_ping_events, PING_OK_BIT);
				led_start_blink(LED_BLINK_GREEN_3);
				lv_scr_load(scr_ping_ok);
				g_state = 6;
				ESP_LOGI(TAG, "===== Ping OK =====");
			}
			if (need_ping_fail) {
				xEventGroupClearBits(g_ping_events, PING_FAIL_BIT);
				led_start_blink(LED_BLINK_RED_3);
				lv_scr_load(scr_ping_fail);
				g_state = 7;
				ESP_LOGI(TAG, "===== Ping Failed =====");
			}
			/* 股票页更新 */
			if (g_state == 8 && g_stock_need_ui) {
				stock_update_header();
				stock_update_footer();
				stock_update_chart();
				g_stock_need_ui = false;
			}
			lvgl_port_unlock();
		}

		/* === Phase 3: 检测页面切换 + 触摸 (需要 LVGL 锁) === */
		if (g_state != 5 && g_state != 8 && lvgl_port_lock(0)) {
			lv_obj_t *active = lv_scr_act();
			if (active == scr_password && g_state != 1) {
				g_state = 1;
				ESP_LOGI(TAG, "--> Password page");
			} else if (active == scr_connecting && g_state != 2) {
				g_state = 2;
				ESP_LOGI(TAG, "--> Connecting");
			} else if (active == scr_failure && g_state != 4) {
				g_state = 4;
				ESP_LOGI(TAG, "--> Failure/retry");
			} else if (active == scr_wifi_list && g_state != 0) {
				g_state = 0;
			}
			lvgl_port_unlock();
		}
	}
}

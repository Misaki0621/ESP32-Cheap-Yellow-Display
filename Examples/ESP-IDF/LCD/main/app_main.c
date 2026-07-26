#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lcd.h"
#include "gui.h"
#include "pic.h"

#define LED_RED_GPIO    4
#define LED_GREEN_GPIO  16
#define LED_BLUE_GPIO   17

#define TOTAL_STEPS     22

#define INFO_X          0
#define INFO_Y          0
#define INFO_W          240
#define INFO_H          14
#define INFO_BG         BLACK
#define INFO_FG         WHITE
#define INFO_SIZE       12

static const char *TAG = "main";

static void led_init(void)
{
	gpio_set_direction(LED_RED_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(LED_GREEN_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(LED_BLUE_GPIO, GPIO_MODE_OUTPUT);

	gpio_set_level(LED_RED_GPIO, 1);
	gpio_set_level(LED_GREEN_GPIO, 1);
	gpio_set_level(LED_BLUE_GPIO, 1);
}

static void led_set_red(void)
{
	gpio_set_level(LED_RED_GPIO, 0);
	gpio_set_level(LED_GREEN_GPIO, 1);
	gpio_set_level(LED_BLUE_GPIO, 1);
}

static void led_set_green(void)
{
	gpio_set_level(LED_RED_GPIO, 1);
	gpio_set_level(LED_GREEN_GPIO, 0);
	gpio_set_level(LED_BLUE_GPIO, 1);
}

static void led_set_blue(void)
{
	gpio_set_level(LED_RED_GPIO, 1);
	gpio_set_level(LED_GREEN_GPIO, 1);
	gpio_set_level(LED_BLUE_GPIO, 0);
}

static void led_set_color(int step)
{
	switch (step % 3) {
	case 1:
		led_set_red();
		break;
	case 2:
		led_set_green();
		break;
	case 0:
		led_set_blue();
		break;
	}
}

static const char* led_color_name(int step)
{
	switch (step % 3) {
	case 1: return "红色";
	case 2: return "绿色";
	case 0: return "蓝色";
	default: return "";
	}
}

static const char* led_color_short(int step)
{
	switch (step % 3) {
	case 1: return "R";
	case 2: return "G";
	case 0: return "B";
	default: return "";
	}
}

static void info_clear(void)
{
	LCD_DrawFillRectangle(INFO_X, INFO_Y, INFO_X + INFO_W - 1, INFO_Y + INFO_H - 1, INFO_BG);
}

static void info_show(int step, const char *action)
{
	char buf[40];
	snprintf(buf, sizeof(buf), "%d/%d %s LED:%s", step, TOTAL_STEPS, action, led_color_short(step));
	info_clear();
	LCD_ShowString(INFO_X, INFO_Y, INFO_BG, INFO_FG, INFO_SIZE, buf, 1);
}

void app_main(void)
{
	int step;

	led_init();

	while (1) {

		step = 1;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 初始化 LCD", step, TOTAL_STEPS);
		Init_LCD();
		info_show(step, "InitLCD");
		vTaskDelay(pdMS_TO_TICKS(1000));
		info_clear();

		step = 2;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "FillRect");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画实心矩形", step, TOTAL_STEPS);
		LCD_DrawFillRectangle(20,20,100,100,GREEN);
		info_clear();

		step = 3;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Rect");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画空心矩形", step, TOTAL_STEPS);
		LCD_DrawRectangle(20,220,100,200,GREEN);
		info_clear();

		step = 4;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Circle");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画圆", step, TOTAL_STEPS);
		LCD_Draw_Circle(180,200,40,WHITE);
		info_clear();

		step = 5;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "FillCircle");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画实心圆", step, TOTAL_STEPS);
		LCD_Draw_FillCircle(180,200,30,WHITE);
		info_clear();

		step = 6;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Point");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画点", step, TOTAL_STEPS);
		LCD_DrawPoint(120,110,RED);
		info_clear();

		step = 7;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Line");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画线", step, TOTAL_STEPS);
		LCD_DrawLine(0, 0, 128, 128,RED);
		info_clear();

		step = 8;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "AngleLine");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画角度线", step, TOTAL_STEPS);
		LCD_Draw_AngleLine(100,100,35,85,BLACK);
		info_clear();

		step = 9;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "ThickLine0");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画粗线1", step, TOTAL_STEPS);
		LCD_DrawBLine0(20,160,60,190,5,YELLOW);
		info_clear();

		step = 10;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "ThickLine1");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画粗线2", step, TOTAL_STEPS);
		LCD_DrawBLine1(40,140,90,190,2,YELLOW);
		info_clear();

		step = 11;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Triangle");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画三角形", step, TOTAL_STEPS);
		LCD_DrawTriangel(100,50,30,100,150,150,RED);
		info_clear();

		step = 12;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "FillTri");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 画实心三角形", step, TOTAL_STEPS);
		LCD_DrawFillTriangel(180,30,160,80,200,120,RED);
		info_clear();

		step = 13;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "CharA12");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符 A (12号叠加)", step, TOTAL_STEPS);
		LCD_ShowChar(120,0,BLACK,RED, 'A',12,1);
		info_clear();

		step = 14;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "CharB12");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符 B (12号覆盖)", step, TOTAL_STEPS);
		LCD_ShowChar(140,0,BLACK,RED, 'B',12,0);
		info_clear();

		step = 15;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "CharA16");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符 A (16号叠加)", step, TOTAL_STEPS);
		LCD_ShowChar(160,0,BLACK,RED, 'A',16,1);
		info_clear();

		step = 16;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "CharB16");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符 B (16号覆盖)", step, TOTAL_STEPS);
		LCD_ShowChar(180,0,BLACK,RED, 'B',16,0);
		info_clear();

		step = 17;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Str12+");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符串 (12号叠加)", step, TOTAL_STEPS);
		LCD_ShowString(10,240,GREEN,RED,12,"ABCDabcd123",1);
		info_clear();

		step = 18;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Str12");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符串 (12号覆盖)", step, TOTAL_STEPS);
		LCD_ShowString(10,252,GREEN,RED,12,"ABCDabcd123",0);
		info_clear();

		step = 19;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Str16+");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符串 (16号叠加)", step, TOTAL_STEPS);
		LCD_ShowString(10,264,GREEN,RED,16,"ABCDabcd123",1);
		info_clear();

		step = 20;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Str16");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示字符串 (16号覆盖)", step, TOTAL_STEPS);
		LCD_ShowString(10,280,GREEN,RED,16,"ABCDabcd123",0);
		info_clear();

		step = 21;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Number");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示数字", step, TOTAL_STEPS);
		LCD_ShowNum(10,296,WHITE,BLACK,123456,7,16);
		info_clear();

		step = 22;
		ESP_LOGI(TAG, "[%d/%d] 设置LED为%s", step, TOTAL_STEPS, led_color_name(step));
		led_set_color(step);
		info_show(step, "Bitmap");
		vTaskDelay(pdMS_TO_TICKS(1000));
		ESP_LOGI(TAG, "[%d/%d] 显示图片", step, TOTAL_STEPS);
		LCD_Drawbmp16(100,240,gImage_qq);
		info_clear();

		ESP_LOGI(TAG, "====== complete ======");
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

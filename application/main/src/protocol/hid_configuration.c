#include <stdint.h>
#include <string.h>

#include "main.h"
#include "hid_configuration.h"
#include "nordic_common.h"
#include "usb_comm.h"
#include "util.h"

#include "action.h"
#include "action_layer.h"
#include "data_storage.h"
#include "keymap.h"
#include "keyboard_fn.h"
#include "keyboard_battery.h"
#include "ble_hid_service.h"

const uint32_t keyboard_function_table =
#ifdef BOOTMAGIC_ENABLE
    (1 << 0) +
#endif
#ifdef MOUSEKEY_ENABLE
    (1 << 1) +
#endif
#ifdef EXTRAKEY_ENABLE
    (1 << 2) +
#endif
#ifdef NKRO_ENABLE
    (1 << 3) +
#endif
#ifdef KEYMAP_STORAGE
    (1 << 8) +
#endif
#ifdef ACTIONMAP_ENABLE
    (1 << 9) +
#endif
#ifdef MACRO_STORAGE
    (1 << 10) +
#endif
#ifdef CONFIG_STORAGE
    (1 << 11) +
#endif
#ifdef RGBLIGHT_ENABLE
    (1 << 16) +
#endif
#ifdef RGB_MATRIX_ENABLE
    (1 << 17) +
#endif
    0;

#ifndef CONF_VENDOR_ID
#define CONF_VENDOR_ID VENDOR_ID
#endif

#ifndef CONF_PRODUCT_ID
#define CONF_PRODUCT_ID PRODUCT_ID
#endif

#ifdef HAS_USB

HID_CONFIG_DEF();
#ifdef CONFIG_STORAGE
static struct hid_config_section* hid_config_get(uint8_t id)
{
    for (uint8_t i = 0; i < HID_CONFIG_COUNT; i++) {
        if (HID_CONFIG_GET(i)->index == id)
            return HID_CONFIG_GET(i);
    }
    return 0;
}
#endif

static const struct host_driver* last_driver = NULL;
static uint8_t current_packet_size = 56;
static keyrecord_t virtual_record = {
    .event.pressed = true
};

/**
 * @brief 响应HID命令
 * 
 * 根据 HID 命令生成响应数据并发送
 *
 * @param cmd HID 命令类型
 * @param len 响应数据的长度
 * @param data 响应数据指针
 */
static void hid_response(enum hid_command cmd, uint8_t len, uint8_t* data)
{
    if (last_driver == NULL || !last_driver->driver_working())
        return;

    uint8_t buff[63];
    buff[0] = cmd;
    buff[1] = len;
    memcpy(&buff[2], data, len);
    last_driver->send_packet(PACKET_CONF, len + 2, buff);
}

/**
 * @brief 响应HID处理结果
 *
 * @param response 响应结果
 */
void hid_response_generic(enum hid_response response)
{
    if (last_driver == NULL || !last_driver->driver_working())
        return;

    last_driver->send_packet(PACKET_CONF, 1, &response);
}

/**
 * @brief 发送运行错误信息
 *
 * @param err_code 错误信息
 */
void hid_send_error(bool flag, uint8_t id, uint32_t err_code)
{
#ifdef DEBUG_INFO
    uint8_t data_buffer[6]; // 创建一个数组来存储err_code的字节
    data_buffer[0] = 0xFE;  // 附加包头确认数据类型
    data_buffer[1] = id;
    // 将err_code的每个字节复制到data_buffer中
    memcpy(data_buffer + 2, &err_code, sizeof(err_code));
    if (flag) {
        uart_send_conf(4, data_buffer);
    } else {
        ble_send_conf(4, data_buffer);
    }
#endif
}

/**
 * @brief 发送运行记录日志
 *
 * @param flag 日志发送方向 1为USB，0为BLE
 * @param id 日志ID 0~255
 * @param data 日志数据
 */
void hid_send_log(bool flag, uint8_t id, uint8_t len, uint8_t* data)
{
#ifdef DEBUG_INFO
    uint8_t data_buffer[61] = {0}; // 创建一个数组来存储log的字节
    data_buffer[0] = 0xFE;  // 附加包头254【0xFE】确认数据类型为运行记录日志
    data_buffer[1] = id;    // 附加ID，用于标示及确认日志来自于代码那个位置
    //将data的每个字节复制到data_buffer中
    memcpy(&data_buffer[2], data, len);
    if (flag) {
        uart_send_conf(len + 2, data_buffer);
    } else {
        ble_send_conf(len + 2, data_buffer);
    }
#endif
}

/**
 * @brief 发送键盘信息
 * 
 */
static void send_information()
{
    const uint8_t info[] = {
        UINT16_SEQ(CONF_VENDOR_ID), // VENDOR
        UINT16_SEQ(CONF_PRODUCT_ID), // PRODUCT
        DEVICE_VER & 0xFF, // HWVER
        HID_PROTOCOL, // PROTOCOL_VER
        UINT32_SEQ(APP_VERSION), // FIRMWARE_VER
        UINT32_SEQ(BUILD_TIME), // BUILD_DATE
        UINT32_SEQ(keyboard_function_table), // FUNCTION_TABLE
        UINT32_SEQ(NRF_FICR->INFO.PART), // 芯片型号
    };
    hid_response(HID_CMD_GENERIC, sizeof(info), (uint8_t*)info);
}

static uint8_t get_trunk_size(uint8_t mtu)
{
    return (mtu - 3) / 4 * 4;
}

static void send_information_sub_1()
{
    uint8_t info[8] = { 
        UINT32_SEQ(NRF_FICR->DEVICEID[0]),
        UINT32_SEQ(NRF_FICR->DEVICEID[1]),
     };

    info[6] = current_packet_size;
    info[7] = 0;
    hid_response(HID_CMD_GENERIC, sizeof(info), info);
}

static void send_information_layer()
{
        uint8_t info[8] = {
        UINT32_SEQ(layer_state), // 层
        UINT32_SEQ(default_layer_state), // 默认层
    };
    hid_response(HID_CMD_ABOUT_LAYER, sizeof(info), (uint8_t*)info);
}

static void send_information_battery()
{
        uint8_t info[1] = {
        battery_info.percentage, // 电量
    };
    hid_response(HID_CMD_GET_BATTERY_INFO, sizeof(info), (uint8_t*)info);
}




/**
 * @brief 获取单个按键键值
 * 
 * @param layer 按键层
 * @param row 按键所在行
 * @param col 按键所在列 
 */
static void get_single_key(uint8_t layer, uint8_t row, uint8_t col)
{
    keypos_t pos = {
        .col = col,
        .row = row
    };
#ifndef ACTIONMAP_ENABLE
    uint8_t code[] = { keymap_key_to_keycode(layer, pos), 0 };
    hid_response(HID_CMD_GENERIC, 2, code);
#else
    action_t action = action_for_key(layer, pos);
    uint8_t code[] = { UINT16_SEQ(action.code) };
    hid_response(HID_CMD_GENERIC, 2, code);
#endif
}
/**
 * @brief 获取单个FN键值
 * 
 * @param id FN的ID
 */
static void get_single_fn(uint8_t id)
{
#if defined(KEYMAP_STORAGE) && !defined(ACTIONMAP_ENABLE)
    uint8_t data[2];
    uint8_t len = storage_read_data(STORAGE_FN, id * 2, 2, data);
    if (len != 2)
        return hid_response_generic(HID_RESP_PARAMETER_ERROR);
    hid_response(HID_CMD_GENERIC, 2, data);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

static void response_storage(enum storage_type type, uint16_t offset, uint16_t len)
{
    if (len > MAX_HID_PACKET_SIZE)
        return hid_response_generic(HID_RESP_PARAMETER_ERROR);

    uint8_t data[MAX_HID_PACKET_SIZE];

    len = storage_read_data(type, offset, len, data);
    hid_response(HID_CMD_GENERIC, len, data);
}

/**
 * @brief 获取所有按键键值
 * 
 * @param offset 按键偏移量
 */
static void get_all_keys(uint16_t offset)
{
    response_storage(STORAGE_KEYMAP, offset, MAX_HID_PACKET_SIZE);
}

/**
 * @brief 获取所有Fn键键值
 * 
 * @param offset 按键偏移量
 */
static void get_all_fns(uint8_t offset)
{
#if defined(KEYMAP_STORAGE) && !defined(ACTIONMAP_ENABLE)
    response_storage(STORAGE_FN, offset, MAX_HID_PACKET_SIZE);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 获取指定配置项目的值
 * 
 * @param offset 
 * @param len 
 */
static void get_single_config(uint8_t id)
{
#ifdef CONFIG_STORAGE
    struct hid_config_section* item = hid_config_get(id);
    if (item == 0) {
        hid_response_generic(HID_RESP_PARAMETER_ERROR);
    } else {
        hid_response(HID_CMD_GENERIC, item->section->len, item->section->data);
    }
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 获取所有宏的值
 * 
 * @param offset 
 */
static void get_all_macro(uint16_t offset)
{
#ifdef MACRO_STORAGE
    response_storage(STORAGE_MACRO, offset, MAX_HID_PACKET_SIZE);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 将数据写入存储并返回hid应答。
 * 
 * 若数据写入大小小于要写入的大小，则返回HID_RESP_WRITE_OVERFLOW
 * 
 * @param type 写入类型
 * @param offset 写入偏移
 * @param data 数据指针
 * @param len 数据长度
 */
static void set_storage(enum storage_type type, uint16_t offset, uint8_t* data, uint16_t len)
{
    if (storage_write_data(type, offset, len, data) < len)
        hid_response_generic(HID_RESP_WRITE_OVERFLOW);
    else
        hid_response_generic(HID_RESP_SUCCESS);
}

/**
 * @brief 设置单个键的键值
 * 
 * @param layer 层数
 * @param row 行
 * @param col 列
 * @param keycode 键值 
 */
static void set_single_key(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode)
{
#ifdef KEYMAP_STORAGE
    uint16_t index = layer * KEYMAP_LAYER_SIZE + row * KEYMAP_ROW_SIZE + col * SINGLE_KEY_SIZE;
    uint8_t data[2] = { UINT16_SEQ(keycode) };
    set_storage(STORAGE_KEYMAP, index, data, SINGLE_KEY_SIZE);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 设置单个FN的键值
 * 
 * @param id FN的ID
 * @param keycode 键值
 */
static void set_single_fn(uint8_t id, uint16_t keycode)
{
#if defined(KEYMAP_STORAGE) && !defined(ACTIONMAP_ENABLE)
    uint8_t data[2] = { UINT16_SEQ(keycode) };
    set_storage(STORAGE_FN, id * 2, data, 2);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 设置所有键值
 * 
 * @param id 分包ID
 * @param len 长度
 * @param data 数据
 */
static void set_all_keys(uint8_t id, uint8_t len, uint8_t* data)
{
#ifdef KEYMAP_STORAGE
    set_storage(STORAGE_KEYMAP, id * MAX_HID_PACKET_SIZE, data, len);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 设置所有FN数据
 * 
 * @param id 分包ID
 * @param len 长度
 * @param data 数据
 */
static void set_all_fns(uint8_t id, uint8_t len, uint8_t* data)
{
#if defined(KEYMAP_STORAGE) && !defined(ACTIONMAP_ENABLE)
    set_storage(STORAGE_FN, id * MAX_HID_PACKET_SIZE, data, len);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 设置单个配置数据
 * 
 * @param offset 偏移
 * @param len 长度
 * @param data 数据
 */
static void set_single_config(uint8_t id, uint8_t len, uint8_t* data)
{
#ifdef CONFIG_STORAGE
    struct hid_config_section* item = hid_config_get(id);
    if (item == 0) {
        hid_response_generic(HID_RESP_PARAMETER_ERROR);
    } else {
        if (item->section->len != len) {
            hid_response_generic(HID_RESP_WRITE_OVERFLOW);
        } else {
            memcpy(item->section->data, data, len);
            hid_response_generic(HID_RESP_SUCCESS);
        }
    }
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 设置所有宏数据
 * 
 * @param id 分包
 * @param len 长度
 * @param data 数据
 */
static void set_all_macro(uint8_t id, uint8_t len, uint8_t* data)
{
#ifdef MACRO_STORAGE
    set_storage(STORAGE_MACRO, id * MAX_HID_PACKET_SIZE, data, len);
#else
    hid_response_generic(HID_RESP_UNDEFINED);
#endif
}

/**
 * @brief 写入数据
 * 
 * @param type 数据类型
 */
static void write_data(uint8_t type)
{
    storage_write(type);
    hid_response_generic(HID_RESP_SUCCESS);
}

/**
 * @brief 读取数据（放弃当前更改）
 * 
 * @param type 读取类型
 */
static void read_data(uint8_t type)
{
    storage_read(type);
    hid_response_generic(HID_RESP_SUCCESS);
}

/**
 * @brief 重置数据
 * 
 * @param type 重置类型
 */
static void reset_data(uint8_t type)
{
    storage_delete(type);
    read_data(type);
}

/**
 * @brief HID 命令处理函数
 * 
 * @param command HID命令 
 * @param len 额外数据长度
 * @param data 额外数据
 */
void hid_on_recv(const struct host_driver* driver, uint8_t command, uint8_t len, uint8_t* data)
{
    last_driver = driver;
    current_packet_size = get_trunk_size(driver->mtu);

    switch (command) {
    case HID_CMD_GET_INFORMATION: {
        uint8_t type = 0;
        if (len == 1)
            type = data[0];

        switch (type) {
        case 0:
            send_information();
            break;
        case 1:
            send_information_sub_1();
            break;
        default:
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
            break;
        }
        break;
    }
    case HID_CMD_GET_SINGLE_KEY:
        if (len != 3)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            get_single_key(data[0], data[1], data[2]);
        break;
    case HID_CMD_GET_SINGLE_FN:
        if (len != 1)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            get_single_fn(data[0]);
        break;
    case HID_CMD_GET_ALL_KEYS:
        if (len != 2)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            get_all_keys(UINT16_READ(data, 0));
        break;
    case HID_CMD_GET_ALL_FNS:
        if (len != 1)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            get_all_fns(data[0]);
        break;
    case HID_CMD_GET_SINGLE_CONFIG:
        if (len != 1)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            get_single_config(data[0]);
        break;
    case HID_CMD_GET_ALL_MACRO:
        if (len != 2)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            get_all_macro(UINT16_READ(data, 0));
        break;
    case HID_CMD_SET_SINGLE_KEY:
        if (len != 5)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            set_single_key(data[0], data[1], data[2], UINT16_READ(data, 3));
        break;
    case HID_CMD_SET_SINGLE_FN:
        if (len != 3)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            set_single_fn(data[0], UINT16_READ(data, 1));
        break;
    case HID_CMD_SET_ALL_KEYS:
        set_all_keys(data[0], len - 1, &data[1]);
        break;
    case HID_CMD_SET_ALL_FNS:
        set_all_fns(data[0], len - 1, &data[1]);
        break;
    case HID_CMD_SET_SINGLE_CONFIG:
        set_single_config(data[0], len - 1, &data[1]);
        break;
    case HID_CMD_SET_ALL_MACRO:
        set_all_macro(data[0], len - 1, &data[1]);
        break;
    case HID_CMD_READ_CONFIG:
        if (len != 1)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            read_data(data[0]);
        break;
    case HID_CMD_WRITE_CONFIG:
        if (len != 1)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            write_data(data[0]);
        break;
    case HID_CMD_RESET_CONFIG:
        if (len != 1)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            reset_data(data[0]);
        break;
    case HID_CMD_ABOUT_LAYER:
        switch (len) {
        case 0:
            send_information_layer();
            break;
        case 8:
            // 操作层，暂无实现
            break;
        default:
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
            break;
        }
        break;
    case HID_CMD_EXECUTE_ACTION_CODE:
        if (len != 2)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            action_function(&virtual_record, data[0], data[1]);
        break;
    case HID_CMD_GET_BATTERY_INFO:
        if (len != 0)
            hid_response_generic(HID_RESP_PARAMETER_ERROR);
        else
            send_information_battery();
        break;
    default:
        hid_response_generic(HID_RESP_UNDEFINED);
        break;
    }
}

#endif

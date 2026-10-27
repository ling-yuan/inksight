#ifndef INKSIGHT_BLE_LINK_H
#define INKSIGHT_BLE_LINK_H

#include <Arduino.h>

/*
 * 低功耗蓝牙协议。设备名 InkSight398。
 * 服务 6f4c0001-8a2e-4c91-b7d4-1e5a9c30f001
 *
 * 控制特征 6f4c0002-8a2e-4c91-b7d4-1e5a9c30f001
 *   手机写入，设备通知。小端整数。需要配对后才能读写。
 *   0x01 设置空闲分钟：后跟 uint16，范围 0..240，默认 5；0 表示从不休眠
 *   0x02 开始收图：后跟 uint32 长度、uint16 宽、uint16 高
 *        只接受 768x552 且长度等于宽*高/4
 *   0x03 显示图片：图像字节必须已经收齐。
 *        设备先回复一次，表示开始刷新。刷新结束后再回复一次。
 *   0x04 读取设置
 *   0x05 设置屏幕版本：后跟 uint8，0=A0，1=A1
 *
 * 休眠后蓝牙关闭，手机无法唤醒；需按设备按键再广播。
 *
 *   设备通知：
 *   0x81 结果：后跟请求码 uint8、状态 uint8
 *        状态 0 成功，1 参数错误，2 正忙，3 图像未收齐，4 图像溢出
 *   0x82 设置：后跟 uint16 空闲分钟、uint8 屏幕版本、uint16 宽、uint16 高
 *
 * 图像特征 6f4c0003-8a2e-4c91-b7d4-1e5a9c30f001
 *   手机在 0x02 之后按顺序写入原始 2bpp 图像。
 *   每字节 4 个像素，高位在前。0 黑，1 白，2 黄，3 红。第 0 行是屏幕上方。
 */

void bleNoteActivity();
void bleStart();
void bleStop();
bool bleConnected();

// 主循环调用。返回 true 时 colorBuf 已收齐，可以刷屏。
bool bleTakeShowRequest();
void bleNotifyShowDone(bool ok);

#endif

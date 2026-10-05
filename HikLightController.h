#ifndef HIKLIGHTCONTROLLER_H
#define HIKLIGHTCONTROLLER_H

#include <QSerialPort>
#include <QSerialPortInfo>
#include <QString>
#include <QDebug>

/**
 * @brief 海康光源控制器类
 *
 * 通过RS232串口控制海康光源设备
 * 串口参数：波特率19200，数据位8，停止位1，无校验，无流控
 * 支持功能：亮度设置(0-255)、开关控制、状态查询
 */
class HikLightController
{
public:
    HikLightController();
    ~HikLightController();

    // ==================== 串口控制 ====================

    /**
     * @brief 初始化并打开串口
     * @param portName 串口名称(如"COM3")
     * @return true-成功, false-失败
     */
    bool initSerial(const QString &portName);

    /**
     * @brief 关闭串口
     */
    void closeSerial();

    /**
     * @brief 检查串口是否打开
     * @return true-已打开, false-未打开
     */
    bool isSerialOpen() const;

    // ==================== 光源控制 ====================

    /**
     * @brief 设置光源亮度
     * @param brightness 亮度值(0-255)
     * @return true-设置成功, false-设置失败
     */
    bool setLightBrightness(int brightness);

    /**
     * @brief 打开光源(常亮模式)
     * @return true-成功, false-失败
     */
    bool setLightOn();

    /**
     * @brief 关闭光源(常灭模式)
     * @return true-成功, false-失败
     */
    bool setLightOff();

    // ==================== 状态查询 ====================

    /**
     * @brief 获取当前缓存的亮度值
     * @return 当前亮度(0-255)
     */
    int getCurrentBrightness() const;

    /**
     * @brief 从设备查询光源亮度
     * @return 查询到的亮度值(0-255)
     */
    int queryLightBrightness();

    /**
     * @brief 获取当前光源开关状态
     * @return true-开启, false-关闭
     */
    bool isLightOn() const;

private:
    QSerialPort *m_serialPort;      // 串口对象
    int m_currentBrightness;        // 当前亮度缓存
    bool m_isLightOn;               // 当前开关状态缓存

    // 内部工具函数
    bool sendCommand(const QString &command);
    QString waitForResponse(int timeoutMs = 1000);
    void parseResponse(const QString &response);
};

#endif // HIKLIGHTCONTROLLER_H


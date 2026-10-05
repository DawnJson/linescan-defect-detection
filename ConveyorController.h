#ifndef CONVEYORCONTROLLER_H
#define CONVEYORCONTROLLER_H

#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QByteArray>
#include <QTimer>

/**
 * @brief 传送带控制器类
 *
 * 通过Modbus ASCII协议控制台达PLC传送带
 * 串口参数：波特率9600，数据位7，停止位1，偶校验，无流控
 * 支持功能：正转、反转、停止
 */
class ConveyorController : public QObject
{
    Q_OBJECT

public:
    explicit ConveyorController(QObject *parent = nullptr);
    ~ConveyorController();

    // ==================== 串口控制 ====================

    /**
     * @brief 初始化并打开串口
     * @param portName 串口名称(如"COM3")
     * @param baudRate 波特率(默认9600)
     * @return true-成功, false-失败
     */
    bool initSerial(const QString &portName, int baudRate = 9600);

    /**
     * @brief 关闭串口
     */
    void closeSerial();

    /**
     * @brief 检查串口是否打开
     * @return true-已打开, false-未打开
     */
    bool isSerialOpen() const;

    // ==================== 传送带控制 ====================

    /**
     * @brief 控制传送带正转
     * @return true-成功, false-失败
     */
    bool setForward();

    /**
     * @brief 控制传送带反转
     * @return true-成功, false-失败
     */
    bool setReverse();

    /**
     * @brief 停止传送带
     * @return true-成功, false-失败
     */
    bool setStop();

signals:
    /**
     * @brief 状态更新信号
     * @param status 当前状态字符串
     */
    void statusUpdated(const QString &status);

    /**
     * @brief 串口因错误(如线缆被拔出)被关闭
     *
     * 仅在错误导致的关闭时发出，主动调用closeSerial()不会触发。
     */
    void portClosed();

private slots:
    // 串口数据读取槽函数（仅丢弃回包，防止输入缓冲区无限增长）
    void onSerialReadyRead();

    // 串口错误处理槽函数
    void onSerialError(QSerialPort::SerialPortError error);

    // 延时步骤到期槽函数
    void onPendingStepTimeout();

private:
    QSerialPort *m_serialPort;      // 串口对象
    QTimer *m_pendingTimer;         // 单次定时器：持有尚未执行的第二步写线圈
    int m_pendingAddress;           // 待写入(置ON)的线圈地址
    QString m_pendingStatus;        // 第二步写入成功后上报的状态

    // PLC 的 Modbus 从站 ID (站号)
    static constexpr int PLC_SLAVE_ID = 1;

    // 台达 DVP-48EH Modbus ASCII 地址映射
    static constexpr int ADDR_M1_FORWARD = 2049;  // M1 - 正转
    static constexpr int ADDR_M2_REVERSE = 2050;  // M2 - 反转

    // ==================== 内部工具函数 ====================

    /**
     * @brief 构建写单个线圈(功能码0x05)的Modbus ASCII帧
     * @param address 线圈地址
     * @param value 写入值(true-ON, false-OFF)
     * @return 完整的ASCII帧
     */
    QByteArray buildAsciiFrame(quint16 address, bool value);

    /**
     * @brief 计算LRC校验码
     * @param data 待校验数据
     * @return LRC校验码(2字节十六进制字符串)
     */
    QByteArray calculateLRC(const QByteArray &data);

    /**
     * @brief 写线圈
     * @param address 线圈地址
     * @param value 写入值(true-ON, false-OFF)
     * @return true-发送成功, false-发送失败
     */
    bool writeCoil(int address, bool value);

    /**
     * @brief 取消尚未执行的延时步骤
     */
    void cancelPendingStep();

    /**
     * @brief 先释放互锁线圈，延时后再置位运行线圈
     * @param releaseAddress 需要先置OFF的线圈地址
     * @param runAddress 延时后置ON的线圈地址
     * @param status 第二步写入成功后上报的状态
     * @return true-第一步发送成功(第二步已排队), false-失败
     */
    bool startMotion(int releaseAddress, int runAddress, const QString &status);

    /**
     * @brief 更新状态
     * @param status 新状态
     */
    void updateStatus(const QString &status);
};

#endif // CONVEYORCONTROLLER_H

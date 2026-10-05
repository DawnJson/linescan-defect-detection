#include "ConveyorController.h"
#include <QDebug>

/**
 * @brief 构造函数
 * 初始化串口对象和状态变量
 */
ConveyorController::ConveyorController(QObject *parent)
    : QObject(parent)
    , m_serialPort(nullptr)
    , m_pendingTimer(nullptr)
    , m_pendingAddress(0)
{
    m_serialPort = new QSerialPort(this);

    // 单次定时器：保存互锁释放之后待执行的第二步
    m_pendingTimer = new QTimer(this);
    m_pendingTimer->setSingleShot(true);
    m_pendingTimer->setInterval(100);   // 延时100ms确保PLC处理完成
    connect(m_pendingTimer, &QTimer::timeout, this, &ConveyorController::onPendingStepTimeout);

    // 连接串口信号
    connect(m_serialPort, &QSerialPort::readyRead, this, &ConveyorController::onSerialReadyRead);
    connect(m_serialPort, &QSerialPort::errorOccurred, this, &ConveyorController::onSerialError);
}

/**
 * @brief 析构函数
 * 关闭串口并释放资源
 */
ConveyorController::~ConveyorController()
{
    closeSerial();
}

// ==================== 串口控制 ====================

/**
 * @brief 初始化并打开串口
 * 配置串口参数并打开串口连接（Modbus ASCII协议要求）
 */
bool ConveyorController::initSerial(const QString &portName, int baudRate)
{
    if (m_serialPort->isOpen())
    {
        qDebug() << "ConveyorController: 串口已打开";
        return false;
    }

    // 配置串口参数（Modbus ASCII协议标准配置）
    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(baudRate);
    m_serialPort->setDataBits(QSerialPort::Data7);         // ASCII模式使用7位数据
    m_serialPort->setParity(QSerialPort::EvenParity);      // 偶校验
    m_serialPort->setStopBits(QSerialPort::OneStop);       // 1个停止位
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl); // 无流控

    // 尝试打开串口
    if (m_serialPort->open(QIODevice::ReadWrite))
    {
        updateStatus(QString("已连接到 %1 (%2 baud)").arg(portName).arg(baudRate));
        qDebug() << "ConveyorController: 串口已连接" << portName;
        return true;
    }

    qDebug() << "ConveyorController: 串口打开失败:" << m_serialPort->errorString();
    updateStatus("未连接");
    return false;
}

void ConveyorController::closeSerial()
{
    cancelPendingStep();

    if (m_serialPort && m_serialPort->isOpen())
    {
        m_serialPort->close();
        updateStatus("未连接");
    }
}

bool ConveyorController::isSerialOpen() const
{
    return m_serialPort && m_serialPort->isOpen();
}

// ==================== 传送带控制 ====================

bool ConveyorController::setForward()
{
    // 根据梯形图逻辑：需要 X1(正转) 有效，且 X2(反转) 必须为 OFF
    // 先释放反转按钮（互锁），再触发正转
    return startMotion(ADDR_M2_REVERSE, ADDR_M1_FORWARD, "正转");
}

bool ConveyorController::setReverse()
{
    // 根据梯形图逻辑：需要 X2(反转) 有效，且 X1(正转) 必须为 OFF
    // 先释放正转按钮（互锁），再触发反转
    return startMotion(ADDR_M1_FORWARD, ADDR_M2_REVERSE, "反转");
}

bool ConveyorController::setStop()
{
    // 任何新命令都必须先取消尚未执行的延时步骤，避免其覆盖停止
    cancelPendingStep();

    if (!isSerialOpen())
    {
        qDebug() << "ConveyorController: 设备未连接";
        return false;
    }

    // 根据梯形图逻辑：释放正转和反转按钮，电机停止
    bool success = true;
    success &= writeCoil(ADDR_M1_FORWARD, false);  // 释放正转 M1
    success &= writeCoil(ADDR_M2_REVERSE, false);  // 释放反转 M2

    if (success)
    {
        updateStatus("停止");
    }
    else
    {
        qDebug() << "ConveyorController: 停止命令发送失败";
    }

    return success;
}

// ==================== 内部工具函数 ====================

QByteArray ConveyorController::buildAsciiFrame(quint16 address, bool value)
{
    QByteArray data;

    // 从站地址
    data.append(QByteArray::number(PLC_SLAVE_ID, 16).rightJustified(2, '0').toUpper());
    // 功能码 0x05：写单个线圈
    data.append("05");
    // 线圈地址 (4个十六进制字符)
    data.append(QByteArray::number(address, 16).rightJustified(4, '0').toUpper());
    // 值: FF00=ON, 0000=OFF
    data.append(value ? "FF00" : "0000");

    // 计算LRC校验
    data.append(calculateLRC(data));

    // 构建完整ASCII帧: : + 数据 + CR LF
    QByteArray frame;
    frame.append(':');
    frame.append(data);
    frame.append("\r\n");

    return frame;
}

QByteArray ConveyorController::calculateLRC(const QByteArray &data)
{
    quint8 lrc = 0;

    // 将每对十六进制字符转换为字节并累加
    for (int i = 0; i < data.length(); i += 2)
    {
        bool ok;
        quint8 byte = data.mid(i, 2).toUInt(&ok, 16);
        if (ok)
        {
            lrc += byte;
        }
    }

    // 取补码
    lrc = (~lrc) + 1;

    return QByteArray::number(lrc, 16).rightJustified(2, '0').toUpper();
}

bool ConveyorController::writeCoil(int address, bool value)
{
    if (!m_serialPort || !m_serialPort->isOpen())
    {
        qDebug() << "ConveyorController: 写入失败，设备未连接";
        return false;
    }

    QByteArray frame = buildAsciiFrame(address, value);

    if (m_serialPort->write(frame) == -1)
    {
        qDebug() << "ConveyorController: 发送失败:" << m_serialPort->errorString();
        return false;
    }

    return true;
}

bool ConveyorController::startMotion(int releaseAddress, int runAddress, const QString &status)
{
    // 任何新命令都必须先取消尚未执行的延时步骤
    cancelPendingStep();

    if (!isSerialOpen())
    {
        qDebug() << "ConveyorController: 设备未连接";
        return false;
    }

    if (!writeCoil(releaseAddress, false))
    {
        qDebug() << "ConveyorController: 释放互锁信号失败";
        return false;
    }

    m_pendingAddress = runAddress;
    m_pendingStatus = status;
    m_pendingTimer->start();

    return true;
}

void ConveyorController::cancelPendingStep()
{
    m_pendingTimer->stop();
}

void ConveyorController::updateStatus(const QString &status)
{
    emit statusUpdated(status);
}

// ==================== 槽函数 ====================

void ConveyorController::onPendingStepTimeout()
{
    if (writeCoil(m_pendingAddress, true))
    {
        updateStatus(m_pendingStatus);
    }
    else
    {
        qDebug() << "ConveyorController:" << m_pendingStatus << "命令发送失败";
    }
}

void ConveyorController::onSerialReadyRead()
{
    // 不解析PLC回包，仅丢弃，防止输入缓冲区无限增长
    m_serialPort->readAll();
}

void ConveyorController::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
    {
        return;
    }

    qDebug() << "ConveyorController: 串口错误:" << m_serialPort->errorString();

    if (error == QSerialPort::ResourceError)
    {
        // 资源错误（如串口被拔出），自动断开连接并通知外部
        const bool wasOpen = m_serialPort->isOpen();
        closeSerial();
        if (wasOpen)
        {
            emit portClosed();
        }
    }
}

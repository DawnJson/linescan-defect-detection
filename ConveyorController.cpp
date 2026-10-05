#include "ConveyorController.h"
#include <QDateTime>
#include <QTimer>

/**
 * @brief 构造函数
 * 初始化串口对象和状态变量
 */
ConveyorController::ConveyorController(QObject *parent)
    : QObject(parent)
    , m_serialPort(nullptr)
    , m_currentStatus("未连接")
{
    m_serialPort = new QSerialPort(this);

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
    if (!isSerialOpen())
    {
        qDebug() << "ConveyorController: 设备未连接";
        return false;
    }

    // 根据梯形图逻辑：需要 X1(正转) 有效，且 X2(反转) 必须为 OFF
    // 先释放反转按钮（互锁），再触发正转
    if (!writeCoil(ADDR_M2_REVERSE, false))
    {
        qDebug() << "ConveyorController: 释放反转信号失败";
        return false;
    }

    // 延时100ms确保PLC处理完成
    QTimer::singleShot(100, this, [this]() {
        if (writeCoil(ADDR_M1_FORWARD, true))
        {
            updateStatus("正转");
        }
        else
        {
            qDebug() << "ConveyorController: 正转命令发送失败";
        }
    });

    return true;
}

bool ConveyorController::setReverse()
{
    if (!isSerialOpen())
    {
        qDebug() << "ConveyorController: 设备未连接";
        return false;
    }

    // 根据梯形图逻辑：需要 X2(反转) 有效，且 X1(正转) 必须为 OFF
    // 先释放正转按钮（互锁），再触发反转
    if (!writeCoil(ADDR_M1_FORWARD, false))
    {
        qDebug() << "ConveyorController: 释放正转信号失败";
        return false;
    }

    // 延时100ms确保PLC处理完成
    QTimer::singleShot(100, this, [this]() {
        if (writeCoil(ADDR_M2_REVERSE, true))
        {
            updateStatus("反转");
        }
        else
        {
            qDebug() << "ConveyorController: 反转命令发送失败";
        }
    });

    return true;
}

bool ConveyorController::setStop()
{
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

QString ConveyorController::getCurrentStatus() const
{
    return m_currentStatus;
}

// ==================== 内部工具函数 ====================

QByteArray ConveyorController::buildAsciiFrame(quint8 functionCode, quint16 address, quint16 value)
{
    QByteArray data;

    // 添加从站地址
    data.append(QByteArray::number(PLC_SLAVE_ID, 16).rightJustified(2, '0').toUpper());
    // 添加功能码
    data.append(QByteArray::number(functionCode, 16).rightJustified(2, '0').toUpper());

    if (functionCode == 0x05) // 写单个线圈
    {
        // 添加地址 (4个十六进制字符)
        data.append(QByteArray::number(address, 16).rightJustified(4, '0').toUpper());
        // 添加值: FF00=ON, 0000=OFF
        data.append(value ? "FF00" : "0000");
    }

    // 计算LRC校验
    QByteArray lrc = calculateLRC(data);
    data.append(lrc);

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

bool ConveyorController::parseAsciiResponse(const QByteArray &response, quint8 &slaveAddr, quint8 &functionCode, QByteArray &data)
{
    // 检查帧格式: 必须以':'开始，以CR LF结束
    if (!response.startsWith(':') || !response.endsWith("\r\n"))
    {
        qDebug() << "ConveyorController: 无效的帧格式";
        return false;
    }

    // 提取数据部分 (去掉':'和CR LF)
    QByteArray frameData = response.mid(1, response.length() - 3);

    if (frameData.length() < 4) // 至少需要地址+功能码+LRC
    {
        qDebug() << "ConveyorController: 帧长度过短";
        return false;
    }

    // 提取LRC校验
    QByteArray receivedLrc = frameData.right(2);
    QByteArray dataWithoutLrc = frameData.left(frameData.length() - 2);

    // 验证LRC
    QByteArray calculatedLrc = calculateLRC(dataWithoutLrc);
    if (receivedLrc != calculatedLrc)
    {
        qDebug() << "ConveyorController: LRC校验失败";
        return false;
    }

    // 解析从站地址和功能码
    bool ok;
    slaveAddr = dataWithoutLrc.left(2).toUInt(&ok, 16);
    if (!ok) return false;

    functionCode = dataWithoutLrc.mid(2, 2).toUInt(&ok, 16);
    if (!ok) return false;

    // 提取数据部分
    data = dataWithoutLrc.mid(4);

    return true;
}

void ConveyorController::processResponse(const QByteArray &response)
{
    quint8 slaveAddr, functionCode;
    QByteArray data;

    if (!parseAsciiResponse(response, slaveAddr, functionCode, data))
    {
        qDebug() << "ConveyorController: 响应解析失败";
        return;
    }

    if (slaveAddr != PLC_SLAVE_ID)
    {
        qDebug() << "ConveyorController: 从站地址不匹配:" << slaveAddr;
        return;
    }

    // 0x05 写线圈响应 - 正常，不输出日志
    if (functionCode != 0x05)
    {
        qDebug() << "ConveyorController: 未处理的功能码:" << functionCode;
    }
}

bool ConveyorController::writeCoil(int address, bool value)
{
    if (!m_serialPort || !m_serialPort->isOpen())
    {
        qDebug() << "ConveyorController: 写入失败，设备未连接";
        return false;
    }

    QByteArray frame = buildAsciiFrame(0x05, address, value ? 0xFF00 : 0x0000);

    if (m_serialPort->write(frame) == -1)
    {
        qDebug() << "ConveyorController: 发送失败:" << m_serialPort->errorString();
        return false;
    }

    return true;
}

void ConveyorController::updateStatus(const QString &status)
{
    m_currentStatus = status;
    emit statusUpdated(status);
}

// ==================== 槽函数 ====================

void ConveyorController::onSerialReadyRead()
{
    m_receiveBuffer.append(m_serialPort->readAll());

    // 查找完整的帧 (以CR LF结束)
    int endIndex = m_receiveBuffer.indexOf("\r\n");
    while (endIndex != -1)
    {
        QByteArray completeFrame = m_receiveBuffer.left(endIndex + 2);
        m_receiveBuffer = m_receiveBuffer.mid(endIndex + 2);

        processResponse(completeFrame);

        // 查找下一个完整帧
        endIndex = m_receiveBuffer.indexOf("\r\n");
    }
}

void ConveyorController::onSerialError(QSerialPort::SerialPortError error)
{
    if (error != QSerialPort::NoError)
    {
        qDebug() << "串口错误";

        if (error == QSerialPort::ResourceError)
        {
            // 资源错误（如串口被拔出），自动断开连接
            closeSerial();
        }
    }
}
